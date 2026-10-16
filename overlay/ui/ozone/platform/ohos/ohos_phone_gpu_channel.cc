// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ui/ozone/platform/ohos/ohos_phone_gpu_channel.h"

#include <errno.h>
#include <native_window/external_window.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>

#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/process/fork_server_ohos.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/thread_pool.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/ozone/platform/ohos/ohos_output_color_config.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_presenter.h"
#include "ui/ozone/platform/ohos/ohos_phone_gpu_shared_buffer.h"
#include "ui/ozone/platform/ohos/ohos_vsync_provider.h"

namespace ui {
namespace {
struct Connection {
  explicit Connection(base::ScopedFD fd) : socket(std::move(fd)) {}
  base::ScopedFD socket;
  std::atomic<bool> active{true};
  std::atomic<bool> presented{false};
  std::atomic<bool> vulkan_requested{false};
  std::atomic<bool> color_retry{false};
  std::shared_ptr<Connection> predecessor;
  base::WaitableEvent retired{base::WaitableEvent::ResetPolicy::MANUAL,
                              base::WaitableEvent::InitialState::NOT_SIGNALED};
};
struct BrowserState {
  // Serializes presentation with replacing a GPU generation. Never held by
  // the UI thread; registry changes only use the registry's own lock.
  base::Lock lock;
  std::shared_ptr<Connection> current GUARDED_BY(lock);
  bool vulkan_failed GUARDED_BY(lock) = false;
};
BrowserState& Browser() {
  static base::NoDestructor<BrowserState> state;
  return *state;
}
struct ChildState {
  base::Lock lock;
  base::ScopedFD socket;
  bool initialized = false;
  uint64_t sequence GUARDED_BY(lock) = 0;
  base::TimeTicks timebase GUARDED_BY(lock);
  base::TimeDelta interval GUARDED_BY(lock);
  base::TimeTicks timing_updated GUARDED_BY(lock);
};
ChildState& Child() {
  static base::NoDestructor<ChildState> state;
  return *state;
}

void ReceiveFrames(std::shared_ptr<Connection> connection) {
  // A previous receiver may still hold HDR leases when the GPU is replaced.
  // Wait on this MayBlock worker, never on UI or under the presentation lock.
  // The next generation must not acquire buffers before the old queue is clean.
  if (connection->predecessor) {
    connection->predecessor->retired.Wait();
    connection->predecessor.reset();
  }
  uint64_t last_sequence = 0;
  bool hdr_display_reported = false;
  std::vector<uint8_t> pixels;
  struct Pending {
    phone_gpu::SharedPacket packet;
    std::unique_ptr<phone_gpu::SharedBufferLease> buffer;
  };
  std::map<uint64_t, Pending> leases;
  phone_gpu::SharedBufferColorState buffer_colors;
  // Bound to this connection and widget. A probe sequence is a fresh color
  // generation; a second probe is rejected while any lease is outstanding.
  struct ColorState {
    OhosOutputColor color;
    uint64_t generation;
    void* window;
  };
  std::map<uint64_t, ColorState> colors;
  OhosVSyncProvider timing(GetOhosApplicationWindowId(), true);
  while (connection->active.load()) {
    uint32_t magic = 0;
    if (!phone_gpu::PeekPhoneGpuMessage(connection->socket.get(), &magic,
                                        200)) {
      if (errno == ETIMEDOUT) {
        timing.PollIdle();
        base::AutoLock lock(Browser().lock);
        buffer_colors.Prune();
        continue;
      }
      break;
    }
    if (magic == phone_gpu::kSharedMagic) {
      phone_gpu::SharedMessage request, response;
      if (!phone_gpu::ReceiveSharedMessage(connection->socket.get(), &request,
                                           0) ||
          request.packet.sequence <= last_sequence)
        break;
      last_sequence = request.packet.sequence;
      const auto& p = request.packet;
      auto& reply = response.packet;
      reply.sequence = p.sequence;
      reply.widget = p.widget;
      reply.lease = p.lease;
      reply.operation = phone_gpu::SharedOp::kReply;
      reply.result = static_cast<uint32_t>(phone_gpu::Result::kFailed);
      {
        base::AutoLock lock(Browser().lock);
        if (Browser().current != connection || !connection->active.load())
          break;
        using Op = phone_gpu::SharedOp;
        if (p.operation == Op::kAllocateImage) {
          if (!phone_gpu::AllocateSharedImage(p.width, p.height, p.prefix[8],
                                               &response)) {
            response.CloseFds();
            reply = phone_gpu::SharedPacket{};
            reply.sequence = p.sequence;
            reply.widget = p.widget;
            reply.operation = Op::kUnavailable;
          }
        } else if (p.operation == Op::kAcquire ||
                   p.operation == Op::kColorProbe) {
          reply.operation = Op::kUnavailable;
          reply.result =
              static_cast<uint32_t>(phone_gpu::SharedBufferAllocatorReady()
                                        ? phone_gpu::Result::kDropped
                                        : phone_gpu::Result::kFailed);
          auto surface = AcquireOhosNativeSurfaceForPhoneGpu(p.widget);
          if ((!surface || !surface->window) &&
              p.operation == Op::kColorProbe &&
              IsOhosNativeSurfaceExpected(p.widget)) {
            // One bounded startup wait, never a per-frame wait. Reacquire
            // under the registry lock to obtain an owned native reference.
            WaitForOhosNativeSurface(p.widget, base::Milliseconds(500));
            surface = AcquireOhosNativeSurfaceForPhoneGpu(p.widget);
          }
          if (p.operation == Op::kColorProbe) {
            LOG(WARNING) << "OHOS phone output: color probe requested mode="
                         << static_cast<uint32_t>(p.color.mode)
                         << " white_millinits=" << p.color.white_millinits
                         << " peak_millinits=" << p.color.peak_millinits
                         << " widget=" << p.widget
                         << " surface=" << (surface && surface->window)
                         << " allocator_ready="
                         << phone_gpu::SharedBufferAllocatorReady();
          }
          if (surface && surface->window) {
            const bool probe = p.operation == Op::kColorProbe;
            auto active_color = colors.find(p.widget);
            const bool no_pending = std::none_of(
                leases.begin(), leases.end(), [&p](const auto& item) {
                  return item.second.packet.widget == p.widget;
                });
            const bool color_valid =
                probe ? no_pending
                      : (p.color_generation == 0
                             ? active_color == colors.end()
                             : active_color != colors.end() &&
                                   active_color->second.generation ==
                                       p.color_generation &&
                                   (active_color->second.color == p.color ||
                                    (no_pending &&
                                     active_color->second.color.mode ==
                                         OhosOutputMode::kHdr10 &&
                                     p.color.mode == OhosOutputMode::kHdr10)) &&
                                   active_color->second.window ==
                                       surface->window);
            const bool hdr_valid =
                p.color.mode == OhosOutputMode::kSdr8 ||
                (!probe || (!OhosOutputColorFailed() &&
                            (p.color.mode != OhosOutputMode::kHdr10 ||
                             OhosSingleDisplaySupportsHdr10())));
            if (probe) {
              LOG(WARNING)
                  << "OHOS phone output: color probe guards color_valid="
                  << color_valid << " hdr_valid=" << hdr_valid
                  << " pending_leases=" << leases.size()
                  << " size=" << surface->bounds.width() << "x"
                  << surface->bounds.height();
            }
            const bool gamut_valid = !probe ||
                p.color.mode != OhosOutputMode::kP3Sdr10 ||
                OhosSingleDisplaySupportsP3();
            if (!color_valid || !hdr_valid || !gamut_valid)
              reply.result = static_cast<uint32_t>(phone_gpu::Result::kFailed);
            if (color_valid && hdr_valid && gamut_valid && leases.size() < 4 &&
                phone_gpu::SharedBufferAllocatorReady() &&
                surface->bounds.width() > 0 && surface->bounds.height() > 0 &&
                surface->bounds.width() <= 8192 &&
                surface->bounds.height() <= 8192 &&
                uint64_t{static_cast<uint32_t>(surface->bounds.width())} *
                        surface->bounds.height() * 4 <=
                    phone_gpu::kMaxFrameBytes &&
                (probe ||
                 (surface->bounds.width() == static_cast<int>(p.width) &&
                  surface->bounds.height() == static_cast<int>(p.height)))) {
              reply.operation = Op::kBuffer;
              reply.width = probe ? surface->bounds.width() : p.width;
              reply.height = probe ? surface->bounds.height() : p.height;
              reply.color = p.color;
              reply.color_generation = probe ? p.sequence : p.color_generation;
              reply.lease = p.sequence;
              auto buffer = std::make_unique<phone_gpu::SharedBufferLease>();
              if (buffer_colors.Prepare(surface->window, p.widget, p.color,
                                        no_pending) &&
                  buffer->Acquire(surface->window, &response)) {
                // A same-format HDR luminance update is bound to this newly
                // acquired lease. Prior queued buffers keep their own metadata.
                if (probe || p.color_generation)
                  colors.insert_or_assign(
                      p.widget, ColorState{p.color, reply.color_generation,
                                           surface->window});
                leases.emplace(reply.lease, Pending{reply, std::move(buffer)});
              } else {
                if (p.color.mode != OhosOutputMode::kSdr8 &&
                    p.color.mode != OhosOutputMode::kP3Sdr10)
                  NoteOhosOutputColorFailure(
                      "NativeBuffer allocation or color metadata");
                response.CloseFds();
                reply = phone_gpu::SharedPacket{};
                reply.sequence = p.sequence;
                reply.widget = p.widget;
                reply.operation = Op::kUnavailable;
                reply.result = static_cast<uint32_t>(
                    p.color.mode != OhosOutputMode::kSdr8 ||
                            !phone_gpu::SharedBufferAllocatorReady()
                        ? phone_gpu::Result::kFailed
                        : phone_gpu::Result::kDropped);
              }
            }
            OH_NativeWindow_NativeObjectUnreference(surface->window);
          }
        } else if (p.operation == Op::kPresent || p.operation == Op::kCancel ||
                   p.operation == Op::kDiscard) {
          auto it = leases.find(p.lease);
          if (it == leases.end() || it->second.packet.widget != p.widget)
            break;
          if (p.operation == Op::kCancel) {
            it->second.buffer->CancelBeforeDrawing();
            reply.result = static_cast<uint32_t>(phone_gpu::Result::kDropped);
          } else if (p.operation == Op::kDiscard) {
            reply.result = static_cast<uint32_t>(phone_gpu::Result::kDropped);
          } else {
            auto surface = AcquireOhosNativeSurfaceForPhoneGpu(p.widget);
            auto active_color = colors.find(p.widget);
            const bool generation_current =
                !p.color_generation ||
                (active_color != colors.end() &&
                 active_color->second.generation == p.color_generation);
            bool current =
                generation_current && p.color == it->second.packet.color &&
                p.color_generation == it->second.packet.color_generation &&
                surface && surface->window == it->second.buffer->window() &&
                surface->bounds.width() ==
                    static_cast<int>(it->second.packet.width) &&
                surface->bounds.height() ==
                    static_cast<int>(it->second.packet.height);
            if (surface && surface->window)
              OH_NativeWindow_NativeObjectUnreference(surface->window);
            if (current && it->second.buffer->Present(request.TakeFence())) {
              reply.result =
                  static_cast<uint32_t>(phone_gpu::Result::kPresented);
              if (!connection->presented.exchange(true))
                LOG(WARNING) << "OHOS phone GPU: first frame presented using "
                                "shared NativeBuffer";
            } else if (!current) {
              // Destructor detaches this allocation; no stale frame is flushed.
              reply.result = static_cast<uint32_t>(phone_gpu::Result::kDropped);
            }
          }
          leases.erase(it);
        } else if (p.operation == Op::kColorReady) {
          const auto found = colors.find(p.widget);
          const bool ready = connection->vulkan_requested.load() &&
                             !Browser().vulkan_failed &&
                             !OhosOutputColorFailed() &&
                             found != colors.end() &&
                             found->second.generation == p.color_generation &&
                             found->second.color == p.color;
          if (ready) {
            if (p.color.mode == OhosOutputMode::kP3Sdr10)
              SetOhosVerifiedPhoneP3Output(true);
            else
              SetOhosVerifiedPhoneHdrOutput(true);
            reply.result = static_cast<uint32_t>(phone_gpu::Result::kPresented);
          }
        } else if (p.operation == Op::kHdrDisplayQuery) {
          uint64_t display_id = 0;
          const bool supported = OhosSingleDisplaySupportsHdr10(
              &display_id, /*log_details=*/!hdr_display_reported);
          hdr_display_reported = true;
          reply.operation = Op::kHdrDisplayState;
          reply.display_id = display_id;
          reply.result =
              static_cast<uint32_t>(supported ? phone_gpu::Result::kPresented
                                              : phone_gpu::Result::kDropped);
        } else if (p.operation == Op::kVulkanState) {
          const bool allowed = !Browser().vulkan_failed &&
                               phone_gpu::SharedBufferAllocatorReady();
          if (allowed)
            connection->vulkan_requested = true;
          reply.result =
              static_cast<uint32_t>(allowed ? phone_gpu::Result::kPresented
                                            : phone_gpu::Result::kDropped);
        } else if (p.operation == Op::kColorFailed) {
          NoteOhosOutputColorFailure("phone allocation/import/metadata");
          connection->color_retry = true;
          reply.result = static_cast<uint32_t>(phone_gpu::Result::kDropped);
        } else if (p.operation == Op::kVulkanFailed) {
          Browser().vulkan_failed = true;
          SetOhosVerifiedPhoneHdrOutput(false);
          reply.result = static_cast<uint32_t>(phone_gpu::Result::kDropped);
          LOG(ERROR)
              << "OHOS phone Vulkan: failed; subsequent GPU generations use GL";
        } else if (p.operation == Op::kTiming) {
          if (p.width >= 30 && p.width <= 120)
            timing.SetPreferredFrameInterval(base::Seconds(1.0 / p.width));
          reply.result = static_cast<uint32_t>(phone_gpu::Result::kDropped);
        } else {
          break;
        }
      }
      base::TimeTicks timebase;
      base::TimeDelta interval;
      if (timing.GetVSyncParametersIfAvailable(&timebase, &interval)) {
        reply.timebase_us = timebase.since_origin().InMicroseconds();
        reply.interval_us = interval.InMicroseconds();
      }
      if (!phone_gpu::SendSharedMessage(connection->socket.get(), response))
        break;
      continue;
    }
    phone_gpu::Frame frame;
    int received = -1;
    if (!phone_gpu::ReceiveFrame(connection->socket.get(), &frame, &received,
                                 200)) {
      if (errno == ETIMEDOUT)
        continue;
      break;
    }
    base::ScopedFD buffer_fd(received);
    if (frame.sequence <= last_sequence)
      break;
    last_sequence = frame.sequence;
    phone_gpu::Reply reply;
    reply.sequence = frame.sequence;
    reply.result = phone_gpu::Result::kDropped;
    {
      base::AutoLock lock(Browser().lock);
      if (Browser().current != connection || !connection->active.load())
        break;
      // Acquired under the registry lock, so XComponent destruction cannot
      // free the window between the lookup and taking our native reference.
      auto surface = AcquireOhosNativeSurfaceForPhoneGpu(frame.widget);
      if (surface && surface->window) {
        // A frame queued before resize must not reset the new window's size.
        if (surface->bounds.width() == static_cast<int>(frame.width) &&
            surface->bounds.height() == static_cast<int>(frame.height)) {
          reply.result = phone_gpu::Result::kFailed;
          if (phone_gpu::ReadPixels(received, frame, &pixels) &&
              phone_gpu::Present(surface->window, frame, pixels.data())) {
            reply.result = phone_gpu::Result::kPresented;
          }
        }
        OH_NativeWindow_NativeObjectUnreference(surface->window);
      }
    }
    if (reply.result == phone_gpu::Result::kPresented &&
        !connection->presented.exchange(true)) {
      LOG(WARNING) << "OHOS phone GPU: first frame presented widget="
                   << frame.widget << " size=" << frame.width << "x"
                   << frame.height;
    }
    if (!phone_gpu::SendReply(connection->socket.get(), reply))
      break;
  }
  {
    base::AutoLock lock(Browser().lock);
    if (Browser().current == connection)
      SetOhosVerifiedPhoneHdrOutput(false);
    if (connection->vulkan_requested.load() && !connection->color_retry.load())
      Browser().vulkan_failed = true;
    leases.clear();
    buffer_colors.Reset();
    connection->retired.Signal();
  }
  connection->active = false;
  shutdown(connection->socket.get(), SHUT_RDWR);
  LOG(WARNING) << "OHOS phone GPU: frame channel closed";
}
}  // namespace

base::ProcessId LaunchOhosPhoneGpuChild(
    const std::string& params,
    const std::vector<std::pair<int, int>>& fds) {
  if (fds.size() >= 16)
    return base::kNullProcessId;
  for (const auto& mapping : fds) {
    if (mapping.second == phone_gpu::kChannelDescriptor)
      return base::kNullProcessId;
  }
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair))
    return base::kNullProcessId;
  base::ScopedFD browser_fd(pair[0]), child_fd(pair[1]);
  auto mappings = fds;
  mappings.emplace_back(child_fd.get(), phone_gpu::kChannelDescriptor);
  const auto pid = base::internal::LaunchOhosForkChild(params, mappings);
  if (pid <= 0)
    return base::kNullProcessId;
  auto connection = std::make_shared<Connection>(std::move(browser_fd));
  {
    base::AutoLock lock(Browser().lock);
    if (Browser().current) {
      connection->predecessor = Browser().current;
      if (Browser().current->vulkan_requested.load() &&
          !Browser().current->color_retry.load())
        Browser().vulkan_failed = true;
      Browser().current->active = false;
      shutdown(Browser().current->socket.get(), SHUT_RDWR);
    }
    Browser().current = connection;
    SetOhosVerifiedPhoneHdrOutput(false);
  }
  base::ThreadPool::PostTask(FROM_HERE,
                             {base::MayBlock(), base::WithBaseSyncPrimitives()},
                             base::BindOnce(&ReceiveFrames, connection));
  LOG(WARNING) << "OHOS phone GPU: fork child pid=" << pid
               << " with shared-buffer negotiation and readback fallback";
  return pid;
}
bool OhosPhoneGpuHasPresented() {
  base::AutoLock lock(Browser().lock);
  return Browser().current && Browser().current->active.load() &&
         Browser().current->presented.load();
}
bool InitializeOhosPhoneGpuChild(int received_fd) {
  if (Child().initialized)
    return false;
  Child().socket.reset(
      base::internal::DuplicateOhosChildDescriptor(received_fd));
  Child().initialized = Child().socket.is_valid();
  return Child().initialized;
}
bool IsOhosPhoneGpuChild() {
  return Child().initialized;
}
int OhosPhoneGpuMinimumBufferFd() {
  base::AutoLock lock(Child().lock);
  return Child().socket.get() + 1;
}
phone_gpu::Result SubmitOhosPhoneGpuFrame(phone_gpu::Frame frame,
                                          int buffer_fd) {
  auto& state = Child();
  base::AutoLock lock(state.lock);
  frame.sequence = ++state.sequence;
  phone_gpu::Result result = phone_gpu::Result::kFailed;
  if (!state.socket.is_valid() ||
      !phone_gpu::SendFrame(state.socket.get(), frame, buffer_fd) ||
      !phone_gpu::ReceiveReply(state.socket.get(), frame.sequence, &result)) {
    // Never reuse a buffer while the peer may still be reading it. Failed
    // submission makes the surface return SWAP_FAILED and Chromium recover.
    if (state.socket.is_valid())
      shutdown(state.socket.get(), SHUT_RDWR);
    state.socket.reset();
    LOG(ERROR) << "OHOS phone GPU: presentation channel failed";
    return phone_gpu::Result::kFailed;
  }
  return result;
}
namespace {
bool ExchangeShared(phone_gpu::SharedMessage* request,
                    phone_gpu::SharedMessage* response) {
  auto& state = Child();
  base::AutoLock lock(state.lock);
  request->packet.sequence = ++state.sequence;
  if (!state.socket.is_valid() ||
      !phone_gpu::SendSharedMessage(state.socket.get(), *request) ||
      !phone_gpu::ReceiveSharedMessage(state.socket.get(), response,
                                       phone_gpu::kFrameTimeoutMs) ||
      response->packet.sequence != request->packet.sequence ||
      response->packet.widget != request->packet.widget) {
    if (state.socket.is_valid())
      shutdown(state.socket.get(), SHUT_RDWR);
    state.socket.reset();
    return false;
  }
  if (response->packet.interval_us >= 4000 &&
      response->packet.interval_us <= 100000 &&
      response->packet.timebase_us > 0) {
    state.timebase =
        base::TimeTicks() + base::Microseconds(response->packet.timebase_us);
    state.interval = base::Microseconds(response->packet.interval_us);
    state.timing_updated = base::TimeTicks::Now();
  }
  return true;
}
}  // namespace
bool AllocateOhosPhoneGpuImage(uint32_t width,
                               uint32_t height,
                               uint32_t native_format,
                               phone_gpu::SharedMessage* response) {
  if (!IsOhosPhoneGpuChild() || !phone_gpu::IsSharedImageFormat(native_format))
    return false;
  phone_gpu::SharedMessage request;
  request.packet.operation = phone_gpu::SharedOp::kAllocateImage;
  request.packet.widget = 1;  // Channel endpoint, not a window allocation.
  request.packet.width = width;
  request.packet.height = height;
  request.packet.prefix[8] = native_format;
  return ExchangeShared(&request, response) &&
         response->packet.operation == phone_gpu::SharedOp::kImage &&
         response->packet.width == width && response->packet.height == height &&
         response->packet.prefix[8] == native_format;
}
bool QueryOhosPhoneHdrDisplay(uint64_t* display_id) {
  if (!IsOhosPhoneGpuChild())
    return false;
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kHdrDisplayQuery;
  request.packet.widget = 1;  // Connection-scoped; not a surface request.
  if (!ExchangeShared(&request, &response) ||
      response.packet.operation != phone_gpu::SharedOp::kHdrDisplayState ||
      response.packet.result !=
          static_cast<uint32_t>(phone_gpu::Result::kPresented)) {
    return false;
  }
  if (display_id)
    *display_id = response.packet.display_id;
  return true;
}
bool AllowOhosPhoneVulkan() {
  if (!IsOhosPhoneGpuChild())
    return false;
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kVulkanState;
  request.packet.widget = 1;  // Connection-scoped control request.
  return ExchangeShared(&request, &response) &&
         response.packet.operation == phone_gpu::SharedOp::kReply &&
         response.packet.result ==
             static_cast<uint32_t>(phone_gpu::Result::kPresented);
}
void ReportOhosPhoneVulkanFailure() {
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kVulkanFailed;
  request.packet.widget = 1;
  ExchangeShared(&request, &response);
}
void ReportOhosPhoneOutputColorFailure() {
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kColorFailed;
  request.packet.widget = 1;
  ExchangeShared(&request, &response);
}
bool ReportOhosPhoneOutputColorReady(uint64_t widget,
                                  const OhosOutputColor& color,
                                  uint64_t color_generation) {
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kColorReady;
  request.packet.widget = widget;
  request.packet.color = color;
  request.packet.color_generation = color_generation;
  return ExchangeShared(&request, &response) &&
         response.packet.operation == phone_gpu::SharedOp::kReply &&
         response.packet.result ==
             static_cast<uint32_t>(phone_gpu::Result::kPresented);
}
bool ProbeOhosPhoneOutputColor(uint64_t widget,
                               const OhosOutputColor& color,
                               phone_gpu::SharedMessage* response) {
  phone_gpu::SharedMessage request;
  request.packet.operation = phone_gpu::SharedOp::kColorProbe;
  request.packet.widget = widget;
  request.packet.color = color;
  request.packet.color_generation = 1;  // Request; server assigns sequence ID.
  return ExchangeShared(&request, response) &&
         response->packet.operation == phone_gpu::SharedOp::kBuffer &&
         response->packet.color == color && response->packet.color_generation;
}
bool AcquireOhosPhoneGpuBuffer(phone_gpu::Frame frame,
                               phone_gpu::SharedMessage* response,
                               const OhosOutputColor& color,
                               uint64_t color_generation) {
  phone_gpu::SharedMessage request;
  request.packet.widget = frame.widget;
  request.packet.width = frame.width;
  request.packet.height = frame.height;
  request.packet.color = color;
  request.packet.color_generation = color_generation;
  return ExchangeShared(&request, response) &&
         response->packet.operation == phone_gpu::SharedOp::kBuffer;
}
phone_gpu::Result PresentOhosPhoneGpuBuffer(uint64_t widget,
                                            uint64_t lease,
                                            int completion_fd,
                                            const OhosOutputColor& color,
                                            uint64_t color_generation) {
  if (completion_fd < -1)
    return phone_gpu::Result::kFailed;
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kPresent;
  request.packet.color = color;
  request.packet.color_generation = color_generation;
  request.packet.widget = widget;
  request.packet.lease = lease;
  // -1 means already complete (successful Vulkan SYNC_FD export).
  if (completion_fd >= 0) {
    request.packet.has_fence = 1;
    request.fds.push_back(completion_fd);
  }
  if (!ExchangeShared(&request, &response) ||
      response.packet.operation != phone_gpu::SharedOp::kReply ||
      response.packet.lease != lease)
    return phone_gpu::Result::kFailed;
  return static_cast<phone_gpu::Result>(response.packet.result);
}
void DiscardOhosPhoneGpuBuffer(uint64_t widget, uint64_t lease) {
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kDiscard;
  request.packet.widget = widget;
  request.packet.lease = lease;
  ExchangeShared(&request, &response);
}
bool CancelOhosPhoneGpuBuffer(uint64_t widget, uint64_t lease) {
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kCancel;
  request.packet.widget = widget;
  request.packet.lease = lease;
  return ExchangeShared(&request, &response) &&
         response.packet.operation == phone_gpu::SharedOp::kReply &&
         response.packet.lease == lease;
}
bool GetOhosPhoneGpuTiming(uint64_t widget,
                           base::TimeTicks* timebase,
                           base::TimeDelta* interval) {
  auto& state = Child();
  {
    base::AutoLock lock(state.lock);
    if (!state.interval.is_zero() &&
        base::TimeTicks::Now() - state.timing_updated <
            base::Milliseconds(100)) {
      *timebase = state.timebase;
      *interval = state.interval;
      return true;
    }
  }
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kTiming;
  request.packet.widget = widget;
  ExchangeShared(&request, &response);
  base::AutoLock lock(state.lock);
  *timebase = state.timebase;
  *interval = state.interval;
  return !interval->is_zero();
}
void SetOhosPhoneGpuFrameInterval(uint64_t widget, base::TimeDelta interval) {
  phone_gpu::SharedMessage request, response;
  request.packet.operation = phone_gpu::SharedOp::kTiming;
  request.packet.widget = widget;
  request.packet.width =
      interval.is_positive()
          ? static_cast<uint32_t>(
                std::clamp(std::lround(1.0 / interval.InSecondsF()), 30l, 120l))
          : 120;
  ExchangeShared(&request, &response);
}
}  // namespace ui
