// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "ui/ozone/platform/ohos/ohos_video_graphite_metadata.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <unistd.h>

using ui::OhosVideoGraphiteMetadata;
OhosVideoGraphiteMetadata Sample() {
  return {{{OhosVideoGraphiteMetadata::kMagic, 1, 1920, 1080, 0,
            0xfedcba98, 0x12345678, 4, 1, 0, 1, 1}}};
}
int main() {
  auto valid = Sample();
  assert(valid.IsValid(1920, 1080));
  for (size_t index : {0u, 1u, 2u, 3u, 7u, 8u, 9u, 10u, 11u}) {
    auto invalid = valid;
    invalid.words[index] = 0xffffffff;
    if (index == 11) invalid.words[index] = 0;
    assert(!invalid.IsValid(1920, 1080));
  }
  auto no_format = valid;
  no_format.words[4] = no_format.words[5] = no_format.words[6] = 0;
  assert(!no_format.IsValid(1920, 1080));
  assert(!valid.IsValid(1080, 1920));

  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  // Metadata can be fragmented in SOCK_STREAM. A complete reply must preserve
  // the 64-bit opaque external-format identifier, with no pointer transport.
  std::thread producer([&] {
    const auto* bytes = reinterpret_cast<const unsigned char*>(valid.words.data());
    for (size_t i = 0; i < sizeof(valid.words); ++i)
      assert(send(pair[1], bytes + i, 1, MSG_NOSIGNAL) == 1);
  });
  OhosVideoGraphiteMetadata decoded;
  assert(ui::ReceiveOhosVideoGraphiteMetadata(pair[0], &decoded));
  producer.join();
  assert(decoded.words == valid.words);
  assert((static_cast<uint64_t>(decoded.words[6]) << 32 | decoded.words[5]) ==
         0x12345678fedcba98ull);
  // The decoder slot must remain retained after metadata is delivered.
  pollfd descriptor{pair[0], POLLIN | POLLHUP, 0};
  assert(poll(&descriptor, 1, 0) == 0);
  close(pair[1]);
  assert(poll(&descriptor, 1, 100) == 1 && (descriptor.revents & POLLHUP));
  close(pair[0]);

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(ui::SendOhosVideoGraphiteMetadata(pair[1], valid));
  assert(ui::ReceiveOhosVideoGraphiteMetadata(pair[0], &decoded));
  assert(decoded.words == valid.words);
  close(pair[0]);
  // A dead producer must fail without SIGPIPE killing the GPU process.
  assert(!ui::SendOhosVideoGraphiteMetadata(pair[1], valid));
  close(pair[1]);

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(send(pair[1], valid.words.data(), 7, MSG_NOSIGNAL) == 7);
  close(pair[1]);
  assert(!ui::ReceiveOhosVideoGraphiteMetadata(pair[0], &decoded));
  close(pair[0]);

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  const auto started = std::chrono::steady_clock::now();
  assert(!ui::ReceiveOhosVideoGraphiteMetadata(pair[0], &decoded));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  assert(elapsed >= std::chrono::seconds(4) && elapsed < std::chrono::seconds(7));
  close(pair[0]); close(pair[1]);
  std::puts("Graphite video metadata: validation, fragmentation, 64-bit ID, lease EOF, dead peer, truncation, deadline PASS");
}
