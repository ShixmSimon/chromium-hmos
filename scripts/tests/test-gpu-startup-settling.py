#!/usr/bin/env python3
"""Run the production GPU failure-marker settling code with a manual task queue.

This is a Linux host regression test, not a Chromium/OHOS kernel build. Compile
only the unchanged production marker/history/settling block with small base
stubs, so delayed presentation and stale callbacks exercise the real retry code.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'overlay/ui/ozone/platform/ohos/ohos_gpu_child_channel.cc').read_text()
start = source.index('base::FilePath GpuChildFailedMarker()')
end = source.index('base::ProcessId StartGpuChild(', start)
production = source[start:end]
prefix = r'''
#include <atomic>
#include <cassert>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#define FROM_HERE 0
#define LOG(level) std::cout
bool marker = false, phone = true, presented = false;
int deletes = 0, writes = 0;
namespace base {
using Lock = std::mutex;
using AutoLock = std::lock_guard<Lock>;
struct FilePath {
  std::string value;
  FilePath() = default;
  explicit FilePath(const char* text) : value(text) {}
  bool empty() const { return value.empty(); }
  FilePath Append(const char* name) const {
    FilePath path; path.value = value + "/" + name; return path;
  }
};
namespace internal { constexpr char kOhosPhoneFork[] = "phone-fork"; }
struct CommandLine {
  static CommandLine* ForCurrentProcess() { static CommandLine line; return &line; }
  FilePath GetSwitchValuePath(const char*) { return FilePath("profile"); }
  bool HasSwitch(const char*) { return phone; }
};
bool WriteFile(const FilePath&, const char*) { marker = true; ++writes; return true; }
bool DeleteFile(const FilePath&) { marker = false; ++deletes; return true; }
int MayBlock() { return 0; }
int Seconds(int value) { return value; }
template<class F, class A> auto BindOnce(F fn, A arg) { return [=] { fn(arg); }; }
struct ThreadPool {
  static inline std::vector<std::function<void()>> pending;
  static void PostDelayedTask(int, std::initializer_list<int>, std::function<void()> fn, int delay) {
    assert(delay == 1); pending.push_back(fn);
  }
};
}
constexpr char kOhosGpuChildFailedMarker[] = "GpuChildFailed";
'''
prefix += 'bool OhosPhoneGpuHasPresented() { return presented; }\n'
suffix = r'''
void tick() {
  assert(base::ThreadPool::pending.size() == 1);
  auto fn = base::ThreadPool::pending.front(); base::ThreadPool::pending.clear(); fn();
}
void begin() {
  auto& h = History(); base::AutoLock lock(h.marker_lock);
  ++h.generation; h.fell_back = false; h.start_failed = false;
  base::ThreadPool::pending.clear(); marker = false; deletes = writes = 0;
  phone = true; presented = false; NoteGpuChildInDoubt();
}
int main() {
  begin(); ClearGpuChildDoubtIfSettled(History().generation.load());
  for (int i = 0; i < 40; ++i) tick();
  assert(marker && !deletes && writes == 1);
  presented = true; tick();
  assert(!marker && deletes == 1 && base::ThreadPool::pending.empty());
  std::cout << "PASS background-first startup clears marker after delayed first frame\n";
  begin(); ClearGpuChildDoubtIfSettled(History().generation.load());
  ++History().generation; NoteGpuChildInDoubt(); presented = true; tick();
  assert(marker && !deletes && base::ThreadPool::pending.empty());
  std::cout << "PASS stale callback cannot clear a newer GPU attempt\n";
  begin(); ClearGpuChildDoubtIfSettled(History().generation.load());
  History().fell_back = true; presented = true; tick();
  assert(marker && !deletes && base::ThreadPool::pending.empty());
  std::cout << "PASS disabled-GL fallback retains failure guard\n";
  begin(); ClearGpuChildDoubtIfSettled(History().generation.load());
  History().start_failed = true; presented = true; tick();
  assert(marker && !deletes && base::ThreadPool::pending.empty());
  std::cout << "PASS failed launch retains failure guard and stops retries\n";
  begin(); phone = false; ClearGpuChildDoubtIfSettled(History().generation.load());
  assert(!marker && deletes == 1 && base::ThreadPool::pending.empty());
  std::cout << "PASS standard Pad GPU settling does not wait for phone broker\n";
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(prefix + production + suffix)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread',
                    '-Wall', '-Wextra', '-Werror', str(path / 'test.cc'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=10)
