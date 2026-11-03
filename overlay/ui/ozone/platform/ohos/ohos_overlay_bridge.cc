// Copyright 2026 The Chromium Authors. All rights reserved.
#include "ohos_overlay_bridge.h"
#include "ohos_rs_overlay_backend.h"

#include <fcntl.h>
#include <native_buffer/graphic_error_code.h>
#include <native_buffer/native_buffer.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#ifdef OHOS_OVERLAY_DIAGNOSTICS
#include <hilog/log.h>
#define TRACE(...) \
  OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F01, "LumieRealVideo", __VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

using namespace ui::ohos_overlay;
namespace {
class NativeReadLease final : public ReadLease {
 public:
  explicit NativeReadLease(const OhosOverlayPlane& p) : input_(p) {
    buffer_ = static_cast<OH_NativeBuffer*>(p.native_buffer);
    if (!buffer_ || OH_NativeBuffer_Reference(buffer_) != 0) {
      buffer_ = nullptr;
      return;
    }
    if (p.acquire_fence >= 0) {
      fence_ = fcntl(p.acquire_fence, F_DUPFD_CLOEXEC, 0);
      if (fence_ < 0) {
        OH_NativeBuffer_Unreference(buffer_);
        buffer_ = nullptr;
      }
    }
  }
  ~NativeReadLease() override {
    if (buffer_)
      OH_NativeBuffer_Unreference(buffer_);
    if (fence_ >= 0)
      close(fence_);
    if (accepted_)
      input_.release_lease(input_.lease_context);
  }
  bool valid() const { return buffer_; }
  void Accept() { accepted_ = true; }
  uint64_t allocation_id() const override {
    return OH_NativeBuffer_GetSeqNum(buffer_);
  }
  void* native_buffer() const override { return buffer_; }
  int BorrowAcquireFence() const override { return fence_; }
  bool ProducerReady() const override {
    if (fence_ == -1)
      return true;
    pollfd p{fence_, POLLIN, 0};
    return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
  }

 private:
  OhosOverlayPlane input_;
  OH_NativeBuffer* buffer_ = nullptr;
  int fence_ = -1;
  bool accepted_ = false;
};
enum class MetadataResult { kPresent, kUnsupported, kInvalid };
MetadataResult ReadMetadata(OH_NativeBuffer* buffer,
                            OH_NativeBuffer_MetadataKey key,
                            std::vector<uint8_t>& result,
                            size_t limit) {
  uint8_t* bytes = nullptr;
  int32_t size = 0;
  const int rc = OH_NativeBuffer_GetMetadataValue(buffer, key, &size, &bytes);
  std::unique_ptr<uint8_t[]> owner(bytes);
  if (rc == NATIVE_ERROR_UNSUPPORTED && !bytes && size == 0)
    return MetadataResult::kUnsupported;
  if (rc || !bytes || size <= 0 || static_cast<size_t>(size) > limit)
    return MetadataResult::kInvalid;
  result.assign(bytes, bytes + size);
  return MetadataResult::kPresent;
}
bool ReadColor(OH_NativeBuffer* buffer, Color& color, uint32_t flags) {
  OH_NativeBuffer_ColorSpace space = OH_COLORSPACE_NONE;
  if (OH_NativeBuffer_GetColorSpace(buffer, &space) != 0)
    return false;
  color.native_space = static_cast<uint32_t>(space);
  switch (space) {
    case OH_COLORSPACE_BT2020_PQ_FULL:
    case OH_COLORSPACE_DISPLAY_BT2020_PQ:
    case OH_COLORSPACE_BT2020_PQ_LIMIT:
      color.transfer = Transfer::kPq;
      break;
    case OH_COLORSPACE_SRGB_FULL:
    case OH_COLORSPACE_DISPLAY_SRGB:
    case OH_COLORSPACE_BT709_FULL:
    case OH_COLORSPACE_BT709_LIMIT:
      color.transfer = Transfer::kSdr;
      break;
    default:
      return false;
  }
  const auto type_result =
      ReadMetadata(buffer, OH_HDR_METADATA_TYPE, color.hdr_type, 64);
  if (type_result == MetadataResult::kInvalid)
    return false;
  if (type_result == MetadataResult::kUnsupported) {
    // Some public SDKs cannot represent CM_METADATA_NONE in their type map.
    // Only the allocation owner can attest to a fresh SDR-only epoch. A getter
    // error alone never grants SDR eligibility, including imported video.
    if (color.transfer != Transfer::kSdr ||
        !(flags & OHOS_OVERLAY_SDR_TAGS_PRISTINE))
      return false;
    std::vector<uint8_t> static_metadata;
    return ReadMetadata(buffer, OH_HDR_STATIC_METADATA, static_metadata,
                        4096) == MetadataResult::kUnsupported;
  }
  if (color.hdr_type.size() != sizeof(OH_NativeBuffer_MetadataType))
    return false;
  OH_NativeBuffer_MetadataType type;
  std::memcpy(&type, color.hdr_type.data(), sizeof(type));
  if (color.transfer == Transfer::kSdr) {
    if (type != OH_VIDEO_NONE)
      return false;  // Stale HDR tag may not be relabelled.
    std::vector<uint8_t> static_metadata;
    if (ReadMetadata(buffer, OH_HDR_STATIC_METADATA, static_metadata, 4096) !=
        MetadataResult::kUnsupported)
      return false;
    color.hdr_type.clear();
    return true;
  }
  if (type != OH_VIDEO_HDR_HDR10)
    return false;
  // A PQ stream may omit static mastering/CLL. Preserve it when present; never
  // invent metadata from a display's peak brightness or mutate decoder buffers.
  return ReadMetadata(buffer, OH_HDR_STATIC_METADATA, color.hdr_static, 4096) !=
         MetadataResult::kInvalid;
}
bool Convert(const OhosOverlayPlane& input, Plane& out) {
  if ((input.flags &
       ~(OHOS_OVERLAY_SDR_TAGS_PRISTINE | OHOS_OVERLAY_REUSE_CONTENT)) ||
      input.acquire_fence < -1 || !input.release_lease)
    return false;
  out.reuse = input.flags & OHOS_OVERLAY_REUSE_CONTENT;
  auto lease = std::make_unique<NativeReadLease>(input);
  if (!lease->valid())
    return false;
  auto* buffer = static_cast<OH_NativeBuffer*>(lease->native_buffer());
  OH_NativeBuffer_Config config = {};
  OH_NativeBuffer_GetConfig(buffer, &config);
  switch (config.format) {
    case NATIVEBUFFER_PIXEL_FMT_RGBA_8888:
      out.format = Format::kRgba8;
      break;
    case NATIVEBUFFER_PIXEL_FMT_BGRA_8888:
      out.format = Format::kBgra8;
      break;
    case NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP:
      out.format = Format::kNv12;
      break;
    case NATIVEBUFFER_PIXEL_FMT_YCBCR_P010:
      out.format = Format::kP010;
      break;
    default:
      return false;
  }
  if (!ReadColor(buffer, out.color, input.flags)) {
    TRACE("convert_color_rejected flags=%{public}u", input.flags);
    return false;
  }
  out.width = config.width;
  out.height = config.height;
  out.source = {input.source.x, input.source.y, input.source.width,
                input.source.height};
  out.destination = {input.destination.x, input.destination.y,
                     input.destination.width, input.destination.height};
  out.lease = std::move(lease);
  return true;
}
}  // namespace

struct OhosOverlayHost {
  explicit OhosOverlayHost(std::unique_ptr<Backend> b)
      : owner(pthread_self()), backend(std::move(b)), session(*backend) {}
  bool OnOwner() const { return pthread_equal(owner, pthread_self()); }
  pthread_t owner;
  std::unique_ptr<Backend> backend;
  Session session;
};
extern "C" OhosOverlayHost* OhosOverlayCreate(uint32_t version,
                                              uint64_t token,
                                              uint64_t parent) {
  if (version != OHOS_OVERLAY_BRIDGE_VERSION)
    return nullptr;
  auto backend = CreateRsOverlayBackend(token, parent);
  return backend ? new OhosOverlayHost(std::move(backend)) : nullptr;
}
extern "C" OhosOverlayResult OhosOverlaySubmit(OhosOverlayHost* host,
                                               const OhosOverlayFrame* input) {
  if (!host || !host->OnOwner() || !input ||
      input->version != OHOS_OVERLAY_BRIDGE_VERSION || input->reserved ||
      input->has_video > 1 || input->primary_has_video_hole > 1)
    return OHOS_OVERLAY_UNSUPPORTED;
  Frame frame;
  frame.primary_has_video_hole = input->primary_has_video_hole;
  if (!Convert(input->primary, frame.primary)) {
    TRACE("primary_rejected");
    return OHOS_OVERLAY_UNSUPPORTED;
  }
  auto* primary = static_cast<NativeReadLease*>(frame.primary.lease.get());
  NativeReadLease* video = nullptr;
  if (input->has_video) {
    frame.video.emplace();
    if (!Convert(input->video, *frame.video)) {
      TRACE("video_rejected");
      return OHOS_OVERLAY_UNSUPPORTED;
    }
    video = static_cast<NativeReadLease*>(frame.video->lease.get());
  }
  const auto result = host->session.Submit(frame);
  if (result != SubmitResult::kAccepted && result != SubmitResult::kBusy)
    TRACE("session_rejected rc=%{public}u", static_cast<unsigned>(result));
  if (result == SubmitResult::kAccepted) {
    primary->Accept();
    if (video)
      video->Accept();
  }
  return static_cast<OhosOverlayResult>(result);
}
extern "C" void OhosOverlayPoll(OhosOverlayHost* host, OhosOverlayStatus* out) {
  if (!host || !host->OnOwner())
    return;
  host->session.Poll();
  if (out)
    *out = {OHOS_OVERLAY_BRIDGE_VERSION,
            static_cast<uint32_t>(host->session.state()),
            static_cast<uint32_t>(host->session.outstanding_allocations()),
            host->session.CommandsProcessed() ? 1u : 0u,
            host->session.last_submission()};
}
extern "C" void OhosOverlayClose(OhosOverlayHost* host) {
  if (host && host->OnOwner())
    host->session.Close();
}
extern "C" int OhosOverlayDestroy(OhosOverlayHost* host) {
  if (!host)
    return 1;
  if (!host->OnOwner() || !host->session.closed())
    return 0;
  delete host;
  return 1;
}
