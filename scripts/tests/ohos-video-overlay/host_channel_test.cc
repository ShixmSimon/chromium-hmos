#include <cassert>
#include <cstdio>
#if defined(__APPLE__)
#include <sys/socket.h>
// Darwin has no AF_UNIX SOCK_SEQPACKET. This test exercises connection/HUP
// lifecycle only; the transport is also tested with real SOCK_SEQPACKET on Linux/OHOS.
int HostTestSocketPair(int domain, int, int protocol, int* fds) {
  return socketpair(domain, SOCK_STREAM, protocol, fds);
}
#define socketpair HostTestSocketPair
#endif
#include "ui/ozone/platform/ohos/ohos_overlay_host_channel.h"
#if defined(__APPLE__)
#undef socketpair
#endif
using ui::ohos_overlay::HostChannel;
int main() {
  HostChannel channel;
  assert(!channel.Activate());
  int first = channel.Connect();
  assert(first >= 0 && channel.Connect() == -1);
  assert(channel.Activate());
  const int original = channel.socket();
  assert(!channel.Activate() && channel.Connect() == -1);
  close(first);  // GPU crash, before the UI timer notices the closed peer.
  assert(channel.PeerLost());
  int next = channel.Connect();
  assert(next >= 0 && channel.Connect() == -1);
  assert(!channel.Activate() && channel.socket() == original);
  channel.Disconnect();
  // While the old RS session drains, old releases cannot reach the new peer.
  char byte = 0;
  assert(channel.socket() == -1);
  assert(recv(next, &byte, 1, MSG_DONTWAIT) == -1);
  // The caller activates only after RS releases/fences completed.
  assert(channel.Activate());
  byte = 'n';
  assert(send(channel.socket(), &byte, 1, 0) == 1);
  assert(recv(next, &byte, 1, 0) == 1 && byte == 'n');
  close(next);
  next = channel.Connect();
  assert(next >= 0);
  close(next);  // Replacement GPU also dies before the drain completes.
  int third = channel.Connect();
  assert(third >= 0);
  channel.Stop();  // Web container detached with a replacement pending.
  assert(channel.Connect() == -1 && !channel.Activate());
  assert(recv(third, &byte, 1, 0) == 0);
  close(third);
  channel.Disconnect();
  puts("PASS GPU reconnect queue, generation isolation, repeated crash and detach");
}
