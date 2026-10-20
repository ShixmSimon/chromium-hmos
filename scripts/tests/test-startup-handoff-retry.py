#!/usr/bin/env python3
"""Exercise actual handoff retry methods with a small task/weak-pointer harness.

This checks scheduling and cancellation, not Chromium's rendering proof. Native
CI and device traces still verify Request/visual-state/presentation integration.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc").read_text()
source = source[source.index("class StartupHandoff {"):]


def method(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


methods = "\n".join(method(signature) for signature in (
    "void Invalidate(", "void SameDocumentNavigationStarted(",
    "void SameDocumentNavigationFinished(", "void RetryAfterSameDocumentNavigation("))

harness = r'''
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#define FROM_HERE 0
class StartupHandoff;
struct Weak {
  StartupHandoff* owner;
  std::weak_ptr<int> generation;
  int expected;
  bool valid() const {
    auto current = generation.lock();
    return current && *current == expected;
  }
};
struct WeakFactory {
  StartupHandoff* owner;
  std::shared_ptr<int> generation = std::make_shared<int>(0);
  void InvalidateWeakPtrs() { ++*generation; }
  Weak GetWeakPtr() { return {owner, generation, *generation}; }
};
namespace base {
struct DictValue {
  std::map<std::string, std::string> values;
  void Set(const char* key, const std::string& value) { values[key] = value; }
  void Set(const char* key, int value) { values[key] = std::to_string(value); }
};
template<class T> auto BindOnce(void (T::*method)(), Weak weak) {
  return [method, weak] { if (weak.valid()) (weak.owner->*method)(); };
}
struct SingleThreadTaskRunner {
  std::vector<std::function<void()>> tasks;
  static SingleThreadTaskRunner* GetCurrentDefault() {
    static SingleThreadTaskRunner runner;
    return &runner;
  }
  void PostTask(int, std::function<void()> task) { tasks.push_back(std::move(task)); }
  void Run() {
    auto pending = std::move(tasks);
    tasks.clear();
    for (auto& task : pending) task();
  }
};
}
class StartupHandoff {
 public:
  std::string token_ = "owned-token";
  std::string phase_ = "frame";
  int request_id_ = 17;
  int widget_ = 3;
  bool pending_ = true;
  bool retry_after_same_document_ = false;
  WeakFactory weak_factory_{this};
  int requests = 0;
  void Trace(const char*, const char*) {}
  // The real Request also invalidates weak pointers before accepting new work.
  void Request(int widget, const base::DictValue& command) {
    assert(widget == widget_);
    assert(command.values.at("startupToken") == token_);
    assert(command.values.at("startupPhase") == "frame");
    assert(command.values.at("requestId") == std::to_string(request_id_));
    Invalidate("request_prepare");
    pending_ = true;
    ++requests;
  }
METHODS
};
int main() {
  auto* runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  {
    StartupHandoff h;
    auto old_proof = h.weak_factory_.GetWeakPtr();
    h.SameDocumentNavigationStarted();
    assert(!old_proof.valid() && !h.pending_);
    h.SameDocumentNavigationFinished();
    assert(h.requests == 0 && runner->tasks.size() == 1);
    runner->Run();
    assert(h.requests == 1 && h.pending_);
  }
  for (auto reason : {"main_document_start", "main_document_committed",
                       "active_contents_changed", "contents_destroyed",
                       "shell_cancel", "token_replaced"}) {
    StartupHandoff h;
    h.SameDocumentNavigationStarted();
    h.SameDocumentNavigationFinished();
    h.Invalidate(reason);
    runner->Run();
    assert(h.requests == 0);
  }
  {
    StartupHandoff h;
    h.SameDocumentNavigationStarted();
    h.SameDocumentNavigationFinished();
    h.SameDocumentNavigationStarted();
    h.SameDocumentNavigationFinished();
    runner->Run();
    assert(h.requests == 1);  // Only the latest same-document finish retries.
  }
  {
    StartupHandoff h;
    h.SameDocumentNavigationStarted();
    h.Invalidate("active_contents_changed");
    h.SameDocumentNavigationFinished();
    runner->Run();
    assert(h.requests == 0);
  }
  for (int mode = 0; mode < 4; ++mode) {
    StartupHandoff h;
    if (mode == 0) h.pending_ = false;
    if (mode == 1) h.phase_ = "ready";
    h.SameDocumentNavigationStarted();
    h.SameDocumentNavigationFinished();
    if (mode == 2) h.token_.clear();
    if (mode == 3) h.request_id_ = 0;
    runner->Run();
    assert(h.requests == 0);
  }
  {
    StartupHandoff h;
    h.SameDocumentNavigationStarted();
    h.SameDocumentNavigationFinished();
    base::DictValue command;
    command.Set("startupToken", h.token_);
    command.Set("startupPhase", h.phase_);
    command.Set("requestId", ++h.request_id_);
    h.Request(h.widget_, command);
    runner->Run();
    assert(h.requests == 1);  // A newer shell request cancels the posted retry.
  }
  {
    auto h = std::make_unique<StartupHandoff>();
    h->SameDocumentNavigationStarted();
    h->SameDocumentNavigationFinished();
    h.reset();
    runner->Run();  // The posted task cannot dereference a destroyed owner.
  }
}
'''.replace("METHODS", methods)

with tempfile.TemporaryDirectory(prefix="startup-handoff-retry-") as directory:
    unit = Path(directory) / "retry_test.cc"
    binary = Path(directory) / "retry_test"
    unit.write_text(harness)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra",
                    "-Werror", str(unit), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Startup handoff retry scheduling/cancellation tests passed.")
