// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/process/fork_server_ohos.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <thread>

#include "AbilityKit/native_child_process.h"

namespace base::internal {
namespace {

// Deliberately libc/STL-only: the single-threaded server must not initialize
// Chromium, use its logging, start task runners, or enter the browser runtime.
constexpr uint32_t kMagic = 0x4f464b31;
constexpr size_t kMaxFds = 16;
constexpr size_t kMaxParams = 64 * 1024;
constexpr size_t kMaxChildren = 64;
constexpr int kRpcTimeoutMs = 5000;
int trace_fd = -1;
struct stat trace_file = {};
std::array<int, 1024> trace_original_fds{};
size_t trace_original_count = 0;
bool trace_truncated = false;

void CaptureTraceDescriptors() {
  if (trace_fd < 0)
    return;
  trace_original_count = 0;
  trace_truncated = false;
  DIR* directory = opendir("/proc/self/fd");
  if (!directory)
    return;
  while (dirent* entry = readdir(directory)) {
    if (entry->d_name[0] == '.')
      continue;
    char* end = nullptr;
    long fd = strtol(entry->d_name, &end, 10);
    if (*end || fd < 0 || fd >= INT_MAX || fd == dirfd(directory))
      continue;
    if (trace_original_count == trace_original_fds.size()) {
      trace_truncated = true;
      break;
    }
    trace_original_fds[trace_original_count++] = static_cast<int>(fd);
  }
  closedir(directory);
}
enum class Operation : uint32_t { kHello, kLaunch, kStatus, kSignal, kStop };
struct Request {
  uint32_t magic = kMagic;
  Operation operation = Operation::kHello;
  uint32_t bytes = 0;
  uint32_t fd_count = 0;
  int32_t pid = 0;
  int32_t signal = 0;
  int32_t destinations[kMaxFds] = {};
  char params[kMaxParams] = {};
};
constexpr size_t kRequestHeader = offsetof(Request, params);
// One lock-free word keeps stage and detail coherent across processes. Only
// the owning child writes; the server snapshots it before reusing the slot.
static_assert(std::atomic<uint64_t>::is_always_lock_free);
struct ChildProgress {
  std::atomic<uint64_t> value{0};
};
ChildProgress* child_progress = nullptr;
int child_minimum_fd = 3;
uint64_t SnapshotProgress(const ChildProgress* progress) {
  return progress->value.load(std::memory_order_relaxed);
}
void SetProgress(OhosForkChildStatus& state, uint64_t value) {
  state.stage = static_cast<uint32_t>(value >> 32);
  state.detail = static_cast<int32_t>(value);
}
struct ExitRecord {
  int32_t pid = 0;
  int32_t status = -1;
  uint64_t progress = 0;
};
struct Reply {
  uint32_t magic = kMagic;
  int32_t error = 0;
  int32_t pid = 0;
  int32_t progress_pid = 0;
  uint64_t progress = 0;
  uint32_t exited_count = 0;
  ExitRecord exited[kMaxChildren] = {};
};
struct ServerChild {
  pid_t pid = 0;
  ChildProgress* progress = nullptr;
};
struct Client {
  const pid_t owner = getpid();
  std::mutex mutex;
  int socket = -1;
  pid_t server = -1;
  bool attempted = false;
  std::map<pid_t, OhosForkChildStatus> children;
};
Client& GetClient() {
  // Intentionally leaked; renderer snapshots never run browser destructors.
  static Client* client = new Client;
  // A renderer must never touch the browser snapshot's locked RPC mutex.
  if (client->owner != getpid())
    client = new Client;
  return *client;
}

bool WaitReadable(int fd, int milliseconds) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(milliseconds);
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    pollfd descriptor{fd, POLLIN, 0};
    int result = poll(&descriptor, 1, left > 0 ? static_cast<int>(left) : 0);
    if (result >= 0) {
      if (!result)
        errno = ETIMEDOUT;
      return result > 0 && (descriptor.revents & (POLLIN | POLLHUP));
    }
    if (errno != EINTR)
      return false;
  }
}

void CloseFds(std::vector<int>& fds) {
  for (int fd : fds)
    close(fd);
  fds.clear();
}

bool IsLoaderRelroFd(int directory_fd, const char* name, int fd) {
  // OHOS musl caches this descriptor in its inherited prelink state. Closing
  // or renumbering it lets a later dlopen mmap an unrelated, reused FD at a
  // RELRO offset (and can SIGBUS beyond that file's end). Keep the original
  // descriptor, not just the existing mappings or a duplicate at another FD.
  constexpr char kRelroTarget[] = "/memfd:relro_cache (deleted)";
  char target[sizeof(kRelroTarget)];
  ssize_t size = readlinkat(directory_fd, name, target, sizeof(target));
  if (size != sizeof(kRelroTarget) - 1 ||
      memcmp(target, kRelroTarget, sizeof(kRelroTarget) - 1) != 0)
    return false;
  // Phone policy can deny getattr (fstat returns EACCES) on this inherited
  // system memfd while readlink, fcntl and mmap remain usable. Metadata access
  // is not a liveness test. The exact procfs target identifies the cache; keep
  // its live original descriptor even when metadata cannot be queried.
  return fcntl(fd, F_GETFD) >= 0;
}

// Close inherited application channels before the server can fork children.
// Loader-owned RELRO cache FDs must survive at their original numbers.
// No broad munmap of Ark/anonymous memory: mapped libraries may still use it.
bool CloseInheritedFds(int keep, int& minimum_received_fd) {
  DIR* directory = opendir("/proc/self/fd");
  if (!directory)
    return false;
  int directory_fd = dirfd(directory);
  while (dirent* entry = readdir(directory)) {
    if (entry->d_name[0] == '.')
      continue;
    char* end = nullptr;
    long fd = strtol(entry->d_name, &end, 10);
    if (*end == '\0' && fd >= 0 && fd < INT_MAX) {
      if (fd >= minimum_received_fd)
        minimum_received_fd = static_cast<int>(fd) + 1;
      if (fd > 2 && fd != keep && fd != directory_fd && fd != trace_fd &&
          !IsLoaderRelroFd(directory_fd, entry->d_name, static_cast<int>(fd)))
        close(static_cast<int>(fd));
    }
  }
  closedir(directory);
  return true;
}

bool ParkReceivedFds(std::vector<int>& fds, int minimum_fd) {
  // Avoid descriptors cached by inherited libraries. On the phone, low-number
  // SCM_RIGHTS channels closed before the child entry; parking fixes that.
  // Stay above both the inherited high-water mark and a conservative floor.
  for (int& fd : fds) {
    int parked = fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
    if (parked < 0)
      return false;
    close(fd);
    fd = parked;
  }
  return true;
}

bool SendRequest(int socket,
                 const Request& request,
                 const std::vector<int>& fds) {
  iovec vector{const_cast<Request*>(&request), kRequestHeader + request.bytes};
  alignas(cmsghdr) char control[CMSG_SPACE(kMaxFds * sizeof(int))] = {};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  if (!fds.empty()) {
    message.msg_control = control;
    message.msg_controllen =
        static_cast<socklen_t>(CMSG_SPACE(fds.size() * sizeof(int)));
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len =
        static_cast<socklen_t>(CMSG_LEN(fds.size() * sizeof(int)));
    memcpy(CMSG_DATA(header), fds.data(), fds.size() * sizeof(int));
  }
  ssize_t sent;
  do {
    sent = sendmsg(socket, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (sent < 0 && errno == EINTR);
  return sent == static_cast<ssize_t>(vector.iov_len);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
bool ReceiveRequest(int socket, Request& request, std::vector<int>& fds) {
  iovec vector{&request, sizeof(request)};
  alignas(cmsghdr) char control[CMSG_SPACE(kMaxFds * sizeof(int))] = {};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  ssize_t received;
  do {
    received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
  } while (received < 0 && errno == EINTR);
  if (received <= 0)
    return false;
  bool valid_control = true;
  for (cmsghdr* header = CMSG_FIRSTHDR(&message); header;
       header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len < CMSG_LEN(0)) {
      valid_control = false;
      continue;
    }
    size_t bytes = header->cmsg_len - CMSG_LEN(0);
    if (bytes % sizeof(int))
      valid_control = false;
    for (size_t offset = 0; offset + sizeof(int) <= bytes;
         offset += sizeof(int)) {
      int fd;
      memcpy(&fd, CMSG_DATA(header) + offset, sizeof(fd));
      fds.push_back(fd);
    }
  }
  return valid_control && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
         received >= static_cast<ssize_t>(kRequestHeader) &&
         request.magic == kMagic && request.bytes < kMaxParams &&
         request.fd_count <= kMaxFds && fds.size() == request.fd_count &&
         received == static_cast<ssize_t>(kRequestHeader + request.bytes);
}

#pragma GCC diagnostic pop

bool SendReply(int socket, const Reply& reply) {
  ssize_t sent;
  do {
    sent = send(socket, &reply, sizeof(reply), MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (sent < 0 && errno == EINTR);
  return sent == sizeof(reply);
}

void ReapServerChildren(std::array<ServerChild, kMaxChildren>& children,
                        Reply& reply) {
  for (auto& child : children) {
    if (child.pid <= 0)
      continue;
    int status = -1;
    pid_t result;
    do {
      result = waitpid(child.pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == child.pid || (result < 0 && errno == ECHILD)) {
      // Keep a record until a reply transfers it to the browser. At most
      // kMaxChildren records, because these slots are reused only after send.
      reply.exited[reply.exited_count++] = {child.pid,
                                            result > 0 ? status : -1,
                                            SnapshotProgress(child.progress)};
      child.pid = 0;
    }
  }
}

bool BindLifetime(pid_t parent) {
  // Check PPID after prctl to close the parent-died-before-registration race.
  return prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() == parent;
}

void ResetSignals() {
  // Reset process lifecycle signals, but leave libc/runtime private signals
  // alone: this is a fork snapshot of loaded system libraries, not an exec.
  struct sigaction action = {};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  for (int signal : {SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGABRT})
    sigaction(signal, &action, nullptr);
  sigset_t empty;
  sigemptyset(&empty);
  sigprocmask(SIG_SETMASK, &empty, nullptr);
}

[[noreturn]] void RunChild(int control,
                           pid_t server,
                           const char* library,
                           Request& request,
                           const std::vector<int>& fds,
                           ChildProgress* progress,
                           int minimum_fd) {
  child_progress = progress;
  child_minimum_fd = minimum_fd;
  ReportOhosForkStage(OhosForkStage::kForked);
  close(control);
  if (!BindLifetime(server))
    _exit(77);
  ResetSignals();
  // Refresh process-local state before the entry can start multiple threads.
  GetClient();
  prctl(PR_SET_NAME, "ohos-fork-child", 0, 0, 0);
  std::array<NativeChildProcess_Fd, kMaxFds> nodes{};
  char names[kMaxFds][16] = {};
  for (size_t i = 0; i < fds.size(); ++i) {
    snprintf(names[i], sizeof(names[i]), "%d", request.destinations[i]);
    nodes[i].fdName = names[i];
    nodes[i].fd = fds[i];
    nodes[i].next = i + 1 < fds.size() ? &nodes[i + 1] : nullptr;
  }
  request.params[request.bytes] = '\0';
  NativeChildProcess_Args args{request.params,
                               {fds.empty() ? nullptr : nodes.data()}};
  prctl(PR_SET_NAME, "ohos-fork-load", 0, 0, 0);
  ReportOhosForkStage(OhosForkStage::kLoading);
  void* handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
  using Entry = void (*)(NativeChildProcess_Args);
  auto entry =
      handle ? reinterpret_cast<Entry>(dlsym(handle, "ChromiumNativeChildMain"))
             : nullptr;
  if (!entry)
    _exit(78);
  prctl(PR_SET_NAME, "ohos-fork-entry", 0, 0, 0);
  ReportOhosForkStage(OhosForkStage::kEntry);
  entry(args);
  ReportOhosForkStage(OhosForkStage::kReturned);
  _exit(0);
}

[[noreturn]] void ServerLoop(int socket, pid_t browser, const char* library) {
  TraceOhosForkDescriptors("server-before-cleanup");
  ResetSignals();
  int minimum_received_fd = 256;
  if (!BindLifetime(browser) || !CloseInheritedFds(socket, minimum_received_fd))
    _exit(79);
  TraceOhosForkDescriptors("server-after-cleanup");
  prctl(PR_SET_NAME, "ohos-fork-server", 0, 0, 0);
  void* shared = mmap(nullptr, sizeof(ChildProgress) * kMaxChildren,
                      PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (shared == MAP_FAILED)
    _exit(79);
  auto* progress = static_cast<ChildProgress*>(shared);
  std::array<ServerChild, kMaxChildren> children{};
  for (size_t i = 0; i < children.size(); ++i) {
    new (&progress[i]) ChildProgress;
    children[i].progress = &progress[i];
  }
  Reply pending;
  if (!SendReply(socket, pending))
    _exit(79);
  for (;;) {
    // Reap even while the browser is idle; preserve exact statuses for its
    // next RPC. No application-global SIGCHLD handler or waitpid(-1).
    ReapServerChildren(children, pending);
    if (!WaitReadable(socket, 100)) {
      if (errno == ETIMEDOUT || errno == EINTR)
        continue;
      break;
    }
    Request request;
    std::vector<int> fds;
    if (!ReceiveRequest(socket, request, fds)) {
      CloseFds(fds);
      break;
    }
    Reply reply = pending;
    bool stop = false;
    if (request.operation == Operation::kLaunch) {
      auto* slot = static_cast<ServerChild*>(nullptr);
      for (auto& child : children)
        if (!child.pid) {
          slot = &child;
          break;
        }
      if (!ParkReceivedFds(fds, minimum_received_fd)) {
        // The normal cleanup below closes both parked and remaining FDs.
        reply.error = errno;
      } else if (!slot || !request.bytes) {
        reply.error = slot ? EINVAL : EAGAIN;
      } else {
        slot->progress->value.store(0, std::memory_order_relaxed);
        pid_t server = getpid();
        TraceOhosForkDescriptors("server-before-child-fork");
        pid_t child = fork();
        if (!child)
          RunChild(socket, server, library, request, fds, slot->progress,
                   minimum_received_fd);
        if (child < 0)
          reply.error = errno;
        else {
          slot->pid = child;
          reply.pid = child;
        }
      }
    } else if (request.operation == Operation::kSignal) {
      bool owned = false;
      for (const auto& child : children)
        owned |= child.pid > 0 && child.pid == request.pid;
      if (!owned)
        reply.error = ESRCH;
      else if (request.signal != SIGKILL && request.signal != SIGTERM)
        reply.error = EINVAL;
      else if (kill(request.pid, request.signal))
        reply.error = errno;
    } else if (request.operation == Operation::kStop) {
      stop = true;
    } else if (request.operation != Operation::kStatus) {
      reply.error = EINVAL;
    }
    if (request.operation == Operation::kStatus) {
      for (const auto& child : children) {
        if (child.pid > 0 && child.pid == request.pid) {
          reply.progress_pid = child.pid;
          reply.progress = SnapshotProgress(child.progress);
          break;
        }
      }
    }
    CloseFds(fds);
    if (!SendReply(socket, reply))
      break;
    pending = Reply{};
    if (stop)
      break;
  }
  // EOF means the browser closed the private channel. No child may survive
  // normal shutdown. Unexpected server death is covered by PDEATHSIG.
  for (const auto& child : children)
    if (child.pid > 0)
      kill(child.pid, SIGKILL);
  for (const auto& child : children)
    if (child.pid > 0) {
      while (waitpid(child.pid, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  close(socket);
  _exit(0);
}

void LoseServer(Client& client) {
  if (client.socket >= 0)
    close(client.socket);
  client.socket = -1;
  if (client.server > 0) {
    kill(client.server, SIGKILL);
    // Only our direct server child. Renderers die through PDEATHSIG.
    while (waitpid(client.server, nullptr, 0) < 0 && errno == EINTR) {
    }
    client.server = -1;
  }
  for (auto& [pid, state] : client.children) {
    if (state.running) {
      state.running = false;
      state.wait_status = -1;
    }
  }
}

bool ReadReply(Client& client, Reply& reply) {
  if (!WaitReadable(client.socket, kRpcTimeoutMs))
    return false;
  ssize_t bytes;
  do {
    bytes = recv(client.socket, &reply, sizeof(reply), MSG_TRUNC);
  } while (bytes < 0 && errno == EINTR);
  if (bytes != sizeof(reply) || reply.magic != kMagic ||
      reply.exited_count > kMaxChildren)
    return false;
  for (size_t i = 0; i < reply.exited_count; ++i) {
    auto found = client.children.find(reply.exited[i].pid);
    if (found != client.children.end()) {
      found->second = {false, reply.exited[i].status};
      SetProgress(found->second, reply.exited[i].progress);
    }
  }
  auto found = client.children.find(reply.progress_pid);
  if (found != client.children.end() && found->second.running)
    SetProgress(found->second, reply.progress);
  return true;
}

bool Rpc(Client& client,
         const Request& request,
         const std::vector<int>& fds,
         Reply& reply) {
  if (client.socket < 0) {
    errno = EPIPE;
    return false;
  }
  if (!SendRequest(client.socket, request, fds) || !ReadReply(client, reply)) {
    int error = errno ? errno : EPROTO;
    LoseServer(client);
    errno = error;
    return false;
  }
  if (reply.error) {
    errno = reply.error;
    return false;
  }
  return true;
}

}  // namespace

bool ConfigureOhosForkFdTrace(const std::string& path) {
  if (GetClient().attempted || trace_fd >= 0 || path.empty())
    return false;
  int fd = open(
      path.c_str(),
      O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return false;
  // Avoid occupying the low cached numbers that this diagnostic investigates.
  trace_fd = fcntl(fd, F_DUPFD_CLOEXEC, 512);
  close(fd);
  if (trace_fd < 0)
    return false;
  if (fstat(trace_fd, &trace_file) || !S_ISREG(trace_file.st_mode)) {
    close(trace_fd);
    trace_fd = -1;
    return false;
  }
  return true;
}

void TraceOhosForkDescriptors(const char* stage) {
  if (trace_fd < 0)
    return;
  const int saved_errno = errno;
  struct stat log = {};
  // Never write through a diagnostic descriptor if it too was reused. Bound
  // disk usage; this is startup diagnostics, not a per-frame trace.
  if (fstat(trace_fd, &log) || log.st_dev != trace_file.st_dev ||
      log.st_ino != trace_file.st_ino || log.st_size >= 8 * 1024 * 1024) {
    errno = saved_errno;
    return;
  }
  for (size_t i = 0; i <= trace_original_count; ++i) {
    char line[768];
    int count;
    if (i == trace_original_count) {
      count = snprintf(
          line, sizeof(line),
          "FDTRACE pid=%d ppid=%d stage=%s end count=%zu truncated=%d\n",
          getpid(), getppid(), stage, trace_original_count, trace_truncated);
    } else {
      int fd = trace_original_fds[i];
      char path[48], target[256] = {};
      snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
      ssize_t length = readlink(path, target, sizeof(target) - 1);
      int link_error = length < 0 ? errno : 0;
      struct stat info = {};
      int stat_error = fstat(fd, &info) ? errno : 0;
      int flags = fcntl(fd, F_GETFD);
      int flags_error = flags < 0 ? errno : 0;
      const bool name_match =
          strcmp(target, "/memfd:relro_cache (deleted)") == 0;
      count = snprintf(
          line, sizeof(line),
          "FDTRACE pid=%d ppid=%d stage=%s fd=%d target=%s link_errno=%d "
          "stat_errno=%d dev=%llu inode=%llu size=%lld nlink=%llu mode=%o "
          "flags=%d flags_errno=%d name_match=%d keep_relro=%d\n",
          getpid(), getppid(), stage, fd, length < 0 ? "<closed>" : target,
          link_error, stat_error, static_cast<unsigned long long>(info.st_dev),
          static_cast<unsigned long long>(info.st_ino),
          static_cast<long long>(info.st_size),
          static_cast<unsigned long long>(info.st_nlink),
          static_cast<unsigned int>(info.st_mode), flags, flags_error,
          name_match, name_match && flags >= 0);
    }
    if (count > 0 && static_cast<size_t>(count) < sizeof(line)) {
      // One append per line: concurrent children cannot share/overwrite a seek
      // offset. Do not retry a partial write by appending half a record later.
      ssize_t written;
      do {
        written = write(trace_fd, line, static_cast<size_t>(count));
      } while (written < 0 && errno == EINTR);
    }
  }
  errno = saved_errno;
}

void ReportOhosForkStage(OhosForkStage stage, int detail) {
  if (child_progress) {
    child_progress->value.store(
        (static_cast<uint64_t>(stage) << 32) | static_cast<uint32_t>(detail),
        std::memory_order_relaxed);
    if (stage == OhosForkStage::kForked || stage == OhosForkStage::kLoading ||
        stage == OhosForkStage::kEntry || stage == OhosForkStage::kNativeMain ||
        stage == OhosForkStage::kDescriptors ||
        stage == OhosForkStage::kChromeMain)
      TraceOhosForkDescriptors(OhosForkStageName(static_cast<uint32_t>(stage)));
  }
}

const char* OhosForkStageName(uint32_t stage) {
  switch (static_cast<OhosForkStage>(stage)) {
    case OhosForkStage::kNotStarted: return "not-started";
    case OhosForkStage::kForked: return "forked";
    case OhosForkStage::kLoading: return "loading";
    case OhosForkStage::kEntry: return "entry";
    case OhosForkStage::kNativeMain: return "native-main";
    case OhosForkStage::kDescriptors: return "descriptors";
    case OhosForkStage::kChromeMain: return "chrome-main";
    case OhosForkStage::kAcceptInvitation: return "accept-invitation";
    case OhosForkStage::kInvitationAccepted: return "invitation-accepted";
    case OhosForkStage::kConnected: return "connected";
    case OhosForkStage::kChannelError: return "channel-error";
    case OhosForkStage::kConnectionTimeout: return "connection-timeout";
    case OhosForkStage::kReturned: return "returned";
  }
  return "unknown";
}

int DuplicateOhosChildDescriptor(int fd) {
  int result;
  do {
    result = fcntl(fd, F_DUPFD_CLOEXEC, child_minimum_fd);
  } while (result < 0 && errno == EINTR);
  return result;
}

bool StartOhosForkServer(const std::string& child_library) {
  Client& client = GetClient();
  std::lock_guard<std::mutex> lock(client.mutex);
  if (client.socket >= 0)
    return true;
  if (client.attempted || child_library.empty() || child_library[0] != '/') {
    errno = EINVAL;
    return false;
  }
  client.attempted = true;
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets))
    return false;
  pid_t browser = getpid();
  CaptureTraceDescriptors();
  TraceOhosForkDescriptors("browser-before-server-fork");
  pid_t server = fork();
  if (server == 0) {
    TraceOhosForkDescriptors("server-after-fork");
    close(sockets[0]);
    ServerLoop(sockets[1], browser, child_library.c_str());
  }
  close(sockets[1]);
  TraceOhosForkDescriptors("browser-after-server-fork");
  if (server < 0) {
    close(sockets[0]);
    return false;
  }
  client.server = server;
  client.socket = sockets[0];
  Reply hello;
  if (!ReadReply(client, hello)) {
    int error = errno ? errno : EPROTO;
    LoseServer(client);
    errno = error;
    return false;
  }
  return true;
}

void StopOhosForkServer() {
  Client& client = GetClient();
  std::lock_guard<std::mutex> lock(client.mutex);
  if (client.socket < 0)
    return;
  Request request;
  request.operation = Operation::kStop;
  Reply reply;
  Rpc(client, request, {}, reply);
  // Give the server a bounded chance to reap its children on a normal stop.
  if (client.server > 0) {
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until) {
      pid_t result = waitpid(client.server, nullptr, WNOHANG);
      if (result == client.server || (result < 0 && errno == ECHILD)) {
        client.server = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  LoseServer(client);
}

bool IsOhosForkServerStarted() {
  Client& client = GetClient();
  std::lock_guard<std::mutex> lock(client.mutex);
  return client.socket >= 0;
}

pid_t LaunchOhosForkChild(const std::string& encoded_params,
                          const std::vector<std::pair<int, int>>& fds) {
  if (encoded_params.empty() || encoded_params.size() >= kMaxParams ||
      encoded_params.find('\0') != std::string::npos || fds.size() > kMaxFds) {
    errno = EINVAL;
    return -1;
  }
  Request request;
  request.operation = Operation::kLaunch;
  request.bytes = static_cast<uint32_t>(encoded_params.size());
  request.fd_count = static_cast<uint32_t>(fds.size());
  memcpy(request.params, encoded_params.data(), encoded_params.size());
  std::vector<int> sources;
  for (size_t i = 0; i < fds.size(); ++i) {
    if (fds[i].first < 0 || fds[i].second < 0) {
      errno = EINVAL;
      return -1;
    }
    for (size_t j = 0; j < i; ++j) {
      if (fds[i].second == fds[j].second) {
        errno = EINVAL;
        return -1;
      }
    }
    sources.push_back(fds[i].first);
    request.destinations[i] = fds[i].second;
  }
  Client& client = GetClient();
  std::lock_guard<std::mutex> lock(client.mutex);
  Reply reply;
  if (!Rpc(client, request, sources, reply) || reply.pid <= 0)
    return -1;
  client.children[reply.pid] = {true, -1};
  return reply.pid;
}

std::optional<OhosForkChildStatus> GetOhosForkChildStatus(pid_t pid) {
  Client& client = GetClient();
  std::lock_guard<std::mutex> lock(client.mutex);
  auto found = client.children.find(pid);
  if (found == client.children.end())
    return std::nullopt;
  if (found->second.running) {
    Request request;
    request.operation = Operation::kStatus;
    request.pid = pid;
    Reply reply;
    Rpc(client, request, {}, reply);
  }
  return found->second;
}

std::optional<bool> SignalOhosForkChild(pid_t pid, int signal) {
  Client& client = GetClient();
  std::lock_guard<std::mutex> lock(client.mutex);
  auto found = client.children.find(pid);
  if (found == client.children.end())
    return std::nullopt;
  if (!found->second.running)
    return found->second.wait_status >= 0;
  Request request;
  request.operation = Operation::kSignal;
  request.pid = pid;
  request.signal = signal;
  Reply reply;
  bool ok = Rpc(client, request, {}, reply);
  return ok || (!found->second.running && found->second.wait_status >= 0);
}

std::optional<bool> WaitForOhosForkChild(pid_t pid,
                                         int64_t milliseconds,
                                         int* exit_code) {
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    auto state = GetOhosForkChildStatus(pid);
    if (!state)
      return std::nullopt;
    if (!state->running) {
      if (state->wait_status < 0)
        return false;
      if (exit_code) {
        *exit_code = WIFEXITED(state->wait_status)
                         ? WEXITSTATUS(state->wait_status)
                         : -1;
      }
      return true;
    }
    if (milliseconds >= 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
                .count() >= milliseconds)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace base::internal

// Dawn is built without Chromium base dependencies. Its optional weak hook
// resolves here when linked into the engine; official NCP has no trace file.
extern "C" __attribute__((visibility("default"))) void
OHOS_TraceForkDescriptors(const char* stage) {
  base::internal::TraceOhosForkDescriptors(stage);
}
