// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/ohos/ohos_native_pixmap.h"

#include <IPCKit/ipc_cparcel.h>
#include <dirent.h>
#include <dlfcn.h>
#include <linux/dma-buf.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include "ui/ozone/platform/ohos/ohos_video_buffer_parcel.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory_coordinator/async_memory_consumer_registration.h"
#include "base/memory_coordinator/memory_consumer.h"
#include "base/no_destructor.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "ui/gfx/client_native_pixmap.h"
#include "ui/gfx/color_space.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_channel.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_buffer.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_implementation.h"
#include "ui/gl/gl_surface_egl.h"
#include "ui/gl/scoped_binders.h"
#include "ui/gl/scoped_egl_image.h"
#include "ui/ozone/public/native_pixmap_gl_binding.h"

namespace ui {
namespace {

// Sample only allocation churn, outside presentation and video-frame paths.
// Counts distinguish unreachable-but-not-yet-collected canvases from descriptors
// that survive their pixmaps. Never log descriptor targets or application data.
std::atomic<size_t> offscreen_pixmaps_created{0};
std::atomic<size_t> offscreen_pixmaps_live{0};

void RecordOffscreenPixmapAllocation() {
  const size_t live = offscreen_pixmaps_live.fetch_add(1) + 1;
  const size_t created = offscreen_pixmaps_created.fetch_add(1) + 1;
  if (created != 1 && created % 512 != 0)
    return;
  DIR* directory = opendir("/proc/self/fd");
  if (!directory) {
    PLOG(ERROR) << "OHOS offscreen resources: cannot sample descriptors; created="
                << created << " live=" << live;
    return;
  }
  size_t open_fds = 0;
  while (const dirent* entry = readdir(directory)) {
    if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9')
      ++open_fds;
  }
  closedir(directory);
  rlimit limits{};
  const bool have_limit = getrlimit(RLIMIT_NOFILE, &limits) == 0;
  LOG(WARNING) << "OHOS offscreen resources: created=" << created
               << " live=" << live << " open_fds=" << (open_fds ? open_fds - 1 : 0)
               << " fd_limit=" << (have_limit ? limits.rlim_cur : 0);
}

// HarmonyOS's EGL target for an OHNativeWindowBuffer
// (ohos-angle.patch passes it through ANGLE).
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
  if (format == viz::SinglePlaneFormat::kRGBA_F16) {
    return NATIVEBUFFER_PIXEL_FMT_RGBA16_FLOAT;
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
class BufferRegistry : public base::MemoryConsumer {
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
    //
    // Video frames are cached memory: without MEM_MMZ_CACHE the allocator
    // hands out uncached pages, and libyuv took 58 ms of CPU to write one
    // 2160p P010 frame in, against 19 ms with it. On a Mate 70 Pro+ that
    // took 2160p60 VP9 from 18 to 30 frames shown a second, the decoder
    // getting the cores the copy gave back.
    config.usage = yuv ? NATIVEBUFFER_USAGE_CPU_READ |
                             NATIVEBUFFER_USAGE_CPU_WRITE |
                             NATIVEBUFFER_USAGE_MEM_DMA |
                             NATIVEBUFFER_USAGE_MEM_MMZ_CACHE |
                             NATIVEBUFFER_USAGE_HW_TEXTURE
                       : NATIVEBUFFER_USAGE_MEM_DMA |
                             NATIVEBUFFER_USAGE_HW_TEXTURE |
                             NATIVEBUFFER_USAGE_HW_RENDER;
    OH_NativeBuffer* buffer = OH_NativeBuffer_Alloc(&config);
    if (!buffer && yuv) {
      // A system without cached MMZ: uncached, slower to write, but works.
      config.usage &= ~NATIVEBUFFER_USAGE_MEM_MMZ_CACHE;
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
        entry->unused_since = base::TimeTicks();
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
      SchedulePurgeLocked();
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
  BufferRegistry()
      : memory_consumer_registration_(
            "OhosNativeBufferRegistry",
            {base::MemoryConsumerTraits::EstimatedMemoryUsage::kLarge,
             base::MemoryConsumerTraits::ReleaseMemoryCost::
                 kFreesPagesWithoutTraversal,
             base::MemoryConsumerTraits::InformationRetention::kLossless,
             base::MemoryConsumerTraits::ExecutionType::kAsynchronous,
             base::MemoryConsumerTraits::SupportsMemoryLimit::kNo},
            this,
            base::MemoryConsumerRegistration::CheckUnregister::kDisabled) {}

  // This is a handle handoff grace period, not an optional image cache. Even
  // critical pressure must not evict a buffer whose handle may still be in
  // transit. Drain only expired entries, just as the delayed task does.
  void OnReleaseMemory() override {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&BufferRegistry::PurgeUnused, base::Unretained(this),
                       /*from_delayed_task=*/false));
  }
  void OnUpdateMemoryLimit() override {}

  void SchedulePurgeLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    if (purge_pending_) {
      return;
    }
    std::optional<base::TimeTicks> next_deadline;
    for (const auto& [key, entry] : entries_) {
      if (entry->users == 0) {
        const base::TimeTicks deadline = entry->unused_since + kUnusedBufferGrace;
        if (!next_deadline || deadline < *next_deadline) {
          next_deadline = deadline;
        }
      }
    }
    if (!next_deadline) {
      return;
    }
    // The singleton outlives the ThreadPool. Coalesce all idle buffers into
    // one task, and stop posting once none remain. Native handle destruction
    // may call into the allocator, so run it off the GPU thread.
    purge_pending_ = base::ThreadPool::PostDelayedTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&BufferRegistry::PurgeUnused, base::Unretained(this),
                       /*from_delayed_task=*/true),
        std::max(base::TimeDelta(), *next_deadline - base::TimeTicks::Now()));
  }

  void PurgeUnused(bool from_delayed_task) {
    std::vector<std::unique_ptr<BufferEntry>> retired;
    size_t released_bytes = 0;
    {
      base::AutoLock hold(lock_);
      if (from_delayed_task) {
        purge_pending_ = false;
      }
      const base::TimeTicks now = base::TimeTicks::Now();
      for (auto it = entries_.begin(); it != entries_.end();) {
        // Recheck under the same lock as Acquire/Unuse. Re-importing a buffer
        // protects it; releasing it again starts a fresh, full grace period.
        if (it->second->users == 0 &&
            now - it->second->unused_since >= kUnusedBufferGrace) {
          released_bytes += it->second->size;
          retired.push_back(std::move(it->second));
          it = entries_.erase(it);
        } else {
          ++it;
        }
      }
      SchedulePurgeLocked();
    }
    // Remove entries before dropping the lock, so no importer can acquire
    // one while its native handles are being destroyed. Existing users keep
    // their entries registered and never enter this list.
    for (const auto& entry : retired) {
      ReleaseEntry(*entry);
    }
    if (!retired.empty()) {
      VLOG(1) << "OHOS native pixmap: reclaimed " << retired.size()
              << " idle buffers, " << released_bytes << " bytes";
    }
  }

  base::Lock lock_;
  uint64_t last_key_ GUARDED_BY(lock_) = 0;
  // Format and size of buffers with no padding to hold a tag.
  std::set<std::tuple<int32_t, int, int>> untaggable_ GUARDED_BY(lock_);
  std::map<uint64_t, std::unique_ptr<BufferEntry>> entries_ GUARDED_BY(lock_);
  bool purge_pending_ GUARDED_BY(lock_) = false;
  base::AsyncMemoryConsumerRegistration memory_consumer_registration_;
};

OH_NativeBuffer_ColorSpace OhosColorSpaceFor(
    const gfx::ColorSpace& color_space);

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

  OhosNativePixmap(OHNativeWindowBuffer* window_buffer,
                   gfx::Size size,
                   viz::SharedImageFormat format,
                   uint32_t buffer_id,
                   base::OnceClosure release)
      : key_(0),
        size_(size),
        format_(format),
        video_buffer_id_(buffer_id),
        video_window_(window_buffer),
        video_release_(std::move(release)) {}

  OhosNativePixmap(OHNativeWindowBuffer* window_buffer,
                   gfx::Size size,
                   viz::SharedImageFormat format,
                   PlaneLayout plane,
                   uint32_t buffer_id,
                   int buffer_fd)
      : key_(0),
        size_(size),
        format_(format),
        video_window_(window_buffer),
        video_release_(base::BindOnce(
            &OH_NativeWindow_DestroyNativeWindowBuffer, window_buffer)),
        offscreen_buffer_id_(buffer_id),
        fd_(buffer_fd) {
    planes_.push_back(plane);
    RecordOffscreenPixmapAllocation();
  }

  OHNativeWindowBuffer* WindowBuffer(const gfx::ColorSpace& color_space) const {
    if (video_window_) {
      // The producer owns this native buffer's metadata. Do not mutate it.
      return video_window_;
    }
    return BufferRegistry::Get().With(
        key_, [&color_space](BufferEntry* entry) -> OHNativeWindowBuffer* {
          if (!entry || !entry->window_buffer) {
            return nullptr;
          }
          if (entry->planes.size() > 1) {
            OH_NativeBuffer_SetColorSpace(entry->buffer,
                                          OhosColorSpaceFor(color_space));
          }
          return entry->window_buffer;
        });
  }

  uint64_t key() const { return key_; }
  std::optional<uint32_t> video_buffer_id() const { return video_buffer_id_; }
  bool has_direct_buffer() const { return video_window_ != nullptr; }

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
  bool SupportsZeroCopyWebGPUImport() const override {
    return offscreen_buffer_id_.has_value();
  }
  viz::SharedImageFormat GetSharedImageFormat() const override {
    return format_;
  }
  uint64_t GetFormatModifier() const override {
    return gfx::NativePixmapHandle::kNoModifier;
  }
  gfx::Size GetBufferSize() const override { return size_; }
  uint32_t GetUniqueId() const override {
    return offscreen_buffer_id_.value_or(static_cast<uint32_t>(key_));
  }
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
    if (offscreen_buffer_id_) {
      base::AutoLock export_lock(offscreen_export_lock_);
      // The window buffer already owns the allocation. Keeping both its native
      // descriptors and the received parcel/plane duplicates multiplied FD
      // pressure for thousands of small WebGPU canvases waiting for GC.
      // Serialize only for an actual IPC export; no pixels or fences change.
      OH_NativeBuffer* native = nullptr;
      if (OH_NativeBuffer_FromNativeWindowBuffer(video_window_, &native) != 0)
        return {};
      phone_gpu::SharedMessage message;
      auto& packet = message.packet;
      packet.operation = phone_gpu::SharedOp::kImage;
      packet.sequence = packet.widget = 1;
      packet.width = size_.width();
      packet.height = size_.height();
      packet.prefix[8] = *OhosFormatFor(format_);
      if (!phone_gpu::ExportSharedImage(native, &message))
        return {};
      handle.ohos_buffer_prefix.assign(packet.prefix.begin(), packet.prefix.end());
      handle.ohos_buffer_tail.assign(packet.tail.begin(),
                                     packet.tail.begin() + packet.tail_bytes);
      for (int fd : message.fds)
        handle.ohos_buffer_fds.emplace_back(fd);
      message.fds.clear();
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
  ~OhosNativePixmap() override {
    if (offscreen_buffer_id_)
      offscreen_pixmaps_live.fetch_sub(1);
    if (key_) {
      BufferRegistry::Get().Unuse(key_);
    }
  }

  const uint64_t key_;
  const gfx::Size size_;
  const viz::SharedImageFormat format_;
  const std::optional<uint32_t> video_buffer_id_;
  // Kept alive by video_release_: either a per-frame consumer queue lease or
  // an independent allocation reference used by a cached import.
  RAW_PTR_EXCLUSION OHNativeWindowBuffer* video_window_ = nullptr;
  base::ScopedClosureRunner video_release_;
  const std::optional<uint32_t> offscreen_buffer_id_;
  // NativeBuffer parcel APIs are not thread-safe. ExportHandle can be called
  // by different GPU representations, so serialize exports of this wrapper.
  mutable base::Lock offscreen_export_lock_;
  // Borrowed from the window buffer or registry entry, both of which outlive
  // this pixmap's operations. IPC exports always duplicate ownership.
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
    struct dma_buf_sync sync = {};
    sync.flags = when | DMA_BUF_SYNC_RW;
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

// Colorimetry reported by the decoder's actual output takes precedence over
// container metadata (e.g. a platform decoder may perform color conversion).
// Unknown/absent metadata leaves the stream color space intact.
std::optional<gfx::ColorSpace> OhosVideoColorSpace(
    OH_NativeBuffer_ColorSpace space) {
  using Primary = gfx::ColorSpace::PrimaryID;
  using Transfer = gfx::ColorSpace::TransferID;
  using Matrix = gfx::ColorSpace::MatrixID;
  using Range = gfx::ColorSpace::RangeID;
  switch (space) {
    case OH_COLORSPACE_BT709_FULL:
    case OH_COLORSPACE_BT709_LIMIT:
      return gfx::ColorSpace(
          Primary::BT709, Transfer::BT709, Matrix::BT709,
          space == OH_COLORSPACE_BT709_FULL ? Range::FULL : Range::LIMITED);
    case OH_COLORSPACE_BT601_EBU_FULL:
    case OH_COLORSPACE_BT601_EBU_LIMIT:
      return gfx::ColorSpace(
          Primary::BT470BG, Transfer::BT709, Matrix::BT470BG,
          space == OH_COLORSPACE_BT601_EBU_FULL ? Range::FULL : Range::LIMITED);
    case OH_COLORSPACE_BT601_SMPTE_C_FULL:
    case OH_COLORSPACE_BT601_SMPTE_C_LIMIT:
      return gfx::ColorSpace(
          Primary::SMPTE170M, Transfer::BT709, Matrix::SMPTE170M,
          space == OH_COLORSPACE_BT601_SMPTE_C_FULL ? Range::FULL
                                                    : Range::LIMITED);
    case OH_COLORSPACE_BT2020_HLG_FULL:
    case OH_COLORSPACE_BT2020_HLG_LIMIT:
      return gfx::ColorSpace(Primary::BT2020, Transfer::HLG, Matrix::BT2020_NCL,
                             space == OH_COLORSPACE_BT2020_HLG_FULL
                                 ? Range::FULL
                                 : Range::LIMITED);
    case OH_COLORSPACE_BT2020_PQ_FULL:
    case OH_COLORSPACE_BT2020_PQ_LIMIT:
      return gfx::ColorSpace(
          Primary::BT2020, Transfer::PQ, Matrix::BT2020_NCL,
          space == OH_COLORSPACE_BT2020_PQ_FULL ? Range::FULL : Range::LIMITED);
    default:
      return std::nullopt;
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
  // to the system EGL and its Vulkan backend imports with
  // VK_OHOS_external_memory -- when the device has that extension, which
  // the display then says with EGL_OHOS_image_native_buffer. Without it,
  // video keeps uploading planes. A process that has not set up GL (kNone)
  // answers for the GPU's sake as GLES would.
  const gl::ANGLEImplementation angle = gl::GetANGLEImplementation();
  if (angle == gl::ANGLEImplementation::kNone ||
      angle == gl::ANGLEImplementation::kOpenGLES) {
    return true;
  }
  if (angle != gl::ANGLEImplementation::kVulkan) {
    return false;
  }
  static const bool vulkan_imports = [] {
    gl::GLDisplayEGL* display = gl::GLSurfaceEGL::GetGLDisplayEGL();
    const char* extensions =
        display ? eglQueryString(display->GetDisplay(), EGL_EXTENSIONS)
                : nullptr;
    const bool imports =
        extensions &&
        std::string(extensions).find("EGL_OHOS_image_native_buffer") !=
            std::string::npos;
    LOG(WARNING) << "OHOS native pixmap: ANGLE Vulkan "
                 << (imports ? "imports" : "cannot import")
                 << " OH_NativeBuffers";
    return imports;
  }();
  return vulkan_imports;
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

scoped_refptr<gfx::NativePixmap> CreateOhosPhoneOffscreenPixmap(
    gfx::Size size,
    viz::SharedImageFormat format) {
  const auto native_format = OhosFormatFor(format);
  if (size.IsEmpty() || !native_format ||
      !phone_gpu::IsSharedImageFormat(*native_format))
    return nullptr;
  phone_gpu::SharedMessage response;
  if (!AllocateOhosPhoneGpuImage(size.width(), size.height(), *native_format,
                                 &response))
    return nullptr;
  gfx::NativePixmapHandle handle;
  handle.ohos_buffer_prefix.assign(response.packet.prefix.begin(),
                                    response.packet.prefix.end());
  handle.ohos_buffer_tail.assign(
      response.packet.tail.begin(),
      response.packet.tail.begin() + response.packet.tail_bytes);
  for (int fd : response.fds)
    handle.ohos_buffer_fds.emplace_back(fd);
  response.fds.clear();
  return ImportOhosOffscreenPixmap(size, format, std::move(handle));
}

scoped_refptr<gfx::NativePixmap> ImportOhosOffscreenPixmap(
    gfx::Size size,
    viz::SharedImageFormat format,
    gfx::NativePixmapHandle handle) {
  const auto fail = [&](const char* step) -> scoped_refptr<gfx::NativePixmap> {
    LOG(ERROR) << "OHOS shared image: step=" << step
               << " size=" << size.ToString() << " format=" << format.ToString()
               << " fds=" << handle.ohos_buffer_fds.size();
    return nullptr;
  };
  const auto native_format = OhosFormatFor(format);
  if (size.IsEmpty() || !native_format ||
      !phone_gpu::IsSharedImageFormat(*native_format) ||
      handle.ohos_video_release_fd.is_valid() ||
      handle.ohos_buffer_prefix.size() != 14 ||
      handle.ohos_buffer_tail.size() > 1044 ||
      handle.ohos_buffer_fds.size() >= phone_gpu::kSharedMaxFds)
    return fail("import-handle");
  phone_gpu::SharedMessage message;
  auto& p = message.packet;
  p.operation = phone_gpu::SharedOp::kImage;
  p.sequence = p.widget = 1;
  p.width = size.width();
  p.height = size.height();
  p.fd_count = static_cast<uint32_t>(handle.ohos_buffer_fds.size());
  p.tail_bytes = static_cast<uint32_t>(handle.ohos_buffer_tail.size());
  std::copy(handle.ohos_buffer_prefix.begin(), handle.ohos_buffer_prefix.end(),
            p.prefix.begin());
  std::copy(handle.ohos_buffer_tail.begin(), handle.ohos_buffer_tail.end(),
            p.tail.begin());
  if (p.prefix[8] != static_cast<uint32_t>(*native_format) ||
      !phone_gpu::ValidSharedPacket(p, handle.ohos_buffer_fds.size()))
    return fail("import-packet");
  for (const auto& fd : handle.ohos_buffer_fds) {
    if (!fd.is_valid())
      return fail("import-fd");
    int copy = HANDLE_EINTR(dup(fd.get()));
    if (copy < 0) {
      PLOG(ERROR) << "OHOS shared image: import parcel fd duplication";
      return fail("import-dup");
    }
    message.fds.push_back(copy);
  }
  auto* native = phone_gpu::ImportSharedBuffer(message);
  if (!native)
    return fail("import-parcel");
  OH_NativeBuffer_Config config{};
  OH_NativeBuffer_GetConfig(native, &config);
  if (config.width != size.width() || config.height != size.height() ||
      config.format != *native_format) {
    OH_NativeBuffer_Unreference(native);
    return fail("import-config");
  }
  auto* window =
      OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(native);
  OH_NativeBuffer_Unreference(native);
  if (!window)
    return fail("import-window");
  // NativeWindowBuffer retains the imported NativeBuffer and its descriptors.
  // Borrow that FD for local Vulkan/Skia users instead of retaining the IPC
  // descriptors plus another plane duplicate until the canvas is collected.
  const BufferHandle* buffer_handle =
      OH_NativeWindow_GetBufferHandleFromNative(window);
  if (!buffer_handle || buffer_handle->fd < 0) {
    OH_NativeWindow_DestroyNativeWindowBuffer(window);
    return fail("import-window-handle");
  }
  return base::MakeRefCounted<OhosNativePixmap>(window, size, format,
      PlaneLayout{p.prefix[5], 0, p.prefix[7]}, p.prefix[0], buffer_handle->fd);
}

scoped_refptr<gfx::NativePixmap> CreateOhosVideoNativePixmap(
    void* window_buffer,
    gfx::ColorSpace* color_space,
    base::OnceClosure release) {
  base::ScopedClosureRunner release_on_failure(std::move(release));
  auto* window = static_cast<OHNativeWindowBuffer*>(window_buffer);
  OH_NativeBuffer* buffer = nullptr;
  if (!window || OH_NativeBuffer_FromNativeWindowBuffer(window, &buffer) != 0 ||
      !buffer) {
    return nullptr;
  }
  OH_NativeBuffer_Config config = {};
  OH_NativeBuffer_GetConfig(buffer, &config);
  if (config.width <= 0 || config.height <= 0) {
    return nullptr;
  }
  viz::SharedImageFormat format;
  switch (config.format) {
    case NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP:
      format = viz::MultiPlaneFormat::kNV12;
      break;
    case NATIVEBUFFER_PIXEL_FMT_YCBCR_P010:
      format = viz::MultiPlaneFormat::kP010;
      break;
    default:
      // Do not infer precision or chroma order from the compressed profile.
      LOG(ERROR) << "OHOS video zero-copy: unsupported native format "
                 << config.format;
      return nullptr;
  }
  OH_NativeBuffer_ColorSpace native_space = OH_COLORSPACE_NONE;
  if (OH_NativeBuffer_GetColorSpace(buffer, &native_space) == 0) {
    if (auto decoded_space = OhosVideoColorSpace(native_space)) {
      *color_space = *decoded_space;
    }
  }
  format.SetPrefersExternalSampler();
  return base::MakeRefCounted<OhosNativePixmap>(
      window, gfx::Size(config.width, config.height), format,
      OH_NativeBuffer_GetSeqNum(buffer),
      release_on_failure.Release());
}

std::optional<uint32_t> GetOhosVideoNativePixmapId(
    const gfx::NativePixmap& pixmap) {
  return static_cast<const OhosNativePixmap&>(pixmap).video_buffer_id();
}

scoped_refptr<gfx::NativePixmap> CloneOhosVideoNativePixmapForImport(
    const gfx::NativePixmap& pixmap) {
  const auto& video = static_cast<const OhosNativePixmap&>(pixmap);
  const auto id = video.video_buffer_id();
  if (!id) {
    return nullptr;
  }
  OH_NativeBuffer* buffer = nullptr;
  if (OH_NativeBuffer_FromNativeWindowBuffer(
          video.WindowBuffer(gfx::ColorSpace()), &buffer) != 0 || !buffer) {
    return nullptr;
  }
  // Do not retain the acquired queue wrapper: it carries a frame lease, and
  // its release runs on the decoder sequence. This independent wrapper owns
  // only an allocation reference and is used/destroyed on the GPU sequence.
  auto* window =
      OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(buffer);
  if (!window) {
    return nullptr;
  }
  using OwnedWindowBuffer =
      std::unique_ptr<OHNativeWindowBuffer,
                      decltype(&OH_NativeWindow_DestroyNativeWindowBuffer)>;
  return base::MakeRefCounted<OhosNativePixmap>(
      window, pixmap.GetBufferSize(), pixmap.GetSharedImageFormat(), *id,
      base::BindOnce([](OwnedWindowBuffer) {},
                     OwnedWindowBuffer(
                         window, &OH_NativeWindow_DestroyNativeWindowBuffer)));
}

gfx::NativePixmapHandle ExportOhosVideoNativePixmap(
    const gfx::NativePixmap& pixmap,
    base::ScopedFD release_endpoint) {
  gfx::NativePixmapHandle handle;
  using Writer = int32_t (*)(OH_NativeBuffer*, OHIPCParcel*);
  auto writer = reinterpret_cast<Writer>(
      dlsym(RTLD_DEFAULT, "OH_NativeBuffer_WriteToParcel"));
  auto* native = static_cast<OH_NativeBuffer*>(GetOhosNativeBuffer(pixmap));
  if (!native || !writer)
    return handle;
  using Parcel = std::unique_ptr<OHIPCParcel, decltype(&OH_IPCParcel_Destroy)>;
  Parcel parcel(OH_IPCParcel_Create(), &OH_IPCParcel_Destroy);
  if (!parcel || writer(native, parcel.get()))
    return handle;
  const void* prefix = OH_IPCParcel_ReadBuffer(parcel.get(), 56);
  if (!prefix)
    return handle;
  handle.ohos_buffer_prefix.resize(14);
  memcpy(handle.ohos_buffer_prefix.data(), prefix, 56);
  const auto& p = handle.ohos_buffer_prefix;
  if (p[1] != 1 || p[2] > 6 || p[3] > 256 || p[13] != 1)
    return {};
  for (uint32_t i = 0; i < p[2] + 1; ++i) {
    int fd = -1;
    if (OH_IPCParcel_ReadFileDescriptor(parcel.get(), &fd) || fd < 0)
      return {};
    handle.ohos_buffer_fds.emplace_back(fd);
  }
  int remaining = OH_IPCParcel_GetReadableBytes(parcel.get());
  if (remaining < 0 || remaining > 1044)
    return {};
  if (remaining) {
    const void* tail = OH_IPCParcel_ReadBuffer(parcel.get(), remaining);
    if (!tail)
      return {};
    handle.ohos_buffer_tail.resize(remaining);
    memcpy(handle.ohos_buffer_tail.data(), tail, remaining);
  }
  auto format = pixmap.GetSharedImageFormat();
  format.ClearPrefersExternalSampler();
  const bool p010 = format == viz::MultiPlaneFormat::kP010;
  const uint32_t native_format = p010 ? NATIVEBUFFER_PIXEL_FMT_YCBCR_P010
                                      : NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP;
  if (!ValidOhosVideoBufferParcel(p, remaining, handle.ohos_buffer_fds.size(),
                                  pixmap.GetBufferSize().width(),
                                  pixmap.GetBufferSize().height(),
                                  native_format, p010 ? 2 : 1)) {
    LOG(ERROR) << "OHOS video parcel rejected size="
               << pixmap.GetBufferSize().ToString() << " stride=" << p[5]
               << " bytes=" << p[7] << " format=" << p[8];
    return {};
  }
  // Describes plane bounds to Chromium's generic handle validator. Sampling
  // uses the original native allocation, never these linear-layout hints.
  const uint64_t y_bytes = uint64_t{p[5]} * p[6];
  const uint64_t uv_bytes = uint64_t{p[5]} * ((p[6] + 1) / 2);
  for (int i = 0; i < 2; ++i) {
    base::ScopedFD fd(HANDLE_EINTR(dup(handle.ohos_buffer_fds[0].get())));
    if (!fd.is_valid())
      return {};
    handle.planes.emplace_back(p[5], i ? y_bytes : 0, i ? uv_bytes : y_bytes,
                               std::move(fd));
  }
  handle.ohos_video_release_fd = std::move(release_endpoint);
  return handle;
}

scoped_refptr<gfx::NativePixmap> ImportOhosVideoNativePixmap(
    gfx::Size size,
    viz::SharedImageFormat format,
    gfx::NativePixmapHandle handle) {
  if (!handle.ohos_video_release_fd.is_valid())
    return nullptr;
  auto plain_format = format;
  plain_format.ClearPrefersExternalSampler();
  if (plain_format != viz::MultiPlaneFormat::kNV12 &&
      plain_format != viz::MultiPlaneFormat::kP010)
    return nullptr;
  const bool p010 = plain_format == viz::MultiPlaneFormat::kP010;
  const uint32_t native_format = p010 ? NATIVEBUFFER_PIXEL_FMT_YCBCR_P010
                                      : NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP;
  if (!ValidOhosVideoBufferParcel(handle.ohos_buffer_prefix,
                                  handle.ohos_buffer_tail.size(),
                                  handle.ohos_buffer_fds.size(), size.width(),
                                  size.height(), native_format, p010 ? 2 : 1))
    return nullptr;
  using Reader = int32_t (*)(OHIPCParcel*, OH_NativeBuffer**);
  auto reader = reinterpret_cast<Reader>(
      dlsym(RTLD_DEFAULT, "OH_NativeBuffer_ReadFromParcel"));
  using Parcel = std::unique_ptr<OHIPCParcel, decltype(&OH_IPCParcel_Destroy)>;
  Parcel parcel(OH_IPCParcel_Create(), &OH_IPCParcel_Destroy);
  if (!reader || !parcel ||
      OH_IPCParcel_WriteBuffer(
          parcel.get(),
          reinterpret_cast<const uint8_t*>(handle.ohos_buffer_prefix.data()),
          56))
    return nullptr;
  for (const auto& fd : handle.ohos_buffer_fds)
    if (!fd.is_valid() ||
        OH_IPCParcel_WriteFileDescriptor(parcel.get(), fd.get()))
      return nullptr;
  if (!handle.ohos_buffer_tail.empty() &&
      OH_IPCParcel_WriteBuffer(parcel.get(), handle.ohos_buffer_tail.data(),
                               handle.ohos_buffer_tail.size()))
    return nullptr;
  OH_NativeBuffer* native = nullptr;
  if (reader(parcel.get(), &native) || !native)
    return nullptr;
  OH_NativeBuffer_Config config{};
  OH_NativeBuffer_GetConfig(native, &config);
  if (config.width != size.width() || config.height != size.height() ||
      config.format != static_cast<int>(native_format)) {
    OH_NativeBuffer_Unreference(native);
    return nullptr;
  }
  auto* window =
      OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer(native);
  const uint32_t id = OH_NativeBuffer_GetSeqNum(native);
  OH_NativeBuffer_Unreference(native);
  if (!window)
    return nullptr;
  format.SetPrefersExternalSampler();
  return base::MakeRefCounted<OhosNativePixmap>(
      window, size, format, id,
      base::BindOnce(
          [](OHNativeWindowBuffer* window, base::ScopedFD release) {
            OH_NativeWindow_DestroyNativeWindowBuffer(window);
          },
          window, std::move(handle.ohos_video_release_fd)));
}

std::unique_ptr<NativePixmapGLBinding> ImportOhosNativePixmap(
    scoped_refptr<gfx::NativePixmap> pixmap,
    const gfx::ColorSpace& color_space,
    unsigned int target,
    unsigned int texture_id) {
  if (!pixmap || !IsOhosNativePixmapFormat(pixmap->GetSharedImageFormat())) {
    return nullptr;
  }
  OHNativeWindowBuffer* window_buffer =
      static_cast<OhosNativePixmap*>(pixmap.get())->WindowBuffer(color_space);
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
  const auto& video = static_cast<const OhosNativePixmap&>(pixmap);
  if (video.has_direct_buffer()) {
    OH_NativeBuffer* native = nullptr;
    if (OH_NativeBuffer_FromNativeWindowBuffer(
            video.WindowBuffer(gfx::ColorSpace()), &native) == 0)
      return native;
    return nullptr;
  }
  // Legacy allocator-backed buffers are identified through their registry.
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
