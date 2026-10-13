// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_TRANSPORT_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_PHONE_GPU_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ui::phone_gpu {

// Private SOCK_SEQPACKET channel; never an OHIPCParcel or a native pointer.
// RGBA8 rows are bottom-up, tightly packed, and carried in a size-checked
// memfd. One outstanding frame per connection. The producer must not modify the
// mapping until the matching reply, and must abandon the channel on timeout.
inline constexpr int kChannelDescriptor = 190;
inline constexpr size_t kMaxFrameBytes = 64 * 1024 * 1024;
inline constexpr int kFrameTimeoutMs = 2000;
enum class Result : uint32_t { kPresented = 0, kDropped = 1, kFailed = 2 };

inline constexpr uint32_t kTopLeftRows = 1;
struct Frame {
  uint32_t magic = 0x50475031;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t reserved = 0;  // kTopLeftRows or zero (legacy bottom-up).
  uint64_t widget = 0;
  uint64_t sequence = 0;
};
struct Reply {
  uint32_t magic = 0x50474131;
  Result result = Result::kFailed;
  uint64_t sequence = 0;
};

size_t FrameBytes(const Frame& frame);
bool SendFrame(int socket, const Frame& frame, int buffer_fd);
// On success the caller owns *buffer_fd; on failure no received FD leaks.
bool ReceiveFrame(int socket, Frame* frame, int* buffer_fd, int timeout_ms);
bool SendReply(int socket, const Reply& reply);
bool ReceiveReply(int socket, uint64_t sequence, Result* result);
bool ValidateBuffer(int fd, size_t bytes);
// pread into browser-owned memory: a peer closing/truncating its shared file
// must never cause SIGBUS in the browser. Device kernels may ignore seals.
bool ReadPixels(int fd, const Frame& frame, std::vector<uint8_t>* pixels);

class Buffer {
 public:
  Buffer() = default;
  ~Buffer();
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  // minimum_fd must exceed the fork snapshot's inherited descriptor range.
  bool Resize(size_t bytes, int minimum_fd);
  void* data() const { return data_; }
  int fd() const { return fd_; }

 private:
  void Reset();
  int fd_ = -1;
  void* data_ = nullptr;
  size_t bytes_ = 0;
};

}  // namespace ui::phone_gpu
#endif
