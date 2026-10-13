// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <AbilityKit/native_child_process.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "base/process/fork_server_ohos.h"

static void* Thread(void* value) {
  // Model a late system-library cleanup, after Chromium restores its FDs.
  // Reusing low FD 3 here would close a live Chromium channel.
  close(3);
  *static_cast<int*>(value) = 42;
  return nullptr;
}

// Same symbol/ABI as the production shim, but no Chromium dependency. This
// tests the real server/SCM_RIGHTS/reaper, not Mojo or the renderer itself.
extern "C" __attribute__((visibility("default"))) void ChromiumNativeChildMain(
    NativeChildProcess_Args args) {
  // NAPI loads the phone fixture's host library with local visibility. Look
  // it up explicitly; the production shim likewise resolves its engine DSO.
#if defined(OHOS_FORK_TEST_SHARED_HOST)
  void* host = dlopen("libforkprobe.so", RTLD_NOW | RTLD_LOCAL);
  if (!host)
    _exit(99);
#else
  void* host = RTLD_DEFAULT;
#endif
  auto duplicate = reinterpret_cast<int (*)(int)>(
      dlsym(host, "ForkTestDuplicateFd"));
  auto report = reinterpret_cast<void (*)(uint32_t, int)>(
      dlsym(host, "ForkTestReportStage"));
  if (!duplicate || !report)
    _exit(99);
  report(static_cast<uint32_t>(base::internal::OhosForkStage::kNativeMain), -23);
  if (!strcmp(args.entryParams, "exit23"))
    _exit(23);
  if (!strcmp(args.entryParams, "crash")) {
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _exit(92);
  }
  if (!strcmp(args.entryParams, "hang")) {
    report(static_cast<uint32_t>(base::internal::OhosForkStage::kConnected), 42);
    if (args.fdList.head) {
      char ready = 'R';
      send(args.fdList.head->fd, &ready, 1, MSG_NOSIGNAL);
    }
    for (;;)
      pause();
  }
  int channel = -1, memory = -1;
  for (auto* node = args.fdList.head; node; node = node->next) {
    if (!strcmp(node->fdName, "77"))
      channel = node->fd;
    if (!strcmp(node->fdName, "88"))
      memory = node->fd;
  }
  // Exercise the production helper used by RestoreFileDescriptors, rather
  // than merely using the server's parked inputs throughout the test.
  channel = duplicate(channel);
  memory = duplicate(memory);
  if (channel < 0 || memory < 0)
    _exit(93);
  int progress[] = {0, getppid(), 0, 1};
  if (send(channel, progress, sizeof(progress), MSG_NOSIGNAL) !=
      sizeof(progress))
    _exit(98);
  void* mapping =
      mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
  if (mapping == MAP_FAILED)
    _exit(95);
  auto* numbers = static_cast<int*>(mapping);
  numbers[2] = 1;
  int value = 0;
  pthread_t thread;
  if (pthread_create(&thread, nullptr, Thread, &value))
    _exit(94);
  numbers[2] = 2;
  if (pthread_join(thread, nullptr) || value != 42)
    _exit(94);
  numbers[2] = 3;
  int response[] = {getpid(), getppid(), static_cast<int>(getuid()),
                    numbers[0] + value};
  numbers[1] = numbers[0] + 1000;
  munmap(mapping, 4096);
  if (send(channel, response, sizeof(response), MSG_NOSIGNAL) !=
      sizeof(response))
    _exit(96);
  int ack = 0;
  if (recv(channel, &ack, sizeof(ack), 0) != sizeof(ack) || ack != 17)
    _exit(97);
}
