// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "ui/ozone/platform/ohos/ohos_overlay_presenter.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <map>
#include <string_view>
#include <utility>
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/timer/timer.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/gfx/overlay_plane_data.h"
#include "ui/gl/presenter.h"
#include "ui/ozone/platform/ohos/ohos_native_pixmap.h"
#include "ui/ozone/platform/ohos/ohos_output_color_config.h"
#include "ui/ozone/platform/ohos/ohos_overlay_transport.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_channel.h"
#include "ui/ozone/public/overlay_candidates_ozone.h"
#include "ui/ozone/public/overlay_manager_ozone.h"

namespace ui {
namespace {
using namespace ohos_overlay;
struct Availability {
  base::Lock lock;
  std::map<gfx::AcceleratedWidget, bool> widgets;
};
Availability& Available() {
  static base::NoDestructor<Availability> a;
  return *a;
}
void SetAvailable(gfx::AcceleratedWidget w, bool value) {
  auto& a = Available();
  base::AutoLock lock(a.lock);
  a.widgets[w] = value;
}
bool IsAvailable(gfx::AcceleratedWidget w) {
  auto& a = Available();
  base::AutoLock lock(a.lock);
  auto it = a.widgets.find(w);
  return it != a.widgets.end() && it->second;
}
bool Integral(const gfx::RectF& r) {
  return !r.IsEmpty() && std::isfinite(r.x()) && std::isfinite(r.y()) &&
         std::isfinite(r.right()) && std::isfinite(r.bottom()) && r.x() >= 0 &&
         r.y() >= 0 && r.right() <= 16384 && r.bottom() <= 16384 &&
         std::floor(r.x()) == r.x() && std::floor(r.y()) == r.y() &&
         std::floor(r.width()) == r.width() &&
         std::floor(r.height()) == r.height();
}
bool Identity(const std::variant<gfx::OverlayTransform, gfx::Transform>& t) {
  auto* value = std::get_if<gfx::OverlayTransform>(&t);
  return value && *value == gfx::OVERLAY_TRANSFORM_NONE;
}
OhosOverlayRect Rect(const gfx::RectF& r) {
  return {r.x(), r.y(), r.width(), r.height()};
}
bool VideoFormat(viz::SharedImageFormat f) {
  if (f.PrefersExternalSampler())
    f.ClearPrefersExternalSampler();
  return f == viz::MultiPlaneFormat::kNV12 || f == viz::MultiPlaneFormat::kP010;
}
class Candidates final : public OverlayCandidatesOzone {
 public:
  explicit Candidates(gfx::AcceleratedWidget widget) : widget_(widget) {}
  void CheckOverlaySupport(OverlaySurfaceCandidateList* list) override {
    for (auto& c : *list)
      c.overlay_handled = false;
    if (!IsAvailable(widget_) || list->size() != 2) {
      Report(!IsAvailable(widget_) ? "presenter-unavailable" : "plane-count");
      return;
    }
    OverlaySurfaceCandidate *primary = nullptr, *video = nullptr;
    for (auto& c : *list) {
      if (c.plane_z_order == 0)
        primary = &c;
      else if (c.plane_z_order == -1)
        video = &c;
    }
    if (!primary || !video) {
      Report("plane-order");
      return;
    }
    const auto& p = *primary;
    const auto& v = *video;
    const char* rejection = nullptr;
    if (p.color_space.IsHDR() || !Integral(p.display_rect))
      rejection = "primary-color-or-geometry";
    else if (!Integral(v.display_rect))
      rejection = "video-non-integral-geometry";
    else if (!p.display_rect.Contains(v.display_rect))
      rejection = "video-outside-primary";
    else if (!Identity(p.transform) || !Identity(v.transform))
      rejection = "transform";
    else if (!v.is_opaque || v.opacity != 1 ||
             !v.rounded_corners.IsEmpty() || v.background_color)
      rejection = "video-blending-or-rounded-corners";
    else if (!VideoFormat(v.format))
      rejection = "video-format";
    else if (v.clip_rect &&
             !gfx::RectF(*v.clip_rect).Contains(v.display_rect))
      rejection = "video-clipped";
    else if (v.crop_rect.IsEmpty() ||
             !gfx::RectF(1.f, 1.f).Contains(v.crop_rect))
      rejection = "video-crop";
    else if (v.color_space.IsHDR() &&
             v.color_space.GetTransferID() != gfx::ColorSpace::TransferID::PQ)
      rejection = "video-hdr-transfer";
    // Resolve the original decoder acquisition before punching the hole.
    else if (!v.native_pixmap || !GetOhosVideoOverlayToken(*v.native_pixmap))
      rejection = "video-pixmap-or-token";
    if (rejection) {
      Report(rejection, &v);
      return;
    }
    Report("supported", &v);
    primary->overlay_handled = video->overlay_handled = true;
  }
  void ObserveHardwareCapabilities(
      HardwareCapabilitiesCallback callback) override {
    HardwareCapabilities caps;
    caps.is_valid = true;
    caps.num_overlay_capable_planes = 2;
    caps.supported_shared_image_formats = {
        viz::SinglePlaneFormat::kRGBA_8888, viz::SinglePlaneFormat::kBGRA_8888,
        viz::MultiPlaneFormat::kNV12, viz::MultiPlaneFormat::kP010};
    callback.Run(caps);
  }

 private:
  void Report(std::string_view result,
              const OverlaySurfaceCandidate* video = nullptr) {
    const auto now = base::TimeTicks::Now();
    if (result == last_result_ || now - last_report_ < base::Seconds(1))
      return;
    last_result_ = result;
    last_report_ = now;
    LOG(WARNING) << "OHOS overlay: candidate=" << result << " widget=" << widget_
                 << " video=" << (video ? video->display_rect.ToString() : "none")
                 << " crop=" << (video ? video->crop_rect.ToString() : "none")
                 << " format=" << (video ? video->format.ToString() : "none");
  }
  std::string_view last_result_;
  base::TimeTicks last_report_;
  gfx::AcceleratedWidget widget_;
};
class Manager final : public OverlayManagerOzone {
 public:
  Manager() { allow_sync_and_real_buffer_page_flip_testing_ = true; }
  std::unique_ptr<OverlayCandidatesOzone> CreateOverlayCandidates(
      gfx::AcceleratedWidget w) override {
    return std::make_unique<Candidates>(w);
  }
};
class Presenter final : public gl::Presenter {
 public:
  Presenter(gfx::AcceleratedWidget widget, base::ScopedFD socket)
      : widget_(widget), socket_(std::move(socket)) {
    SetAvailable(widget_, true);
    timer_.Start(FROM_HERE, base::Milliseconds(4), this, &Presenter::Poll);
  }
  bool SupportsPlaneGpuFences() const override { return true; }
  bool SupportsViewporter() const override { return true; }
  bool Resize(const gfx::Size& size,
              float scale,
              const gfx::ColorSpace& color,
              bool alpha) override {
    if (!socket_.is_valid() || size.IsEmpty() || size.width() > 8192 ||
        size.height() > 8192 || color.IsHDR() ||
        color != gfx::ColorSpace::CreateSRGB())
      return false;
    size_ = size;
    return true;
  }
  bool ScheduleOverlayPlane(gl::OverlayImage image,
                            std::unique_ptr<gfx::GpuFence> fence,
                            const gfx::OverlayPlaneData& data) override {
    const unsigned plane = data.z_order == -1 ? 1 : 0;
    if (!image || (data.z_order != 0 && data.z_order != -1) ||
        scheduled_[plane] || !Identity(data.plane_transform) ||
        !Integral(data.display_bounds) || data.opacity != 1 ||
        !data.rounded_corners.IsEmpty() || data.is_solid_color || data.color ||
        (data.clip_rect &&
         !gfx::RectF(*data.clip_rect).Contains(data.display_bounds)) ||
        (plane && (!VideoFormat(image->GetSharedImageFormat()) ||
                   !GetOhosVideoOverlayToken(*image)))) {
      invalid_ = true;
      return false;
    }
    auto item = std::make_unique<Scheduled>();
    item->image = std::move(image);
    item->fence = std::move(fence);
    item->data = data;
    scheduled_[plane] = std::move(item);
    return true;
  }
  void Present(SwapCompletionCallback complete,
               PresentationCallback feedback,
               gfx::FrameData data) override {
    auto keep_alive = base::WrapRefCounted(this);
    if (scheduled_[1] && scheduled_[1]->data.color_space.IsHDR() &&
        !hdr_guard_.Validate())
      invalid_ = true;
    auto flight = std::make_unique<Flight>();
    flight->sequence = ++sequence_;
    flight->queued_at = base::TimeTicks::Now();
    flight->complete = std::move(complete);
    flight->feedback = std::move(feedback);
    flight->message = std::make_unique<WireMessage>();
    auto& p = flight->message->packet;
    p.sequence = sequence_;
    p.has_video = scheduled_[1] ? 1 : 0;
    if (invalid_ || !scheduled_[0] || !socket_.is_valid() || size_.IsEmpty()) {
      flights_.emplace(sequence_, std::move(flight));
      invalid_ = true;
      return;
    }
    for (unsigned i = 0; i < 2; ++i) {
      if (!scheduled_[i]) {
        flight->released[i] = true;
        continue;
      }
      auto& item = *scheduled_[i];
      auto& plane = i ? p.video : p.primary;
      auto& previous = last_[i];
      const uint64_t token = i ? GetOhosVideoOverlayToken(*item.image) : 0;
      plane.reuse = previous.image && !item.fence &&
                    (i ? previous.content == token
                       : previous.image.get() == item.image.get());
      plane.content = i ? token : (plane.reuse ? previous.content : sequence_);
      flight->content[i] = plane.content;
      flight->images[i] = item.image;
      if (i)
        p.video_token = token;
      auto source = item.data.crop_rect;
      source.Scale(item.image->GetBufferSize().width(),
                   item.image->GetBufferSize().height());
      plane.source = Rect(source);
      plane.destination = Rect(item.data.display_bounds);
      if (!i && !plane.reuse) {
        auto handle = item.image->ExportHandle();
        if (handle.ohos_buffer_prefix.size() != 14 ||
            handle.ohos_buffer_tail.size() > p.tail.size()) {
          invalid_ = true;
          break;
        }
        std::copy(handle.ohos_buffer_prefix.begin(),
                  handle.ohos_buffer_prefix.end(), p.prefix.begin());
        std::copy(handle.ohos_buffer_tail.begin(),
                  handle.ohos_buffer_tail.end(), p.tail.begin());
        p.tail_bytes = handle.ohos_buffer_tail.size();
        p.fd_count = handle.ohos_buffer_fds.size();
        for (auto& fd : handle.ohos_buffer_fds)
          flight->message->fds.push_back(fd.release());
      }
      previous = {item.image, plane.content};
    }
    if (!p.has_video)
      last_[1] = {};
    // Parcel FDs precede both acquire fences, in plane order.
    for (unsigned i = 0; i < 2; ++i)
      if (scheduled_[i] && scheduled_[i]->fence) {
        auto fence = scheduled_[i]->fence->GetGpuFenceHandle().Clone();
        if (!fence.is_null()) {
          (i ? p.video : p.primary).has_fence = 1;
          flight->message->fds.push_back(fence.Release().release());
        }
      }
    scheduled_[0].reset();
    scheduled_[1].reset();
    const bool valid =
        !invalid_ && ValidWirePacket(p, flight->message->fds.size());
    flights_.emplace(sequence_, std::move(flight));
    if (!valid || flights_.size() > 12)
      invalid_ = true;
    // Poll only on the timer: BufferQueue commits its mailbox list AFTER
    // Present returns, so even rejection callbacks must be asynchronous.
  }

 private:
  friend class base::RefCounted<gl::Presenter>;
  ~Presenter() override {
    timer_.Stop();
    SetAvailable(widget_, false);
    if (socket_.is_valid()) {
      WireMessage close;
      close.packet.operation = WireOp::kClose;
      close.packet.sequence = ++sequence_;
      SendWireMessage(socket_.get(), close);
    }
    // Browser owns independent decoder reads until RS retirement, even if the
    // GPU channel disappears. Allocation lifetime alone would not suffice.
  }
  struct Scheduled {
    gl::OverlayImage image;
    std::unique_ptr<gfx::GpuFence> fence;
    gfx::OverlayPlaneData data;
  };
  struct Last {
    gl::OverlayImage image;
    uint64_t content = 0;
  };
  struct Flight {
    uint64_t sequence = 0, content[2] = {};
    base::TimeTicks queued_at;
    bool released[2] = {}, sent = false, accepted = false, processed = false;
    gl::OverlayImage images[2];
    std::unique_ptr<WireMessage> message;
    SwapCompletionCallback complete;
    PresentationCallback feedback;
  };
  void Fail(const char* reason) {
    SetAvailable(widget_, false);
    timer_.Stop();
    socket_.reset();
    scheduled_[0].reset();
    scheduled_[1].reset();
    last_[0] = {};
    last_[1] = {};
    auto flights = std::move(flights_);
    LOG(ERROR) << "OHOS overlay: channel/plane rejected; recreate output "
                  "without promotion; reason=" << reason;
    for (auto& [seq, f] : flights) {
      if (f->complete)
        std::move(f->complete)
            .Run(gfx::SwapCompletionResult(gfx::SwapResult::SWAP_FAILED));
      if (f->feedback)
        std::move(f->feedback).Run(gfx::PresentationFeedback::Failure());
    }
  }
  void Poll() {
    auto keep_alive = base::WrapRefCounted(this);
    if (invalid_) {
      Fail("invalid-scheduled-frame");
      return;
    }
    if (!socket_.is_valid())
      return;
    for (const auto& [seq, f] : flights_) {
      if (f->complete &&
          base::TimeTicks::Now() - f->queued_at > base::Seconds(10)) {
        // Recreate the GPU output, but let the browser keep polling/draining
        // real RS leases. A timeout never fabricates a buffer release.
        for (const auto& [pending_seq, pending] : flights_) {
          LOG(ERROR) << "OHOS overlay: timeout sequence=" << pending_seq
                     << " accepted=" << pending->accepted
                     << " processed=" << pending->processed
                     << " primary_released=" << pending->released[0]
                     << " video_released=" << pending->released[1]
                     << " primary_content=" << pending->content[0]
                     << " video_content=" << pending->content[1];
        }
        Fail("completion-timeout");
        return;
      }
    }
    for (auto& [seq, f] : flights_) {
      if (f->sent)
        continue;
      if (!SendWireMessage(socket_.get(), *f->message)) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
          Fail("send");
        return;
      }
      f->sent = true;
      f->message.reset();
    }
    for (unsigned n = 0; n < 64; ++n) {
      WireMessage msg;
      if (!ReceiveWireMessage(socket_.get(), &msg)) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          Fail("receive");
          return;
        }
        break;
      }
      const auto& p = msg.packet;
      auto it = flights_.find(p.sequence);
      if (it == flights_.end()) {
        Fail("unknown-sequence");
        return;
      }
      auto& f = *it->second;
      if (p.operation == WireOp::kAccepted && !f.accepted)
        f.accepted = true;
      else if (p.operation == WireOp::kProcessed && f.accepted &&
               !f.processed) {
        f.processed = true;
        const bool video = f.content[1] != 0;
        if (video != video_processed_) {
          LOG(WARNING) << "OHOS overlay: " << (video ? "promotion" : "demotion")
                       << " commands processed widget=" << widget_
                       << " sequence=" << p.sequence;
          video_processed_ = video;
        }
      } else if (p.operation == WireOp::kReleased && f.accepted &&
                 !f.released[p.plane]) {
        f.released[p.plane] = true;
        f.images[p.plane].reset();
      } else {
        LOG(ERROR) << "OHOS overlay: reply operation="
                   << static_cast<unsigned>(p.operation) << " result=" << p.result
                   << " sequence=" << p.sequence << " plane=" << p.plane;
        Fail("rejected-or-unexpected-reply");
        return;
      }
    }
    if (!reported_) {
      const bool processed =
          std::any_of(flights_.begin(), flights_.end(),
                      [](const auto& item) { return item.second->processed; });
      if (processed) {
        if (!ReportOhosPhoneOverlayPresented(widget_)) {
          Fail("presentation-report");
          return;
        }
        reported_ = true;
      }
    }
    // BufferQueue can render into a retired primary again after swap ACK, so
    // every replaced primary must have its real RS release before completion.
    // Video is different: it is imported read-only, and the browser's separate
    // Video/Buffer lease keeps the original decoder acquisition alive until RS
    // releases it, even after SharedImage access ends. Keep its Flight/image
    // until kReleased, but do not gate swap ACK on it. RS can retain two video
    // buffers; waiting for N-1 here prevents submitting the N+1 that retires it.
    std::vector<SwapCompletionCallback> completions;
    std::vector<PresentationCallback> feedbacks;
    for (auto& [seq, f] : flights_) {
      if (!f->complete)
        continue;
      if (!f->processed)
        break;
      bool ready = true;
      for (const auto& [older, prior] : flights_) {
        if (older >= seq)
          break;
        if (!prior->released[0] && prior->content[0] != f->content[0])
          ready = false;
      }
      if (!ready)
        break;
      completions.push_back(std::move(f->complete));
      feedbacks.push_back(std::move(f->feedback));
    }
    for (auto it = flights_.begin(); it != flights_.end();) {
      if (!it->second->complete && it->second->released[0] &&
          it->second->released[1])
        it = flights_.erase(it);
      else
        ++it;
    }
    for (size_t i = 0; i < completions.size(); ++i) {
      std::move(completions[i])
          .Run(gfx::SwapCompletionResult(gfx::SwapResult::SWAP_ACK));
      // Command processing / buffer retirement is not a physical latch time.
      // Do not manufacture presentation timestamps or VSYNC feedback.
      std::move(feedbacks[i]).Run(gfx::PresentationFeedback::Failure());
    }
  }
  gfx::AcceleratedWidget widget_;
  base::ScopedFD socket_;
  gfx::Size size_;
  uint64_t sequence_ = 0;
  bool invalid_ = false;
  bool reported_ = false;
  bool video_processed_ = false;
  OhosHdrDisplayGuard hdr_guard_;
  base::RepeatingTimer timer_;
  std::unique_ptr<Scheduled> scheduled_[2];
  Last last_[2];
  std::map<uint64_t, std::unique_ptr<Flight>> flights_;
};
}  // namespace
scoped_refptr<gl::Presenter> CreateOhosOverlayPresenter(
    gfx::AcceleratedWidget widget) {
  if (!IsOhosPhoneGpuChild())
    return nullptr;
  auto socket = OpenOhosPhoneOverlayChannel(widget);
  if (!socket.is_valid())
    return nullptr;
  LOG(WARNING) << "OHOS overlay: presenter connected widget=" << widget;
  return base::MakeRefCounted<Presenter>(widget, std::move(socket));
}
std::unique_ptr<OverlayManagerOzone> CreateOhosOverlayManager() {
  return std::make_unique<Manager>();
}
}  // namespace ui
