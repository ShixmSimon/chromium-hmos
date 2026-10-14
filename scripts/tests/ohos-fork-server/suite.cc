// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "suite.h"
#include "base/process/fork_server_ohos.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The test DSO resolves these wrappers so the same fixture can run from a
// Linux executable or the phone probe's native shared library.
extern "C" __attribute__((visibility("default"))) int ForkTestDuplicateFd(int fd) {
  return base::internal::DuplicateOhosChildDescriptor(fd);
}
extern "C" __attribute__((visibility("default"))) void ForkTestReportStage(
    uint32_t stage, int detail) {
  base::internal::ReportOhosForkStage(
      static_cast<base::internal::OhosForkStage>(stage), detail);
}

namespace {
using namespace base::internal;
int cached_runtime_fd = -1;
void CloseCachedRuntimeFd() {
  if (cached_runtime_fd >= 0)
    close(cached_runtime_fd);
}
int FdCount() {
  DIR* directory = opendir("/proc/self/fd");
  if (!directory)
    return -1;
  int count = 0;
  while (dirent* entry = readdir(directory))
    if (entry->d_name[0] != '.')
      ++count;
  closedir(directory);
  return count - 1;
}
bool Normal(int iteration, pid_t& server, std::string* detail = nullptr) {
  int channel[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel))
    return false;
  int fd = syscall(SYS_memfd_create, "fork-server-test", 1);
  if (fd < 0 || ftruncate(fd, 4096)) {
    if (detail)
      *detail += "memfd/ftruncate errno=" + std::to_string(errno) + "\n";
    if (fd >= 0)
      close(fd);
    close(channel[0]);
    close(channel[1]);
    return false;
  }
  void* mapping =
      mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    if (detail)
      *detail += "mmap errno=" + std::to_string(errno) + "\n";
    close(fd);
    close(channel[0]);
    close(channel[1]);
    return false;
  }
  auto* values = static_cast<int*>(mapping);
  values[0] = iteration;
  values[1] = 0;
  values[2] = 0;
  pid_t child = LaunchOhosForkChild("normal", {{channel[1], 77}, {fd, 88}});
  bool caller_keeps_fds =
      fcntl(channel[1], F_GETFD) >= 0 && fcntl(fd, F_GETFD) >= 0;
  close(channel[1]);
  close(fd);
  pollfd ready{channel[0], POLLIN, 0};
  int response[4] = {};
  int entry_stage = 0;
  int poll_result = -1;
  ssize_t received = -1;
  int ipc_errno = 0;
  bool ok = child > 0 && caller_keeps_fds;
  do {
    if (ok) {
      do {
        poll_result = poll(&ready, 1, 5000);
      } while (poll_result < 0 && errno == EINTR);
      if (poll_result > 0) {
        do {
          received = recv(channel[0], response, sizeof(response), 0);
        } while (received < 0 && errno == EINTR);
      }
      ipc_errno = errno;
      ok = poll_result > 0 && received == sizeof(response);
    }
    if (ok && response[0] == 0) {
      entry_stage = response[3];
      server = response[1];
    }
  } while (ok && response[0] == 0);
  if (ok) {
    if (server <= 0)
      server = response[1];
    ok = response[0] == child && response[1] == server && server != getpid() &&
         response[2] == static_cast<int>(getuid()) &&
         response[3] == iteration + 42 && values[1] == iteration + 1000;
  }
  int ack = 17;
  if (ok)
    ok = send(channel[0], &ack, sizeof(ack), MSG_NOSIGNAL) == sizeof(ack);
  close(channel[0]);
  int child_stage = values[2];
  munmap(mapping, 4096);
  if (!ok && child > 0)
    SignalOhosForkChild(child, SIGKILL);
  int code = -1;
  auto waited = child > 0 ? WaitForOhosForkChild(child, 5000, &code)
                          : std::optional<bool>();
  auto state = child > 0 ? GetOhosForkChildStatus(child)
                         : std::optional<OhosForkChildStatus>();
  if (detail && (!ok || !waited.value_or(false) || code != 0)) {
    *detail +=
        "normal stage=" + std::to_string(child_stage) +
        " entry=" + std::to_string(entry_stage) +
        " poll=" + std::to_string(poll_result) +
        " revents=" + std::to_string(ready.revents) +
        " recv=" + std::to_string(received) +
        " ipc_errno=" + std::to_string(ipc_errno) +
        " child=" + std::to_string(child) + " ipc_ok=" + std::to_string(ok) +
        " exit=" + std::to_string(code) +
        " wait_status=" + std::to_string(state ? state->wait_status : -1) +
        " errno=" + std::to_string(errno) + "\n";
  }
  return ok && waited.value_or(false) && code == 0 && state &&
         !state->running && WIFEXITED(state->wait_status) &&
         WEXITSTATUS(state->wait_status) == 0 &&
         state->stage == static_cast<uint32_t>(OhosForkStage::kReturned);
}
}  // namespace

std::string RunOhosForkServerTests(const std::string& child_library) {
  using namespace base::internal;
  int passed = 0, failed = 0;
  std::string report;
  auto check = [&](bool ok, const std::string& name) {
    report += std::string(ok ? "PASS " : "FAIL ") + name + "\n";
    ok ? ++passed : ++failed;
    return ok;
  };
  check(!IsOhosForkServerStarted(), "default creates no server");
  check(!GetOhosForkChildStatus(getpid()) &&
            !SignalOhosForkChild(getpid(), SIGKILL),
        "unowned PID stays on platform path");
  check(!StartOhosForkServer("relative.so"), "reject relative library path");
  // Model an inherited library's fork hook closing its cached descriptor.
  // The first fork closes it in the server; later SCM_RIGHTS must not reuse
  // that number in the child. This reproduces the phone's low-FD failure.
  cached_runtime_fd = 3;
  check(pthread_atfork(nullptr, nullptr, CloseCachedRuntimeFd) == 0,
        "install inherited runtime FD cleanup regression");
  // The server must not inherit the host's blocked/ignored termination policy.
  struct sigaction ignored = {}, previous = {};
  ignored.sa_handler = SIG_IGN;
  sigemptyset(&ignored.sa_mask);
  sigaction(SIGTERM, &ignored, &previous);
  sigset_t blocked, old_mask;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &blocked, &old_mask);
  const bool started = StartOhosForkServer(child_library);
  cached_runtime_fd = -1;
  pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
  sigaction(SIGTERM, &previous, nullptr);
  if (!check(started, "start early server"))
    return report + "RESULT startup failed errno=" + std::to_string(errno) +
           "\n";
  check(IsOhosForkServerStarted() && StartOhosForkServer(child_library),
        "start is idempotent while alive");
  check(LaunchOhosForkChild("", {}) < 0, "reject empty parameters");
  check(LaunchOhosForkChild(std::string(65536, 'x'), {}) < 0,
        "reject oversized parameters");
  check(LaunchOhosForkChild("normal", {{0, 77}, {1, 77}}) < 0,
        "reject duplicate destination FDs");
  check(LaunchOhosForkChild("normal", {{-1, 77}}) < 0,
        "reject invalid source FD");
  std::vector<std::pair<int, int>> too_many;
  for (int i = 0; i < 17; ++i)
    too_many.emplace_back(0, 77 + i);
  check(LaunchOhosForkChild("normal", too_many) < 0, "reject more than 16 FDs");
  int before = FdCount();
  pid_t server = -1;
  for (int i = 1; i <= 100; ++i) {
    if (!check(Normal(i, server, &report),
               "normal IPC/shared-memory/thread #" + std::to_string(i)))
      break;
  }
  int after = FdCount();
  report += "browser=" + std::to_string(getpid()) +
            " server=" + std::to_string(server) +
            " FD before=" + std::to_string(before) +
            " after=" + std::to_string(after) + "\n";
  check(before == after, "completed launches retain no extra FDs");
  pid_t abnormal = LaunchOhosForkChild("exit23", {});
  int code = -1;
  check(abnormal > 0 &&
            WaitForOhosForkChild(abnormal, 5000, &code).value_or(false) &&
            code == 23,
        "preserve nonzero exit code 23");
  auto progress = GetOhosForkChildStatus(abnormal);
  check(progress &&
            progress->stage == static_cast<uint32_t>(OhosForkStage::kNativeMain) &&
            progress->detail == -23,
        "child progress and signed detail survive reaping");
  pid_t crash = LaunchOhosForkChild("crash", {});
  bool waited =
      crash > 0 && WaitForOhosForkChild(crash, 5000, &code).value_or(false);
  auto state = GetOhosForkChildStatus(crash);
  check(waited && state && WIFSIGNALED(state->wait_status) &&
            WTERMSIG(state->wait_status) == SIGABRT,
        "preserve SIGABRT crash status");
  pid_t hung = LaunchOhosForkChild("hang", {});
  check(hung > 0 && !WaitForOhosForkChild(hung, 0, nullptr).value_or(true),
        "zero-timeout live child");
  check(SignalOhosForkChild(hung, SIGKILL).value_or(false) &&
            WaitForOhosForkChild(hung, 5000, nullptr).value_or(false),
        "kill through server and reap");
  state = GetOhosForkChildStatus(hung);
  check(state && WIFSIGNALED(state->wait_status) &&
            WTERMSIG(state->wait_status) == SIGKILL,
        "preserve SIGKILL status");
  check(Normal(101, server, &report), "launch again after crash and kill");
  pid_t terminated = LaunchOhosForkChild("hang", {});
  bool term_ok =
      terminated > 0 &&
      SignalOhosForkChild(terminated, SIGTERM).value_or(false) &&
      WaitForOhosForkChild(terminated, 5000, nullptr).value_or(false);
  state = GetOhosForkChildStatus(terminated);
  check(term_ok && state && WIFSIGNALED(state->wait_status) &&
            WTERMSIG(state->wait_status) == SIGTERM,
        "reset inherited ignored and blocked SIGTERM");
  std::vector<pid_t> concurrent;
  for (int i = 0; i < 8; ++i)
    concurrent.push_back(LaunchOhosForkChild("hang", {}));
  bool all_started = true;
  for (pid_t pid : concurrent)
    all_started &= pid > 0;
  check(all_started, "eight concurrent children");
  StopOhosForkServer();
  check(!IsOhosForkServerStarted(), "stop server");
  // A stopped server is our own direct child and must already be reaped.
  int status = 0;
  check(
      server > 0 && waitpid(server, &status, WNOHANG) == -1 && errno == ECHILD,
      "server reaped by browser");
  bool all_gone = true;
  for (pid_t pid : concurrent)
    all_gone &= pid > 0 && kill(pid, 0) == -1 && errno == ESRCH;
  check(all_gone, "shutdown killed and reaped all live children");
  check(!StartOhosForkServer(child_library),
        "no late server restart after shutdown");
  check(LaunchOhosForkChild("normal", {}) < 0, "no late-fork fallback");
  report += "RESULT passed=" + std::to_string(passed) +
            " failed=" + std::to_string(failed) + "\n";
  return report;
}

#ifndef OHOS_FORK_TEST_EMBEDDED
namespace {
bool check_loader_fds = false;
bool simulate_loader_fd_loss = false;
bool deny_loader_fstat = false;
std::array<int, 2> loader_fds{-1, -1};
std::array<struct stat, 2> loader_files{};
std::array<int, 4> application_fds{-1, -1, -1, -1};
constexpr off_t kRelroOffset = 0x3e000;
constexpr int kRelroMarker = 0x52454c52;

bool HasTrace(const std::string& contents,
              const std::string& stage,
              int fd,
              const std::string& expected) {
  std::istringstream lines(contents);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find("stage=" + stage + " fd=" + std::to_string(fd) + " ") !=
            std::string::npos &&
        line.find(expected) != std::string::npos)
      return true;
  }
  return false;
}
}  // namespace

// Model the phone's getattr denial without changing readlink/fcntl/mmap.
// Only the host test executable links --wrap=fstat, never the phone fixture.
extern "C" int __real_fstat(int fd, struct stat* info);
extern "C" int __wrap_fstat(int fd, struct stat* info) {
  if (deny_loader_fstat && (fd == loader_fds[0] || fd == loader_fds[1])) {
    errno = EACCES;
    return -1;
  }
  return __real_fstat(fd, info);
}

// Called by the test DSO's constructor, during the real child's dlopen.
extern "C" __attribute__((visibility("default"))) bool
ForkTestCheckLoaderFds() {
  if (simulate_loader_fd_loss) {
    int replacement = syscall(SYS_memfd_create, "shared-memory-region", 1);
    if (replacement < 0 || ftruncate(replacement, 4096))
      return false;
    if (replacement != loader_fds[0]) {
      if (dup2(replacement, loader_fds[0]) < 0)
        return false;
      close(replacement);
    }
    TraceOhosForkDescriptors("test-reused-before-vulkan");
    return true;
  }
  if (!check_loader_fds)
    return true;
  for (int fd : application_fds) {
    if (fcntl(fd, F_GETFD) != -1 || errno != EBADF)
      return false;
  }
  // Occupy the freed low numbers with short Chromium-like memory objects.
  // A stale loader FD must neither disappear nor silently alias these files.
  std::vector<int> replacements;
  bool ok = true;
  for (int i = 0; i < 32; ++i) {
    int fd = syscall(SYS_memfd_create, "shared-memory-region", 1);
    if (fd < 0)
      return false;
    replacements.push_back(fd);
    ok &= ftruncate(fd, 4096) == 0;
  }
  for (size_t i = 0; i < loader_fds.size(); ++i) {
    int fd = loader_fds[i];
    struct stat info = {};
    if (__real_fstat(fd, &info) != 0 || info.st_dev != loader_files[i].st_dev ||
        info.st_ino != loader_files[i].st_ino ||
        fcntl(fd, F_GETFD) != FD_CLOEXEC) {
      ok = false;
      continue;
    }
    void* mapping = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd,
                         kRelroOffset);
    if (mapping == MAP_FAILED) {
      ok = false;
      continue;
    }
    auto* marker = static_cast<volatile int*>(mapping);
    ok &= *marker == kRelroMarker;
    *marker = kRelroMarker + 1;
    munmap(mapping, 4096);
  }
  for (int fd : replacements)
    close(fd);
  TraceOhosForkDescriptors("test-loader-constructor");
  return ok;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[2]) == "--trace-fd-loss") {
    loader_fds[0] = syscall(SYS_memfd_create, "relro_cache", 1);
    if (loader_fds[0] < 0)
      return 1;
    char path[] = "/tmp/ohos-fd-loss-XXXXXX";
    int file = mkstemp(path);
    if (file < 0)
      return 1;
    close(file);
    if (!ConfigureOhosForkFdTrace(path))
      return 1;
    simulate_loader_fd_loss = true;
    cached_runtime_fd = loader_fds[0];
    if (pthread_atfork(nullptr, nullptr, CloseCachedRuntimeFd) ||
        !StartOhosForkServer(argv[1]))
      return 1;
    pid_t child = LaunchOhosForkChild("exit23", {});
    int code = -1;
    bool ok = child > 0 &&
              WaitForOhosForkChild(child, 5000, &code).value_or(false) &&
              code == 23;
    StopOhosForkServer();
    std::ifstream input(path);
    std::string trace((std::istreambuf_iterator<char>(input)), {});
    ok &= HasTrace(trace, "browser-before-server-fork", loader_fds[0],
                   "keep_relro=1");
    ok &= HasTrace(trace, "server-after-fork", loader_fds[0], "stat_errno=9");
    ok &=
        HasTrace(trace, "server-after-cleanup", loader_fds[0], "stat_errno=9");
    ok &= HasTrace(trace, "test-reused-before-vulkan", loader_fds[0],
                   "target=/memfd:shared-memory-region (deleted)");
    errno = EDOM;
    TraceOhosForkDescriptors("test-errno-preserved");
    ok &= errno == EDOM;
    unlink(path);
    close(loader_fds[0]);
    puts(ok ? "PASS trace distinguishes atfork FD loss from cleanup and later "
              "reuse"
            : "FAIL trace FD lifecycle");
    return ok ? 0 : 1;
  }
  if (argc == 3 && (std::string(argv[2]) == "--loader-fds" ||
                    std::string(argv[2]) == "--loader-fstat-denied")) {
    for (size_t i = 0; i < loader_fds.size(); ++i) {
      int fd = syscall(SYS_memfd_create, "relro_cache", 1);
      loader_fds[i] = fd;
      if (fd < 0 || ftruncate(fd, kRelroOffset + 4096) ||
          pwrite(fd, &kRelroMarker, sizeof(kRelroMarker), kRelroOffset) !=
              sizeof(kRelroMarker) ||
          fstat(fd, &loader_files[i]))
        return 1;
    }
    application_fds[0] = syscall(SYS_memfd_create, "shared-memory-region", 1);
    application_fds[1] = syscall(SYS_memfd_create, "relro_cache-extra", 1);
    if (application_fds[0] < 0 || application_fds[1] < 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                   &application_fds[2]))
      return 1;
    check_loader_fds = true;
    deny_loader_fstat = std::string(argv[2]) == "--loader-fstat-denied";
    char trace_path[] = "/tmp/ohos-fd-preserved-XXXXXX";
    int trace_file_fd = mkstemp(trace_path);
    if (trace_file_fd < 0)
      return 1;
    close(trace_file_fd);
    if (!ConfigureOhosForkFdTrace(trace_path))
      return 1;
    if (!StartOhosForkServer(argv[1]))
      return 1;
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
      pid_t child = LaunchOhosForkChild("exit23", {});
      int code = -1;
      ok &= child > 0 &&
            WaitForOhosForkChild(child, 5000, &code).value_or(false) &&
            code == 23;
    }
    StopOhosForkServer();
    std::ifstream trace_file_input(trace_path);
    std::string trace((std::istreambuf_iterator<char>(trace_file_input)), {});
    for (int fd : loader_fds) {
      ok &= HasTrace(trace, "browser-before-server-fork", fd, "keep_relro=1");
      ok &= HasTrace(trace, "server-after-cleanup", fd, "keep_relro=1");
      ok &= HasTrace(trace, "test-loader-constructor", fd, "keep_relro=1");
      if (deny_loader_fstat) {
        for (const char* stage :
             {"browser-before-server-fork", "server-after-cleanup",
              "test-loader-constructor"})
          ok &= HasTrace(trace, stage, fd, "stat_errno=13");
      }
    }
    unlink(trace_path);
    for (int fd : application_fds) {
      ok &= fcntl(fd, F_GETFD) >= 0;
      close(fd);
    }
    for (int fd : loader_fds)
      close(fd);
    if (deny_loader_fstat)
      puts("INJECT fstat EACCES on loader caches in parent/server/child");
    puts(ok ? "PASS loader RELRO original FDs survive dlopen and FD reuse; "
              "application FDs close"
            : "FAIL loader RELRO FD preservation");
    return ok ? 0 : 1;
  }
  if (argc == 3 && std::string(argv[2]) == "--server-loss") {
    // Reap PDEATHSIG orphans ourselves, including in containers without init.
    if (prctl(PR_SET_CHILD_SUBREAPER, 1) || !StartOhosForkServer(argv[1]))
      return 1;
    pid_t server = -1;
    if (!Normal(1, server))
      return 1;
    int channel[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel))
      return 1;
    pid_t child = LaunchOhosForkChild("hang", {{channel[1], 77}});
    close(channel[1]);
    char ready = 0;
    if (child <= 0 || recv(channel[0], &ready, 1, 0) != 1 || ready != 'R')
      return 1;
    close(channel[0]);
    auto live = GetOhosForkChildStatus(child);
    if (!live || !live->running ||
        live->stage != static_cast<uint32_t>(OhosForkStage::kConnected) ||
        live->detail != 42)
      return 1;
    if (kill(server, SIGKILL))
      return 1;
    siginfo_t info = {};
    if (waitid(P_PID, server, &info, WEXITED | WNOWAIT))
      return 1;
    auto state = GetOhosForkChildStatus(child);
    bool ok = state && !state->running && state->wait_status == -1 &&
              !WaitForOhosForkChild(child, 1000, nullptr).value_or(true) &&
              !SignalOhosForkChild(child, SIGKILL).value_or(true) &&
              !StartOhosForkServer(argv[1]) &&
              LaunchOhosForkChild("hang", {}) < 0;
    int status = 0;
    pid_t reaped;
    do {
      reaped = waitpid(child, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    ok &= reaped == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    puts(ok ? "PASS server loss: unknown exit, children die, no fallback"
            : "FAIL server loss");
    return ok ? 0 : 1;
  }
  if (argc == 3 && std::string(argv[2]) == "--missing-library") {
    if (!StartOhosForkServer("/nonexistent/ohos-test-child.so"))
      return 1;
    pid_t child = LaunchOhosForkChild("normal", {});
    int code = 0;
    bool ok = child > 0 &&
              WaitForOhosForkChild(child, 5000, &code).value_or(false) &&
              code == 78;
    auto progress = GetOhosForkChildStatus(child);
    ok &= progress &&
          progress->stage == static_cast<uint32_t>(OhosForkStage::kLoading);
    StopOhosForkServer();
    puts(ok ? "PASS missing library: nonzero bootstrap exit"
            : "FAIL missing library");
    return ok ? 0 : 1;
  }
  if (argc != 2)
    return 2;
  const std::string report = RunOhosForkServerTests(argv[1]);
  fputs(report.c_str(), stdout);
  return report.find("FAIL ") == std::string::npos &&
                 report.find("failed=0") != std::string::npos
             ? 0
             : 1;
}
#endif
