// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "ohos_overlay_session.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace ui::ohos_overlay {
namespace {
bool ValidRect(const Rect& r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
         std::isfinite(r.height) && r.width > 0 && r.height > 0 &&
         std::isfinite(r.x + r.width) && std::isfinite(r.y + r.height);
}
bool ValidPlane(const Plane& p, bool primary) {
  if (!p.lease || !p.lease->native_buffer() || !p.width || !p.height ||
      p.width > 16384 || p.height > 16384 || !ValidRect(p.source) ||
      !ValidRect(p.destination) || p.source.x < 0 || p.source.y < 0 ||
      p.source.x + p.source.width > p.width ||
      p.source.y + p.source.height > p.height || p.color.hdr_type.size() > 64 ||
      p.color.hdr_static.size() > 4096)
    return false;
  if (primary) {
    // Underlay requires full alpha precision. Do not put SDR webpage text into
    // an 8-bit PQ primary or force its color space to match the HDR video.
    return (p.format == Format::kRgba8 || p.format == Format::kBgra8) &&
           p.color.transfer == Transfer::kSdr && p.color.hdr_type.empty() &&
           p.color.hdr_static.empty();
  }
  if (p.format != Format::kNv12 && p.format != Format::kP010)
    return false;
  if (p.color.transfer != Transfer::kSdr && p.color.transfer != Transfer::kPq)
    return false;  // HLG not yet validated.
  if (p.color.transfer == Transfer::kPq &&
      (p.format != Format::kP010 || !p.color.native_space ||
       p.color.hdr_type.empty()))
    return false;
  return p.color.transfer != Transfer::kSdr ||
         (p.color.hdr_type.empty() && p.color.hdr_static.empty());
}
}  // namespace

Session::Session(Backend& backend, size_t max_allocations_per_plane)
    : backend_(backend), max_allocations_per_plane_(max_allocations_per_plane) {}
Session::~Session() {
  // Dropping an outstanding decoder lease corrupts displayed frames. The
  // caller must finish asynchronous close, never impose a destruction timeout.
  if (!closed() || !leases_.empty())
    std::abort();
}
bool Session::Valid(const Frame& f) {
  if (!ValidPlane(f.primary, true) ||
      f.primary_has_video_hole != f.video.has_value())
    return false;
  if (!f.video)
    return true;
  if (!ValidPlane(*f.video, false) ||
      f.primary.lease->allocation_id() == f.video->lease->allocation_id())
    return false;
  const auto& web = f.primary.destination;
  const auto& video = f.video->destination;
  // The oracle validates clipping and the presenter applies the source crop.
  // Reject geometry that would expose video outside the submitted primary.
  return video.x >= web.x && video.y >= web.y &&
         video.x + video.width <= web.x + web.width &&
         video.y + video.height <= web.y + web.height;
}
SubmitResult Session::Submit(Frame& f) {
  if (state_ != State::kRunning)
    return SubmitResult::kClosed;
  if (!Valid(f) || !backend_.Supports(f))
    return SubmitResult::kUnsupported;
  Poll();
  if (!backend_.ReadyForFrame())
    return SubmitResult::kBusy;
  const auto reusable = [&](const Plane& p, unsigned plane) {
    const auto found = leases_.find(p.lease->allocation_id());
    return !p.reuse ||
           (current_[plane] == p.lease->allocation_id() &&
            found != leases_.end() && found->second.plane == plane &&
            !found->second.released && p.lease->BorrowAcquireFence() == -1 &&
            found->second.format == p.format &&
            found->second.width == p.width &&
            found->second.height == p.height &&
            found->second.color.transfer == p.color.transfer &&
            found->second.color.native_space == p.color.native_space &&
            found->second.color.hdr_type == p.color.hdr_type &&
            found->second.color.hdr_static == p.color.hdr_static);
  };
  if (!reusable(f.primary, 0) || (f.video && !reusable(*f.video, 1)))
    return SubmitResult::kUnsupported;
  // Primary and video retire independently. Counting distinct submission IDs
  // lets retained video fill the entire budget and block a web-only update
  // that RS needs in order to return the previous primary. Bound each queue,
  // retaining every real read lease until its release and fences complete.
  size_t allocations[2] = {};
  for (const auto& [id, held] : leases_)
    ++allocations[held.plane];
  if ((!f.primary.reuse && allocations[0] >= max_allocations_per_plane_) ||
      (f.video && !f.video->reuse &&
       allocations[1] >= max_allocations_per_plane_) ||
      (!f.primary.reuse && leases_.count(f.primary.lease->allocation_id())) ||
      (f.video && !f.video->reuse &&
       leases_.count(f.video->lease->allocation_id())))
    return SubmitResult::kBusy;
  if (last_submission_ == std::numeric_limits<uint64_t>::max())
    return SubmitResult::kClosed;
  const auto sequence = last_submission_ + 1;
  auto batch = backend_.Prepare(sequence, f);
  if (!batch)
    return SubmitResult::kPrepareFailed;  // Caller still owns both read leases.
  const auto hold = [&](Plane& p, unsigned plane) {
    const auto id = p.lease->allocation_id();
    current_[plane] = id;
    if (p.reuse) {
      redundant_.push_back(std::move(p.lease));
      return;
    }
    leases_.emplace(id, Held{sequence, plane, false, false, std::move(p.lease),
                             p.format, p.width, p.height, p.color});
  };
  hold(f.primary, 0);
  if (f.video)
    hold(*f.video, 1);
  else
    current_[1] = 0;
  last_submission_ = sequence;
  backend_.Commit(std::move(batch));
  return SubmitResult::kAccepted;
}
void Session::Poll() {
  redundant_.clear();
  if (closed())
    return;
  for (const auto& event : backend_.PollReleases()) {
    auto found = leases_.find(event.allocation);
    if (found == leases_.end() ||
        found->second.submission != event.submission ||
        found->second.plane != event.plane)
      continue;  // Late callback from a previous use/generation is not a
                 // release.
    auto& held = found->second;
    held.released |= event.server_returned;
    held.fence_ready |= event.server_returned && event.release_fence_signaled;
  }
  // Remove from routing before running lease destructors; they may notify the
  // decoder and enqueue further work on its own sequence.
  std::vector<std::unique_ptr<ReadLease>> retired;
  for (auto it = leases_.begin(); it != leases_.end();) {
    auto& held = it->second;
    if (held.released && held.fence_ready && held.lease->ProducerReady()) {
      retired.push_back(std::move(held.lease));
      it = leases_.erase(it);
    } else {
      ++it;
    }
  }
  if (state_ == State::kWaitingForCommands &&
      (last_submission_ == 0 || backend_.CommandsProcessed(last_submission_))) {
    backend_.BeginDrain();
    state_ = State::kDraining;
  }
  if (state_ == State::kDraining && leases_.empty() &&
      backend_.DrainCommandsProcessed()) {
    backend_.DestroyLayers();
    state_ = State::kClosed;
  }
}
void Session::Close() {
  if (state_ == State::kRunning)
    state_ = State::kWaitingForCommands;
  Poll();
}
}  // namespace ui::ohos_overlay
