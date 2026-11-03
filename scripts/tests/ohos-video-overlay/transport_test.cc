#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include "ui/ozone/platform/ohos/ohos_overlay_transport.h"
using namespace ui::ohos_overlay;
void Fill(WirePacket& p) {
  p.sequence = 1;
  p.has_video = 1;
  p.video_token = 9;
  p.fd_count = 1;
  p.primary.content = 1;
  p.primary.source = p.primary.destination = {0, 0, 640, 480};
  p.video.content = 9;
  p.video.source = {0, 0, 1920, 1080};
  p.video.destination = {20, 40, 600, 338};
  p.prefix = {123, 1, 0, 0, 640, 2560, 480, 640 * 480 * 4, 12, 0, 0, 0, 0, 1};
}
int main() {
  WireMessage source;
  Fill(source.packet);
  assert(ValidWirePacket(source.packet, 1));
  auto invalid = source.packet;
  invalid.video.destination.width = 700;
  assert(!ValidWirePacket(invalid, 1));
  invalid = source.packet;
  invalid.video.source.x = std::numeric_limits<float>::quiet_NaN();
  assert(!ValidWirePacket(invalid, 1));
  invalid = source.packet;
  invalid.prefix[2] = 7;
  assert(!ValidWirePacket(invalid, 8));
  invalid = source.packet;
  invalid.prefix[8] = 35;
  assert(!ValidWirePacket(invalid, 1));
  invalid = source.packet;
  invalid.video.reuse = 1;
  invalid.video.has_fence = 1;
  assert(!ValidWirePacket(invalid, 2));
  invalid = source.packet;
  invalid.has_video = 0;
  assert(!ValidWirePacket(invalid, 1));
  invalid = source.packet;
  invalid.operation = WireOp::kReleased;
  assert(!ValidWirePacket(invalid, 0));
  int sockets[2], pipes[2];
  assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
  assert(!pipe(pipes));
  source.fds.push_back(pipes[0]);
  assert(SendWireMessage(sockets[0], source));
  WireMessage dest;
  assert(ReceiveWireMessage(sockets[1], &dest));
  assert(dest.fds.size() == 1 && (fcntl(dest.fds[0], F_GETFD) & FD_CLOEXEC));
  source.CloseFds();  // Received FD independently owns the resource.
  assert(write(pipes[1], "x", 1) == 1);
  char byte = 0;
  assert(read(dest.fds[0], &byte, 1) == 1 && byte == 'x');
  dest.CloseFds();
  assert(!ReceiveWireMessage(sockets[1], &dest) &&
         (errno == EAGAIN || errno == EWOULDBLOCK));
  // Bypass the sender validator: malformed/truncated input never escapes.
  char garbage[sizeof(WirePacket) + 1]{};
  assert(send(sockets[0], garbage, sizeof(garbage), 0) == sizeof(garbage));
  assert(!ReceiveWireMessage(sockets[1], &dest) && errno == EPROTO &&
         dest.fds.empty());
  // A reused primary transfers geometry only, with no native parcel or fence.
  WireMessage reuse;
  Fill(reuse.packet);
  reuse.packet.primary.reuse = 1;
  reuse.packet.fd_count = 0;
  reuse.packet.prefix = {};
  assert(ValidWirePacket(reuse.packet, 0));
  assert(SendWireMessage(sockets[0], reuse));
  assert(ReceiveWireMessage(sockets[1], &dest));
  close(sockets[0]);
  assert(!ReceiveWireMessage(sockets[1], &dest) && errno == ECONNRESET);
  close(sockets[1]);
  close(pipes[1]);
  puts(
      "PASS overlay wire geometry, parcel bounds, reuse, owned descriptor "
      "transfer, malformed packets and disconnect");
}
