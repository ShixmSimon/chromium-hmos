// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_SESSION_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_SESSION_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ui::ohos_overlay {

struct Rect {
  float x = 0, y = 0, width = 0, height = 0;
};
enum class Format { kRgba8, kBgra8, kRgb10A2, kNv12, kP010 };
enum class Transfer { kSdr, kPq, kHlg };
struct Color {
  Transfer transfer = Transfer::kSdr;
  uint32_t native_space = 0;
  // Bytes are producer metadata, not a reconstruction from codec/profile.
  std::vector<uint8_t> hdr_type;
  std::vector<uint8_t> hdr_static;
};

// An owned read lease, NOT merely an allocation reference. The implementation
// keeps the decoder ConsumerSurface acquisition / GPU read alive until it is
// destroyed. A cached NativePixmap import alone is insufficient.
class ReadLease {
 public:
  virtual ~ReadLease() = default;
  virtual uint64_t allocation_id() const = 0;
  virtual void* native_buffer() const = 0;
  virtual int BorrowAcquireFence() const = 0;
  virtual bool ProducerReady() const = 0;
};

struct Plane {
  std::unique_ptr<ReadLease> lease;
  // Same immutable content as this plane's currently retained allocation.
  // Geometry may change, but no new producer fence/write is allowed.
  bool reuse = false;
  Format format = Format::kRgba8;
  uint32_t width = 0, height = 0;
  Rect source, destination;
  Color color;
};
struct Frame {
  Plane primary;
  std::optional<Plane> video;
  // Pixels in the video destination are transparent in this primary. A frame
  // without an underlay must contain the composited video again.
  bool primary_has_video_hole = false;
};

// The backend owns one pair of nodes and one transaction thread. It must not
// submit a primary through NativeWindowFlushBuffer independently of the video.
// Prepare validates/imports every resource and reserves the transaction marker
// WITHOUT staging RS commands. Failed preparation may be discarded safely.
// Commit is called only after all allocation routing is installed. If a server
// failure cannot be rolled back, retain every lease and report a fatal state;
// a timeout or transaction callback must never manufacture a buffer release.
class Backend {
 public:
  class Prepared {
   public:
    virtual ~Prepared() = default;
  };
  struct Release {
    uint64_t submission = 0;
    uint64_t allocation = 0;
    unsigned plane = 0;
    bool server_returned = false;
    bool release_fence_signaled = false;
  };
  virtual ~Backend() = default;
  virtual bool Supports(const Frame&) const = 0;
  virtual bool ReadyForFrame() const { return true; }
  virtual std::unique_ptr<Prepared> Prepare(uint64_t submission,
                                            const Frame&) = 0;
  virtual void Commit(std::unique_ptr<Prepared>) = 0;
  virtual std::vector<Release> PollReleases() = 0;
  virtual bool CommandsProcessed(uint64_t submission) const = 0;
  // Only called after the latest buffer batch marker was acknowledged.
  // Hide + detach + CleanBuffer + flush, then GoBackground + flush.
  virtual void BeginDrain() = 0;
  virtual bool DrainCommandsProcessed() const = 0;
  virtual void DestroyLayers() = 0;
};

enum class SubmitResult {
  kAccepted,
  kUnsupported,
  kBusy,
  kPrepareFailed,
  kClosed
};
enum class State { kRunning, kWaitingForCommands, kDraining, kClosed };

// Shared by GL/Vulkan presenters; no graphics-backend dependencies. All calls
// on the owning transaction sequence. Backend callbacks only queue events.
// Close is asynchronous: the owner must retain Session and keep Poll() running
// until closed(), including navigation, GPU loss, and hidden-window teardown.
class Session {
 public:
  explicit Session(Backend& backend, size_t max_frames = 4);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  SubmitResult Submit(Frame& frame);
  void Poll();
  void Close();
  State state() const { return state_; }
  bool closed() const { return state_ == State::kClosed; }
  size_t outstanding_allocations() const { return leases_.size(); }
  uint64_t last_submission() const { return last_submission_; }
  static bool Valid(const Frame& frame);
  bool CommandsProcessed() const {
    return backend_.CommandsProcessed(last_submission_);
  }

 private:
  struct Held {
    uint64_t submission;
    unsigned plane;
    bool released = false;
    bool fence_ready = false;
    std::unique_ptr<ReadLease> lease;
    Format format;
    uint32_t width, height;
    Color color;
  };
  Backend& backend_;
  const size_t max_frames_;
  State state_ = State::kRunning;
  uint64_t last_submission_ = 0;
  std::map<uint64_t, Held> leases_;
  uint64_t current_[2] = {};
  // Release redundant references on the next Poll, after C callers have armed
  // their ACCEPTED callbacks. The original retained read still protects pixels.
  std::vector<std::unique_ptr<ReadLease>> redundant_;
};
}  // namespace ui::ohos_overlay
#endif
