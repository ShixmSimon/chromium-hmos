#!/usr/bin/env python3
"""Exercise real FD cleanup and shared-state validation without a device."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def compile_run(source, files=None):
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        for name, content in (files or {}).items():
            target = folder / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        (folder / 'test.cc').write_text(source)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall',
                        '-Wextra', '-Werror', '-I', str(folder), '-I', str(ROOT / 'overlay'),
                        str(folder / 'test.cc'), '-o', str(folder / 'test')], check=True)
        subprocess.run([str(folder / 'test')], check=True, timeout=30)


class RecoveryTest(unittest.TestCase):
    def test_fatal_shared_context_restarts_only_the_isolated_gpu(self):
        # Execute the production recovery helper; only process transport is stubbed.
        compile_run(r'''
#include <cassert>
#include "gpu/ipc/service/ohos_gpu_context_recovery.h"
int main() {
  using namespace gpu;
  for (bool child : {false, true}) {
    for (ContextResult result : {ContextResult::kSuccess,
                                ContextResult::kTransientFailure,
                                ContextResult::kFatalFailure}) {
      GpuChannelManagerDelegate delegate;
      for (int client = 0; client < 1000; ++client)
        RestartOhosGpuAfterSharedContextFailure(child, result, &delegate);
      assert(delegate.exits == (child && result == ContextResult::kFatalFailure ? 1 : 0));
    }
  }
}
''', {
            'gpu/command_buffer/common/context_result.h':
                '#pragma once\nnamespace gpu { enum class ContextResult { kSuccess, kTransientFailure, kFatalFailure }; }',
            'gpu/ipc/service/gpu_channel_manager_delegate.h': r'''
#pragma once
#include <initializer_list>
namespace gpu {
namespace error { enum ContextLostReason { kMakeCurrentFailed }; }
struct GpuChannelManagerDelegate {
  int exits = 0;
  bool IsExiting() const { return exits != 0; }
  void MaybeExitOnContextLost(error::ContextLostReason reason) {
    assert(reason == error::kMakeCurrentFailed);
    ++exits;
  }
};
}
''',
        })

    def test_fork_cleanup_keeps_trace_numbers_without_leaking_app_channels(self):
        source = (ROOT / 'overlay/base/process/fork_server_ohos.cc').read_text()
        functions = '\n'.join(function(source, name) for name in (
            'bool IsLoaderRelroFd(', 'bool IsSystemTraceMarkerFd(', 'bool CloseInheritedFds('))
        # macOS has no /proc. Redirect only the directory lookup to a fixture
        # containing real symlinks; all close/readlink/fcntl/fork/mmap are real.
        functions = functions.replace('opendir("/proc/self/fd")', 'opendir(fd_directory.c_str())')
        compile_run(r'''
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
std::string fd_directory;
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wsign-conversion"
FUNCTIONS
#pragma GCC diagnostic pop
int memory_file() {
  char path[] = "/tmp/lumie-fd-test.XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0 && unlink(path) == 0 && ftruncate(fd, 4096) == 0);
  return fd;
}
void entry(int fd, const std::string& target) {
  assert(symlink(target.c_str(), (fd_directory + "/" + std::to_string(fd)).c_str()) == 0);
}
int main() {
  char path[] = "/tmp/lumie-fd-directory.XXXXXX";
  assert(mkdtemp(path));
  fd_directory = path;
  std::vector<int> preserved;
  for (const char* target : {"/sys/kernel/debug/tracing/trace_marker",
                            "/sys/kernel/tracing/trace_marker",
                            "/memfd:relro_cache (deleted)"}) {
    int fd = memory_file();
    entry(fd, target);
    preserved.push_back(fd);
  }
  std::vector<int> closed;
  for (const char* target : {"/data/app/trace_marker",
                            "/sys/kernel/tracing/trace_marker.extra",
                            "socket:[123]", "/memfd:shared-memory (deleted)"}) {
    int fd = memory_file();
    entry(fd, target);
    closed.push_back(fd);
  }
  // Model an inherited cached trace FD writing after arbitrary allocations.
  void* marker = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, preserved[0], 0);
  assert(marker != MAP_FAILED);
  for (int attempt = 0; attempt < 4; ++attempt) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      int minimum = 256;
      assert(CloseInheritedFds(-1, minimum));
      for (int fd : closed)
        assert(fcntl(fd, F_GETFD) == -1);
      std::vector<int> replacements;
      for (int i = 0; i < 32; ++i) replacements.push_back(memory_file());
      const char trace[] = "E|7391|I38";
      for (int fd : preserved) {
        assert(fcntl(fd, F_GETFD) >= 0);
        assert(pwrite(fd, trace, sizeof(trace), 0) == sizeof(trace));
      }
      char state[sizeof(trace)];
      for (int fd : replacements) {
        assert(pread(fd, state, sizeof(state), 0) == sizeof(state));
        for (char value : state) assert(value == 0);
      }
      _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(strcmp(static_cast<char*>(marker), "E|7391|I38") == 0);
  }
  munmap(marker, 4096);
  for (auto* descriptors : {&preserved, &closed})
    for (int fd : *descriptors) {
      assert(fcntl(fd, F_GETFD) >= 0);  // Child cleanup cannot affect parent.
      unlink((fd_directory + "/" + std::to_string(fd)).c_str());
      close(fd);
    }
  assert(rmdir(fd_directory.c_str()) == 0);
}
'''.replace('FUNCTIONS', functions))

    def test_corrupted_state_loses_channel_without_advancing_fences(self):
        # Stub transport only. The validator is the production overlay header.
        shared = r'''
#pragma once
#include <cstdint>
namespace gpu {
namespace error {
enum Error { kNoError, kInvalidSize, kOutOfBounds, kUnknownCommand,
             kInvalidArguments, kLostContext, kGenericError,
             kDeferCommandUntilLater, kDeferLaterCommands, kErrorLast = 8 };
enum ContextLostReason { kGuilty, kInnocent, kUnknown, kOutOfMemory,
                        kMakeCurrentFailed, kGpuChannelLost,
                        kInvalidGpuMessage, kContextLostReasonLast = 6 };
}
struct CommandBuffer {
  struct State {
    int get_offset = 7, token = 8;
    uint64_t release_count = 9;
    error::Error error = error::kNoError;
    error::ContextLostReason context_lost_reason = error::kUnknown;
    uint32_t generation = 10, set_get_buffer_count = 11;
  };
};
struct CommandBufferSharedState {
  CommandBuffer::State candidate;
  void Read(CommandBuffer::State* state) { *state = candidate; }
};
}
'''
        compile_run(r'''
#include <cassert>
#include "gpu/ipc/client/ohos_command_buffer_state.h"
int main() {
  using namespace gpu;
  CommandBufferSharedState shared;
  for (int error = 0; error <= error::kErrorLast; ++error)
    for (int reason = 0; reason <= error::kContextLostReasonLast; ++reason) {
      shared.candidate.error = static_cast<error::Error>(error);
      shared.candidate.context_lost_reason = static_cast<error::ContextLostReason>(reason);
      shared.candidate.token = 100;
      CommandBuffer::State state;
      ReadOhosCommandBufferSharedState(&shared, &state);
      assert(state.error == error && state.context_lost_reason == reason && state.token == 100);
    }
  for (auto pair : {std::pair<int, int>{0x7c453833, 0x31393337},
                    {-1, 2}, {9, 2}, {5, -1}, {5, 7}, {0, 0x31393337}}) {
    shared.candidate.error = static_cast<error::Error>(pair.first);
    shared.candidate.context_lost_reason = static_cast<error::ContextLostReason>(pair.second);
    shared.candidate.get_offset = shared.candidate.token = 123456;
    shared.candidate.release_count = 999999;
    shared.candidate.generation = shared.candidate.set_get_buffer_count = 123456;
    CommandBuffer::State state;
    ReadOhosCommandBufferSharedState(&shared, &state);
    assert(state.error == error::kLostContext);
    assert(state.context_lost_reason == error::kInvalidGpuMessage);
    assert(state.get_offset == 7 && state.token == 8 && state.release_count == 9);
    assert(state.generation == 10 && state.set_get_buffer_count == 11);
  }
}
'''.replace('#include <cassert>', '#include <cassert>\n#include <utility>\n#include <initializer_list>'), {
            'gpu/command_buffer/common/command_buffer_shared.h': shared,
            'gpu/command_buffer/common/constants.h': '#pragma once\n',
        })


if __name__ == '__main__':
    unittest.main()
