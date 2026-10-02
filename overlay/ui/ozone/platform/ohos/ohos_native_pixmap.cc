// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/ohos/ohos_native_pixmap.h"

#include <linux/dma-buf.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "ui/gfx/client_native_pixmap.h"
#include "ui/gfx/color_space.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/scoped_binders.h"
#include "ui/gl/scoped_egl_image.h"
#include "ui/ozone/public/native_pixmap_gl_binding.h"

namespace ui {
namespace {

// Experiment, while 2160p60 frames take 64 ms of CPU to copy in: usage bits
// added to a video buffer's (hex, --ohos-native-buffer-usage=0x10000), and
// how a mapping is synced for the CPU (--ohos-native-buffer-sync=rw, write
// or none).
uint64_t ExtraVideoUsage() {
  static const uint64_t extra = [] {
    uint64_t value = 0;
    base::HexStringToUInt64(
        base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
            "ohos-native-buffer-usage"),
        &value);
    return value;
  }();
  return extra;
}

std::string VideoSyncMode() {
  static const base::NoDestructor<std::string> mode(
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          "ohos-native-buffer-sync"));
  return *mode;
}

// HarmonyOS's EGL target for an OHNativeWindowBuffer
// (ohos-angle-native-buffer-image.patch passes it through ANGLE).
constexpr EGLenum kEglNativeBufferOhos = 0x34E1;

// How long a buffer stays findable after its last pixmap is gone; see the
// header.
constexpr base::TimeDelta kUnusedBufferGrace = base::Seconds(10);

// Video asks for its YUV formats with the external sampler preferred, and
// that is part of a format's identity: compare without it.
viz::SharedImageFormat WithoutExternalSampler(viz::SharedImageFormat format) {
  if (format.is_multi_plane() && format.PrefersExternalSampler()) {
    format.ClearPrefersExternalSampler();
  }
  return format;
}

std::optional<int32_t> OhosFormatFor(viz::SharedImageFormat format) {
  format = WithoutExternalSampler(format);
  if (format == viz::MultiPlaneFormat::kNV12) {
    return NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP;
  }
  if (format == viz::MultiPlaneFormat::kP010) {
    return NATIVEBUFFER_PIXEL_FMT_YCBCR_P010;
  }
  // WebGPU's textures (see IsOhosNativePixmapFormat).
  if (format == viz::SinglePlaneFormat::kRGBA_8888) {
    return NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
  }
  if (format == viz::SinglePlaneFormat::kBGRA_8888) {
    return NATIVEBUFFER_PIXEL_FMT_BGRA_8888;
  }
  return std::nullopt;
}

struct PlaneLayout {
  uint32_t stride = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
};

// One allocated OH_NativeBuffer, with what is needed to find and use it again.
struct BufferEntry {
  OH_NativeBuffer* buffer = nullptr;
  OHNativeWindowBuffer* window_buffer = nullptr;
  base::ScopedFD fd;
  // The dma-buf's size; the last bytes hold its tag, see BufferRegistry.
  size_t size = 0;
  std::vector<PlaneLayout> planes;
  int users = 0;
  base::TimeTicks unused_since;
};

// Marks a buffer as one of this registry's, with its id, in the padding
// after its last plane.
struct BufferTag {
  uint64_t magic;
  uint64_t key;
};
constexpr uint64_t kBufferTagMagic = 0x58504f484f534f52;  // "ROSOHOPX"

// Where a buffer of `buffer_size` bytes keeps its tag, if its planes, ending
// at `data_end`, leave room for one.
std::optional<size_t> TagOffset(size_t buffer_size, size_t data_end) {
  if (buffer_size < data_end + sizeof(BufferTag)) {
    return std::nullopt;
  }
  return buffer_size - sizeof(BufferTag);
}

// Reads the tag at `offset` of the dma-buf `fd` through a mapping of the
// page that holds it.
std::optional<BufferTag> ReadTag(int fd, size_t offset) {
  const size_t page = static_cast<size_t>(getpagesize());
  const size_t page_start = offset & ~(page - 1);
  const size_t length = offset - page_start + sizeof(BufferTag);
  void* mapping = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd,
                       static_cast<off_t>(page_start));
  if (mapping == MAP_FAILED) {
    return std::nullopt;
  }
  BufferTag tag;
  memcpy(&tag, static_cast<uint8_t*>(mapping) + (offset - page_start),
         sizeof(tag));
  munmap(mapping, length);
  return tag;
}

void ReleaseEntry(BufferEntry& entry) {
  if (entry.window_buffer) {
    OH_NativeWindow_DestroyNativeWindowBuffer(entry.window_buffer);
  }
  if (entry.buffer) {
    OH_NativeBuffer_Unreference(entry.buffer);
  }
  entry.window_buffer = nullptr;
  entry.buffer = nullptr;
}

// The buffers this process allocated, by an id of their own. A dma-buf fd
// cannot be traced back to one by its inode, which HarmonyOS reports as 0
// for every dma-buf, nor by kcmp, which the app sandbox kills a process for,
// nor by a DMA_BUF_SET_NAME name, which its fdinfo does not show. So each
// buffer carries its id in the padding after its last plane, which nothing
// else writes, and a handle is resolved by reading it back.
class BufferRegistry {
 public:
  static BufferRegistry& Get() {
    static base::NoDestructor<BufferRegistry> registry;
    return *registry;
  }

  // Allocates a buffer and registers it with one user. Returns its key.
  std::optional<uint64_t> Allocate(gfx::Size size, viz::SharedImageFormat format) {
    const std::optional<int32_t> ohos_format = OhosFormatFor(format);
    if (!ohos_format || size.IsEmpty()) {
      LOG(ERROR) << "OHOS native pixmap: not allocating " << format.ToString()
                 << " " << size.ToString();
      return std::nullopt;
    }
    const std::tuple<int32_t, int, int> shape(*ohos_format, size.width(),
                                              size.height());
    {
      base::AutoLock hold(lock_);
      if (untaggable_.contains(shape)) {
        return std::nullopt;
      }
    }
    OH_NativeBuffer_Config config = {};
    config.width = size.width();
    config.height = size.height();
    config.format = *ohos_format;
    const bool yuv = format.is_multi_plane();
    // Video frames are written by the CPU and sampled; WebGPU's textures are
    // rendered to as well, and never touched by the CPU.
    config.usage = yuv ? NATIVEBUFFER_USAGE_CPU_READ |
                             NATIVEBUFFER_USAGE_CPU_WRITE |
                             NATIVEBUFFER_USAGE_MEM_DMA |
                             NATIVEBUFFER_USAGE_HW_TEXTURE
                       : NATIVEBUFFER_USAGE_MEM_DMA |
                             NATIVEBUFFER_USAGE_HW_TEXTURE |
                             NATIVEBUFFER_USAGE_HW_RENDER;
    if (yuv && ExtraVideoUsage()) {
      config.usage |= ExtraVideoUsage();
    }
    OH_NativeBuffer* buffer = OH_NativeBuffer_Alloc(&config);
    if (!buffer && yuv && ExtraVideoUsage()) {
      LOG(WARNING) << "OHOS native pixmap: usage 0x" << std::hex
                   << config.usage << " refused, trying without the extra";
      config.usage &= ~ExtraVideoUsage();
      buffer = OH_NativeBuffer_Alloc(&config);
    }
    if (!buffer) {
      LOG(ERROR) << "OHOS native pixmap: could not allocate "
                 << size.ToString() << " " << format.ToString();
      return std::nullopt;
    }

    auto entry = std::make_unique<BufferEntry>();
    entry->buffer = buffer;

    // Where each plane sits. MapPlanes reports a plane's row pitch as its
    // column stride (for a 1920-wide P010 luma plane: row 2, column 3840).
    // A YUV buffer stays mapped until its tag is written below.
    void* address = nullptr;
    if (yuv) {
      OH_NativeBuffer_Planes planes = {};
      if (OH_NativeBuffer_MapPlanes(buffer, &address, &planes) != 0 ||
          planes.planeCount < 2) {
        LOG(ERROR) << "OHOS native pixmap: no plane layout";
        ReleaseEntry(*entry);
        return std::nullopt;
      }
      const uint64_t rows[] = {static_cast<uint64_t>(size.height()),
                               static_cast<uint64_t>((size.height() + 1) / 2)};
      for (size_t i = 0; i < 2; ++i) {
        entry->planes.push_back({planes.planes[i].columnStride,
                                 planes.planes[i].offset,
                                 planes.planes[i].columnStride * rows[i]});
      }
    } else {
      OH_NativeBuffer_Config allocated = {};
      OH_NativeBuffer_GetConfig(buffer, &allocated);
      const uint32_t stride = static_cast<uint32_t>(allocated.stride);
      entry->planes.push_back(
          {stride, 0, static_cast<uint64_t>(stride) * size.height()});
    }
    const auto unmap = [&] {
      if (address) {
        OH_NativeBuffer_Unmap(buffer);
        address = nullptr;
      }
    };

    entry->window_buffer =
        OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(buffer);
    BufferHandle* handle =
        entry->window_buffer
            ? OH_NativeWindow_GetBufferHandleFromNative(entry->window_buffer)
            : nullptr;
    if (!handle || handle->fd < 0) {
      LOG(ERROR) << "OHOS native pixmap: no dma-buf fd";
      unmap();
      ReleaseEntry(*entry);
      return std::nullopt;
    }
    entry->fd.reset(HANDLE_EINTR(dup(handle->fd)));
    if (!entry->fd.is_valid()) {
      PLOG(ERROR) << "OHOS native pixmap: dup";
      unmap();
      ReleaseEntry(*entry);
      return std::nullopt;
    }
    entry->size = static_cast<size_t>(handle->size);
    uint64_t key;
    {
      base::AutoLock hold(lock_);
      key = ++last_key_;
    }
    if (yuv) {
      const PlaneLayout& last_plane = entry->planes.back();
      const std::optional<size_t> tag_offset =
          TagOffset(entry->size, last_plane.offset + last_plane.size);
      if (!tag_offset) {
        // Remembered, so that a video of this size goes back to uploading
        // planes at once rather than allocating a buffer for every frame.
        LOG(ERROR) << "OHOS native pixmap: no room to tag a "
                   << format.ToString() << " " << size.ToString()
                   << " buffer of " << entry->size << " bytes; not offered again";
        {
          base::AutoLock hold(lock_);
          untaggable_.insert(shape);
        }
        unmap();
        ReleaseEntry(*entry);
        return std::nullopt;
      }
      const BufferTag tag = {kBufferTagMagic, key};
      memcpy(static_cast<uint8_t*>(address) + *tag_offset, &tag, sizeof(tag));
      unmap();
    }
    entry->users = 1;

    static bool logged[2] = {false, false};
    bool& logged_format =
        logged[*ohos_format == NATIVEBUFFER_PIXEL_FMT_YCBCR_P010 ? 1 : 0];
    if (!logged_format) {
      logged_format = true;
      std::string layout;
      for (const PlaneLayout& plane : entry->planes) {
        layout += base::NumberToString(plane.stride) + "@" +
                  base::NumberToString(plane.offset) + " ";
      }
      LOG(WARNING) << "OHOS native pixmap: " << format.ToString() << " "
                   << size.ToString() << " planes " << layout << "buffer "
                   << handle->size << " usage 0x" << std::hex << config.usage;
    }

    base::AutoLock hold(lock_);
    PurgeUnused();
    entries_[key] = std::move(entry);
    return key;
  }

  // Takes a user on the buffer `fd` belongs to, if this process has it.
  std::optional<uint64_t> Acquire(int fd) {
    const off_t end = lseek(fd, 0, SEEK_END);
    base::AutoLock hold(lock_);
    for (auto& [key, entry] : entries_) {
      if (end > 0 && entry->size != static_cast<size_t>(end)) {
        continue;
      }
      const std::optional<BufferTag> tag =
          ReadTag(fd, entry->size - sizeof(BufferTag));
      if (tag && tag->magic == kBufferTagMagic && tag->key == key) {
        ++entry->users;
        return key;
      }
    }
    return std::nullopt;
  }

  void Unuse(uint64_t key) {
    base::AutoLock hold(lock_);
    auto it = entries_.find(key);
    if (it != entries_.end() && --it->second->users == 0) {
      it->second->unused_since = base::TimeTicks::Now();
    }
  }

  // The buffer whose fd is `fd`, as an OhosNativePixmap reports it.
  OH_NativeBuffer* BufferForFd(int fd) {
    base::AutoLock hold(lock_);
    for (auto& [key, entry] : entries_) {
      if (entry->fd.get() == fd) {
        return entry->buffer;
      }
    }
    return nullptr;
  }

  // Runs `use` on the entry for `key`, or on null, under the lock.
  template <typename Use>
  auto With(uint64_t key, Use use) {
    base::AutoLock hold(lock_);
    auto it = entries_.find(key);
    return use(it == entries_.end() ? nullptr : it->second.get());
  }

 private:
  friend class base::NoDestructor<BufferRegistry>;
  BufferRegistry() = default;

  void PurgeUnused() EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    const base::TimeTicks now = base::TimeTicks::Now();
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second->users == 0 &&
          now - it->second->unused_since > kUnusedBufferGrace) {
        ReleaseEntry(*it->second);
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }

  base::Lock lock_;
  uint64_t last_key_ GUARDED_BY(lock_) = 0;
  // Format and size of buffers with no padding to hold a tag.
  std::set<std::tuple<int32_t, int, int>> untaggable_ GUARDED_BY(lock_);
  std::map<uint64_t, std::unique_ptr<BufferEntry>> entries_ GUARDED_BY(lock_);
};

class OhosNativePixmap : public gfx::NativePixmap {
 public:
  OhosNativePixmap(uint64_t key, gfx::Size size, viz::SharedImageFormat format)
      : key_(key), size_(size), format_(format) {
    BufferRegistry::Get().With(key_, [this](BufferEntry* entry) {
      if (entry) {
        fd_ = entry->fd.get();
        planes_ = entry->planes;
      }
      return 0;
    });
  }

  uint64_t key() const { return key_; }

  bool AreDmaBufFdsValid() const override { return fd_ >= 0; }
  int GetDmaBufFd(size_t plane) const override { return fd_; }
  uint32_t GetDmaBufPitch(size_t plane) const override {
    return plane < planes_.size() ? planes_[plane].stride : 0;
  }
  size_t GetDmaBufOffset(size_t plane) const override {
    return plane < planes_.size() ? planes_[plane].offset : 0;
  }
  size_t GetDmaBufPlaneSize(size_t plane) const override {
    return plane < planes_.size() ? planes_[plane].size : 0;
  }
  size_t GetNumberOfPlanes() const override { return planes_.size(); }
  bool SupportsZeroCopyWebGPUImport() const override { return false; }
  viz::SharedImageFormat GetSharedImageFormat() const override {
    return format_;
  }
  uint64_t GetFormatModifier() const override {
    return gfx::NativePixmapHandle::kNoModifier;
  }
  gfx::Size GetBufferSize() const override { return size_; }
  uint32_t GetUniqueId() const override { return static_cast<uint32_t>(key_); }
  bool ScheduleOverlayPlane(
      gfx::AcceleratedWidget widget,
      const gfx::OverlayPlaneData& overlay_plane_data,
      std::vector<gfx::GpuFence> acquire_fences,
      std::vector<gfx::GpuFence> release_fences) override {
    return false;
  }
  gfx::NativePixmapHandle ExportHandle() const override {
    gfx::NativePixmapHandle handle;
    if (fd_ < 0) {
      return handle;
    }
    for (const PlaneLayout& plane : planes_) {
      base::ScopedFD fd(HANDLE_EINTR(dup(fd_)));
      if (!fd.is_valid()) {
        PLOG(ERROR) << "OHOS native pixmap: dup";
        return gfx::NativePixmapHandle();
      }
      handle.planes.emplace_back(plane.stride, plane.offset, plane.size,
                                 std::move(fd));
    }
    return handle;
  }

 private:
  ~OhosNativePixmap() override { BufferRegistry::Get().Unuse(key_); }

  const uint64_t key_;
  const gfx::Size size_;
  const viz::SharedImageFormat format_;
  // Owned by the registry entry, which outlives this pixmap.
  int fd_ = -1;
  std::vector<PlaneLayout> planes_;
};

// The renderer's view of a buffer: the dma-buf mapped for libyuv to write a
// frame in. Renderers are processes of their own, without the registry, so
// this maps the fd itself, as ClientNativePixmapDmaBuf does.
class OhosClientNativePixmap : public gfx::ClientNativePixmap {
 public:
  explicit OhosClientNativePixmap(gfx::NativePixmapHandle handle)
      : handle_(std::move(handle)) {}
  ~OhosClientNativePixmap() override {
    Unmap();
    if (mapping_) {
      munmap(mapping_, mapping_size_);
    }
  }

  bool Map() override {
    if (mapped_) {
      return true;
    }
    const int fd = handle_.planes[0].fd.get();
    if (!mapping_) {
      const off_t end = lseek(fd, 0, SEEK_END);
      const gfx::NativePixmapPlane& last = handle_.planes.back();
      mapping_size_ = end > 0 ? static_cast<size_t>(end)
                              : static_cast<size_t>(last.offset + last.size);
      void* address = mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
      if (address == MAP_FAILED) {
        PLOG(ERROR) << "OHOS native pixmap: mmap of " << mapping_size_
                    << " bytes";
        mapping_size_ = 0;
        return false;
      }
      mapping_ = static_cast<uint8_t*>(address);
    }
    Sync(DMA_BUF_SYNC_START);
    mapped_ = true;
    return true;
  }
  void Unmap() override {
    if (!mapped_) {
      return;
    }
    Sync(DMA_BUF_SYNC_END);
    mapped_ = false;
  }
  size_t GetNumberOfPlanes() const override { return handle_.planes.size(); }
  void* GetMemoryAddress(size_t plane) const override {
    return mapping_ && plane < handle_.planes.size()
               ? mapping_ + handle_.planes[plane].offset
               : nullptr;
  }
  int GetStride(size_t plane) const override {
    return plane < handle_.planes.size()
               ? static_cast<int>(handle_.planes[plane].stride)
               : 0;
  }
  gfx::NativePixmapHandle CloneHandleForIPC() const override {
    return gfx::CloneHandleForIPC(handle_);
  }
  uint64_t GetPlaneSize(size_t plane) const override {
    return plane < handle_.planes.size() ? handle_.planes[plane].size : 0;
  }

 private:
  // Keeps the CPU's caches and the GPU's view of the buffer in step.
  void Sync(uint64_t when) {
    const std::string mode = VideoSyncMode();
    if (mode == "none") {
      return;
    }
    struct dma_buf_sync sync = {};
    sync.flags = when | (mode == "write" ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_RW);
    if (HANDLE_EINTR(ioctl(handle_.planes[0].fd.get(), DMA_BUF_IOCTL_SYNC,
                           &sync)) != 0) {
      PLOG(WARNING) << "OHOS native pixmap: DMA_BUF_IOCTL_SYNC";
    }
  }

  gfx::NativePixmapHandle handle_;
  uint8_t* mapping_ = nullptr;
  size_t mapping_size_ = 0;
  bool mapped_ = false;
};

class OhosClientNativePixmapFactory : public gfx::ClientNativePixmapFactory {
 public:
  std::unique_ptr<gfx::ClientNativePixmap> ImportFromHandle(
      gfx::NativePixmapHandle handle,
      const gfx::Size& size,
      viz::SharedImageFormat format,
      gfx::BufferUsage usage) override {
    if (!IsOhosNativePixmapFormat(format) || handle.planes.empty()) {
      return nullptr;
    }
    for (const gfx::NativePixmapPlane& plane : handle.planes) {
      if (!plane.fd.is_valid()) {
        LOG(ERROR) << "OHOS native pixmap: client handle without an fd";
        return nullptr;
      }
    }
    return std::make_unique<OhosClientNativePixmap>(std::move(handle));
  }
};

// What the driver needs to convert this buffer's YUV: the matrix, the
// range and, for HDR, the transfer the values are in.
OH_NativeBuffer_ColorSpace OhosColorSpaceFor(const gfx::ColorSpace& space) {
  const bool full = space.GetRangeID() == gfx::ColorSpace::RangeID::FULL;
  switch (space.GetMatrixID()) {
    case gfx::ColorSpace::MatrixID::BT2020_NCL:
      if (space.GetTransferID() == gfx::ColorSpace::TransferID::HLG) {
        return full ? OH_COLORSPACE_BT2020_HLG_FULL
                    : OH_COLORSPACE_BT2020_HLG_LIMIT;
      }
      return full ? OH_COLORSPACE_BT2020_PQ_FULL
                  : OH_COLORSPACE_BT2020_PQ_LIMIT;
    case gfx::ColorSpace::MatrixID::SMPTE170M:
      return full ? OH_COLORSPACE_BT601_SMPTE_C_FULL
                  : OH_COLORSPACE_BT601_SMPTE_C_LIMIT;
    case gfx::ColorSpace::MatrixID::BT470BG:
      return full ? OH_COLORSPACE_BT601_EBU_FULL
                  : OH_COLORSPACE_BT601_EBU_LIMIT;
    default:
      return full ? OH_COLORSPACE_BT709_FULL : OH_COLORSPACE_BT709_LIMIT;
  }
}

class OhosNativePixmapGLBinding : public NativePixmapGLBinding {
 public:
  OhosNativePixmapGLBinding(scoped_refptr<gfx::NativePixmap> pixmap,
                            gl::ScopedEGLImage image)
      : pixmap_(std::move(pixmap)), image_(std::move(image)) {}

 private:
  scoped_refptr<gfx::NativePixmap> pixmap_;
  gl::ScopedEGLImage image_;
};

}  // namespace

bool IsOhosNativePixmapFormat(viz::SharedImageFormat format) {
  if (!OhosFormatFor(format).has_value()) {
    return false;
  }
  // The import is EGL_NATIVE_BUFFER_OHOS, which ANGLE's GLES backend hands
  // to the system EGL. Its Vulkan backend has nothing of the kind -- that
  // would be VK_OHOS_native_buffer -- and crashed creating the image, so
  // under it video keeps uploading planes. A process that has not set up
  // GL (kNone) answers for the GPU's sake as GLES would.
  const gl::ANGLEImplementation angle = gl::GetANGLEImplementation();
  return angle == gl::ANGLEImplementation::kNone ||
         angle == gl::ANGLEImplementation::kOpenGLES;
}

scoped_refptr<gfx::NativePixmap> CreateOhosNativePixmap(
    gfx::Size size,
    viz::SharedImageFormat format) {
  const std::optional<uint64_t> key =
      BufferRegistry::Get().Allocate(size, format);
  if (!key) {
    return nullptr;
  }
  return base::MakeRefCounted<OhosNativePixmap>(*key, size, format);
}

scoped_refptr<gfx::NativePixmap> CreateOhosNativePixmapFromHandle(
    gfx::Size size,
    viz::SharedImageFormat format,
    gfx::NativePixmapHandle handle) {
  if (!IsOhosNativePixmapFormat(format) || handle.planes.empty() ||
      !handle.planes[0].fd.is_valid()) {
    return nullptr;
  }
  const std::optional<uint64_t> key =
      BufferRegistry::Get().Acquire(handle.planes[0].fd.get());
  if (!key) {
    LOG(ERROR) << "OHOS native pixmap: handle for a buffer from elsewhere";
    return nullptr;
  }
  return base::MakeRefCounted<OhosNativePixmap>(*key, size, format);
}

std::unique_ptr<NativePixmapGLBinding> ImportOhosNativePixmap(
    scoped_refptr<gfx::NativePixmap> pixmap,
    const gfx::ColorSpace& color_space,
    unsigned int target,
    unsigned int texture_id) {
  if (!pixmap || !IsOhosNativePixmapFormat(pixmap->GetSharedImageFormat())) {
    return nullptr;
  }
  const uint64_t key = static_cast<OhosNativePixmap*>(pixmap.get())->key();
  OHNativeWindowBuffer* window_buffer = BufferRegistry::Get().With(
      key, [&color_space](BufferEntry* entry) -> OHNativeWindowBuffer* {
        if (!entry || !entry->window_buffer) {
          return nullptr;
        }
        // How the driver converts YUV; an RGB buffer has nothing to convert.
        if (entry->planes.size() > 1) {
          OH_NativeBuffer_SetColorSpace(entry->buffer,
                                        OhosColorSpaceFor(color_space));
        }
        return entry->window_buffer;
      });
  if (!window_buffer) {
    LOG(ERROR) << "OHOS native pixmap: nothing to import";
    return nullptr;
  }

  const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  gl::ScopedEGLImage image = gl::MakeScopedEGLImage(
      EGL_NO_CONTEXT, kEglNativeBufferOhos,
      static_cast<EGLClientBuffer>(window_buffer), attributes);
  if (!image.is_valid()) {
    LOG(ERROR) << "OHOS native pixmap: EGLImage import failed, 0x" << std::hex
               << eglGetError();
    return nullptr;
  }
  {
    gl::ScopedTextureBinder binder(target, texture_id);
    glEGLImageTargetTexture2DOES(target, image.get());
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    LOG(WARNING) << "OHOS native pixmap: imported "
                 << pixmap->GetSharedImageFormat().ToString() << " "
                 << pixmap->GetBufferSize().ToString() << " as "
                 << color_space.ToString();
  }
  return std::make_unique<OhosNativePixmapGLBinding>(std::move(pixmap),
                                                     std::move(image));
}

void* GetOhosNativeBuffer(const gfx::NativePixmap& pixmap) {
  if (!pixmap.AreDmaBufFdsValid()) {
    return nullptr;
  }
  return BufferRegistry::Get().BufferForFd(pixmap.GetDmaBufFd(0));
}

std::unique_ptr<gfx::ClientNativePixmapFactory>
CreateOhosClientNativePixmapFactory() {
  return std::make_unique<OhosClientNativePixmapFactory>();
}

}  // namespace ui
