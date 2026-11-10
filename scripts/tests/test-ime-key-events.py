#!/usr/bin/env python3
"""Run the production IME edit methods with fake text clients and dispatchers.

Checks event ordering, single insertion/deletion, and reentrant focus/lifetime
changes. Native CI and phone tests cover the real Aura/Blink/IME integration.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'overlay/ui/ozone/platform/ohos/ohos_input_method.cc').read_text()


def method(name):
    start = SOURCE.index('void OhosInputMethod::' + name + '(')
    opening = SOURCE.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


harness = r'''
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
namespace base {
template<class Signature> class OnceCallback;
template<class R, class... Args> class OnceCallback<R(Args...)> {
 public:
  template<class F> OnceCallback(F f) : fn_(std::move(f)) {}
  R Run(Args... args) { return std::move(fn_)(args...); }
 private:
  std::function<R(Args...)> fn_;
};
template<class F, class... Bound> auto BindOnce(F f, Bound... bound) {
  return [f, args = std::make_tuple(std::move(bound)...)](auto... rest) mutable {
    std::apply([&](auto&&... first) { f(std::move(first)..., rest...); },
               std::move(args));
  };
}
}  // namespace base
enum class EventType { kKeyPressed, kKeyReleased };
constexpr int VKEY_PROCESSKEY = 229;
struct KeyEvent { EventType type; int key;
  KeyEvent(EventType t, int k, int flags) : type(t), key(k) { assert(flags == 0); }
};
struct Details { bool dispatcher_destroyed = false; bool target_destroyed = false; };
struct Weak { std::weak_ptr<int> state;
  explicit operator bool() const { return !state.expired(); }
};
struct WeakFactory {
  std::shared_ptr<int> state = std::make_shared<int>(0);
  Weak GetWeakPtr() { return {state}; }
  void Invalidate() { state.reset(); }
};
struct TextInputClient {
  enum class InsertTextCursorBehavior { kMoveCursorAfterText };
  explicit TextInputClient(std::vector<std::string>* log) : events(log) {}
  std::u16string text;
  std::vector<std::string>* events;
  std::function<void()> after_edit;
  size_t delete_before = 0, delete_after = 0;
  bool composing = false;
  void InsertText(const std::u16string& value, InsertTextCursorBehavior) {
    events->push_back("insert");
    if (composing) text = value;
    else text += value;
    composing = false;
    if (after_edit) after_edit();
  }
  void ExtendSelectionAndDelete(size_t before, size_t after) {
    events->push_back("delete");
    delete_before = before; delete_after = after;
    if (after_edit) after_edit();
  }
};
class OhosInputMethod {
 public:
  std::vector<std::string> events;
  TextInputClient first{&events}, second{&events};
  TextInputClient* current = &first;
  bool type_none = false;
  WeakFactory weak_factory_;
  std::function<void(EventType)> on_dispatch;
  Details details;
  TextInputClient* GetTextInputClient() { return current; }
  bool IsTextInputTypeNone() { return type_none; }
  Details DispatchKeyEventPostIME(KeyEvent* event) {
    assert(event->key == VKEY_PROCESSKEY);
    events.push_back(event->type == EventType::kKeyPressed ? "down" : "up");
    if (on_dispatch) on_dispatch(event->type);
    return details;
  }
  void RefreshTextSnapshot(bool notify) { assert(notify); events.push_back("snapshot"); }
  void NotifyCursorRect() { events.push_back("cursor"); }
  void DispatchImeEdit(base::OnceCallback<void(TextInputClient*)>);
  void HandleInsertText(std::u16string);
  void HandleDelete(size_t, size_t);
};
METHODS
int main() {
  {
    OhosInputMethod ime;
    ime.HandleInsertText(u"6cmF");
    assert(ime.first.text == u"6cmF");
    assert((ime.events == std::vector<std::string>{"down", "insert", "up", "snapshot", "cursor"}));
  }
  {
    OhosInputMethod ime;
    ime.first.text = u"suggestion";
    ime.first.composing = true;
    ime.HandleInsertText(u"中文🙂");
    assert(ime.first.text == u"中文🙂" && !ime.first.composing);
    assert((ime.events == std::vector<std::string>{"down", "insert", "up", "snapshot", "cursor"}));
  }
  {
    OhosInputMethod ime;
    ime.HandleDelete(2, 3);
    assert(ime.first.delete_before == 2 && ime.first.delete_after == 3);
    assert((ime.events == std::vector<std::string>{"down", "delete", "up", "snapshot", "cursor"}));
  }
  for (int state = 0; state < 3; ++state) {
    OhosInputMethod ime;
    if (state == 0) ime.current = nullptr;
    if (state == 1) ime.type_none = true;
    if (state == 2) { ime.HandleInsertText(u""); assert(ime.events.empty()); continue; }
    ime.HandleInsertText(u"a");
    ime.HandleDelete(1, 0);
    assert(ime.events.empty());
  }
  // Down/edit/up may synchronously change focus, disable editing or destroy
  // the dispatcher. Never send text or a trailing keyup to the next field.
  for (int phase = 0; phase < 3; ++phase) {
    for (int cause = 0; cause < 5; ++cause) {
      OhosInputMethod ime;
      auto invalidate = [&] {
        if (cause == 0) ime.current = &ime.second;
        if (cause == 1) ime.type_none = true;
        if (cause == 2) ime.weak_factory_.Invalidate();
        if (cause == 3) ime.details.target_destroyed = true;
        if (cause == 4) ime.details.dispatcher_destroyed = true;
      };
      if (phase == 1 && cause >= 3) continue;
      ime.on_dispatch = [&](EventType type) {
        if ((phase == 0 && type == EventType::kKeyPressed) ||
            (phase == 2 && type == EventType::kKeyReleased)) invalidate();
      };
      if (phase == 1) ime.first.after_edit = invalidate;
      ime.HandleInsertText(u"x");
      const std::vector<std::string> expected = phase == 0 ?
          std::vector<std::string>{"down"} : phase == 1 ?
          std::vector<std::string>{"down", "insert"} :
          std::vector<std::string>{"down", "insert", "up"};
      assert(ime.events == expected);
      assert(ime.second.text.empty());
    }
  }
}
'''
methods = '\n'.join(method(name) for name in (
    'DispatchImeEdit', 'HandleInsertText', 'HandleDelete'))
with tempfile.TemporaryDirectory(prefix='ohos-ime-events-') as directory:
    path = Path(directory)
    (path / 'test.cc').write_text(harness.replace('METHODS', methods))
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20',
                    '-Wall', '-Wextra', '-Werror', str(path / 'test.cc'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('IME edit event ordering, text/deletion and reentrant lifetime checks passed')
