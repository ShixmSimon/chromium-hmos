#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <iostream>
#include "ui/ozone/platform/ohos/ohos_video_buffer_parcel.h"

int main() {
  std::vector<uint32_t> p(14);
  p[1] = p[13] = 1;
  p[4] = 1280;
  p[5] = 2560;
  p[6] = 736;
  p[7] = 2826240;
  p[8] = 35;
  auto valid = [&](const auto& q, size_t tail = 20, size_t fds = 1) {
    return ui::ValidOhosVideoBufferParcel(q, tail, fds, 1280, 736, 35, 2);
  };
  assert(valid(p));
  auto bad = p;
  bad.resize(13);
  assert(!valid(bad));
  bad = p;
  bad[1] = 2;
  assert(!valid(bad));
  bad = p;
  bad[2] = 7;
  assert(!valid(bad, 20, 8));
  bad = p;
  bad[3] = 257;
  assert(!valid(bad, 1048));
  bad = p;
  bad[4] = 1279;
  assert(!valid(bad));
  bad = p;
  bad[5] = 2559;
  assert(!valid(bad));
  bad = p;
  bad[5] = UINT32_MAX;
  assert(!valid(bad));
  bad = p;
  bad[7] = 2826239;
  assert(!valid(bad));
  bad = p;
  bad[7] = UINT32_MAX;
  assert(!valid(bad));
  bad = p;
  bad[8] = 12;
  assert(!valid(bad));
  bad = p;
  bad[13] = 0;
  assert(!valid(bad));
  assert(!valid(p, 19));
  assert(!valid(p, 20, 0));
  assert(!ui::ValidOhosVideoBufferParcel(p, 20, 1, 0, 736, 35, 2));
  assert(!ui::ValidOhosVideoBufferParcel(p, 20, 1, 8193, 736, 35, 2));
  assert(!ui::ValidOhosVideoBufferParcel(p, 20, 1, 1280, 736, 35, 4));
  p[5] = 1280;
  p[7] = 1413120;
  p[8] = 24;
  assert(ui::ValidOhosVideoBufferParcel(p, 20, 1, 1280, 736, 24, 1));
  // A renderer/frame release must not release the decoder lease while a GPU
  // import retains its duplicated endpoint. Only the last endpoint signals EOF.
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
  int gpu_import = dup(pair[1]);
  assert(gpu_import >= 0);
  pollfd watch{pair[0], POLLIN | POLLHUP, 0};
  close(pair[1]);
  assert(poll(&watch, 1, 0) == 0);
  close(gpu_import);
  assert(poll(&watch, 1, 100) == 1);
  char c;
  assert(read(pair[0], &c, 1) == 0);
  close(pair[0]);
  std::cout
      << "PASS native video parcel bounds and last-import lease lifetime\n";
}
