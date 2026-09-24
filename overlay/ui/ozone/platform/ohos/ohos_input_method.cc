// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/ohos/ohos_input_method.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "ui/base/ime/composition_text.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/base/ime/text_input_type.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/range/range.h"
#include "ui/ozone/platform/ohos/ohos_event_source.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"

namespace ui {
namespace {

constexpr size_t kMaximumImeTextLength = 8192;

struct ProxySnapshot {
  InputMethod_TextInputType input_type = IME_TEXT_INPUT_TYPE_TEXT;
  InputMethod_EnterKeyType enter_key_type = IME_ENTER_KEY_UNSPECIFIED;
  std::u16string text;
  int32_t selection_start = 0;
  int32_t selection_end = 0;
  int32_t window_id = 0;
};

struct ProxyEntry {
  scoped_refptr<base::SingleThreadTaskRunner> task_runner;
  base::WeakPtr<OhosInputMethod> input_method;
  ProxySnapshot snapshot;
};

class ProxyRegistry {
 public:
  void Register(InputMethod_TextEditorProxy* proxy, ProxyEntry entry) {
    base::AutoLock lock(lock_);
    entries_[proxy] = std::move(entry);
  }

  void Unregister(InputMethod_TextEditorProxy* proxy) {
    base::AutoLock lock(lock_);
    entries_.erase(proxy);
  }

  void UpdateSnapshot(InputMethod_TextEditorProxy* proxy,
                      ProxySnapshot snapshot) {
    base::AutoLock lock(lock_);
    auto entry = entries_.find(proxy);
    if (entry != entries_.end()) {
      entry->second.snapshot = std::move(snapshot);
    }
  }

  std::optional<ProxyEntry> Lookup(InputMethod_TextEditorProxy* proxy) {
    base::AutoLock lock(lock_);
    auto entry = entries_.find(proxy);
    if (entry == entries_.end()) {
      return std::nullopt;
    }
    return entry->second;
  }

 private:
  base::Lock lock_;
  std::map<InputMethod_TextEditorProxy*, ProxyEntry> entries_ GUARDED_BY(lock_);
};

ProxyRegistry& GetProxyRegistry() {
  static base::NoDestructor<ProxyRegistry> registry;
  return *registry;
}

void PostToInputMethod(InputMethod_TextEditorProxy* proxy,
                       base::OnceCallback<void(OhosInputMethod*)> callback) {
  std::optional<ProxyEntry> entry = GetProxyRegistry().Lookup(proxy);
  if (!entry || !entry->task_runner) {
    return;
  }

  entry->task_runner->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<OhosInputMethod> input_method,
                        base::OnceCallback<void(OhosInputMethod*)> callback) {
                       if (input_method) {
                         std::move(callback).Run(input_method.get());
                       }
                     },
                     entry->input_method, std::move(callback)));
}

InputMethod_TextInputType ToOhosInputType(TextInputType type) {
  switch (type) {
    case TEXT_INPUT_TYPE_NONE:
      return IME_TEXT_INPUT_TYPE_NONE;
    case TEXT_INPUT_TYPE_PASSWORD:
      // What ArkUI's own TextInput sends for InputType.Password (both are 7):
      // the system's secure password keyboard. SCREEN_LOCK_PASSWORD is the
      // lock screen's own keyboard, which the IME draws as a translucent
      // overlay rather than its password keyboard.
      return IME_TEXT_INPUT_TYPE_VISIBLE_PASSWORD;
    case TEXT_INPUT_TYPE_EMAIL:
      return IME_TEXT_INPUT_TYPE_EMAIL_ADDRESS;
    case TEXT_INPUT_TYPE_NUMBER:
      return IME_TEXT_INPUT_TYPE_NUMBER_DECIMAL;
    case TEXT_INPUT_TYPE_TELEPHONE:
      return IME_TEXT_INPUT_TYPE_PHONE;
    case TEXT_INPUT_TYPE_URL:
      return IME_TEXT_INPUT_TYPE_URL;
    case TEXT_INPUT_TYPE_DATE:
    case TEXT_INPUT_TYPE_DATE_TIME:
    case TEXT_INPUT_TYPE_DATE_TIME_LOCAL:
    case TEXT_INPUT_TYPE_MONTH:
    case TEXT_INPUT_TYPE_TIME:
    case TEXT_INPUT_TYPE_WEEK:
    case TEXT_INPUT_TYPE_DATE_TIME_FIELD:
      return IME_TEXT_INPUT_TYPE_DATETIME;
    case TEXT_INPUT_TYPE_TEXT_AREA:
    case TEXT_INPUT_TYPE_CONTENT_EDITABLE:
      return IME_TEXT_INPUT_TYPE_MULTILINE;
    case TEXT_INPUT_TYPE_TEXT:
    case TEXT_INPUT_TYPE_SEARCH:
    case TEXT_INPUT_TYPE_NULL:
      return IME_TEXT_INPUT_TYPE_TEXT;
  }
}

InputMethod_EnterKeyType ToOhosEnterKeyType(TextInputType type) {
  switch (type) {
    case TEXT_INPUT_TYPE_SEARCH:
      return IME_ENTER_KEY_SEARCH;
    case TEXT_INPUT_TYPE_TEXT_AREA:
    case TEXT_INPUT_TYPE_CONTENT_EDITABLE:
      return IME_ENTER_KEY_NEWLINE;
    default:
      return IME_ENTER_KEY_UNSPECIFIED;
  }
}

int32_t ClampToInt32(size_t value) {
  return static_cast<int32_t>(std::min(
      value, static_cast<size_t>(std::numeric_limits<int32_t>::max())));
}

void CopyTextForIme(const ProxySnapshot& snapshot,
                    bool left_of_cursor,
                    int32_t requested,
                    char16_t output[],
                    size_t* output_length) {
  if (!output_length || requested <= 0) {
    if (output_length) {
      *output_length = 0;
    }
    return;
  }

  const size_t cursor =
      std::min(static_cast<size_t>(std::max(snapshot.selection_end, 0)),
               snapshot.text.size());
  const size_t count = left_of_cursor
                           ? std::min(static_cast<size_t>(requested), cursor)
                           : std::min(static_cast<size_t>(requested),
                                      snapshot.text.size() - cursor);
  if (output && count > 0) {
    const size_t start = left_of_cursor ? cursor - count : cursor;
    std::copy_n(snapshot.text.data() + start, count, output);
  }
  *output_length = count;
}

}  // namespace

OhosInputMethod::OhosInputMethod(
    ImeKeyEventDispatcher* ime_key_event_dispatcher,
    gfx::AcceleratedWidget widget)
    : InputMethodMinimal(ime_key_event_dispatcher), widget_(widget) {}

OhosInputMethod::~OhosInputMethod() {
  Detach();
}

void OhosInputMethod::OnFocus() {
  InputMethodMinimal::OnFocus();
  focused_ = true;
  UpdateImeState();
}

void OhosInputMethod::OnBlur() {
  focused_ = false;
  Detach();
  InputMethodMinimal::OnBlur();
}

void OhosInputMethod::OnTextInputTypeChanged(TextInputClient* client) {
  InputMethodMinimal::OnTextInputTypeChanged(client);
  if (IsTextInputClientFocused(client)) {
    UpdateImeState();
  }
}

void OhosInputMethod::OnCaretBoundsChanged(const TextInputClient* client) {
  if (!IsTextInputClientFocused(client)) {
    return;
  }
  NotifyTextInputCaretBoundsChanged(client);
  RefreshTextSnapshot(true);
  NotifyCursorRect();
}

void OhosInputMethod::CancelComposition(const TextInputClient* client) {
  if (!IsTextInputClientFocused(client) || !client->HasCompositionText()) {
    return;
  }
  GetTextInputClient()->ClearCompositionText();
  RefreshTextSnapshot(true);
}

void OhosInputMethod::OnDidChangeFocusedClient(TextInputClient* focused_before,
                                               TextInputClient* focused) {
  (void)focused_before;
  (void)focused;
  UpdateImeState();
}

bool OhosInputMethod::Attach() {
  text_editor_proxy_ = OH_TextEditorProxy_Create();
  if (!text_editor_proxy_) {
    LOG(ERROR) << "HarmonyOS IME failed to create text editor proxy";
    return false;
  }

  const bool callbacks_registered =
      OH_TextEditorProxy_SetGetTextConfigFunc(text_editor_proxy_,
                                              &GetTextConfig) == IME_ERR_OK &&
      OH_TextEditorProxy_SetInsertTextFunc(text_editor_proxy_, &InsertText) ==
          IME_ERR_OK &&
      OH_TextEditorProxy_SetDeleteForwardFunc(text_editor_proxy_,
                                              &DeleteForward) == IME_ERR_OK &&
      OH_TextEditorProxy_SetDeleteBackwardFunc(text_editor_proxy_,
                                               &DeleteBackward) == IME_ERR_OK &&
      OH_TextEditorProxy_SetSendKeyboardStatusFunc(
          text_editor_proxy_, &SendKeyboardStatus) == IME_ERR_OK &&
      OH_TextEditorProxy_SetSendEnterKeyFunc(text_editor_proxy_,
                                             &SendEnterKey) == IME_ERR_OK &&
      OH_TextEditorProxy_SetMoveCursorFunc(text_editor_proxy_, &MoveCursor) ==
          IME_ERR_OK &&
      OH_TextEditorProxy_SetHandleSetSelectionFunc(
          text_editor_proxy_, &HandleSetSelection) == IME_ERR_OK &&
      OH_TextEditorProxy_SetHandleExtendActionFunc(
          text_editor_proxy_, &HandleExtendAction) == IME_ERR_OK &&
      OH_TextEditorProxy_SetGetLeftTextOfCursorFunc(
          text_editor_proxy_, &GetLeftTextOfCursor) == IME_ERR_OK &&
      OH_TextEditorProxy_SetGetRightTextOfCursorFunc(
          text_editor_proxy_, &GetRightTextOfCursor) == IME_ERR_OK &&
      OH_TextEditorProxy_SetGetTextIndexAtCursorFunc(
          text_editor_proxy_, &GetTextIndexAtCursor) == IME_ERR_OK &&
      OH_TextEditorProxy_SetReceivePrivateCommandFunc(
          text_editor_proxy_, &ReceivePrivateCommand) == IME_ERR_OK &&
      OH_TextEditorProxy_SetSetPreviewTextFunc(text_editor_proxy_,
                                               &SetPreviewText) == IME_ERR_OK &&
      OH_TextEditorProxy_SetFinishTextPreviewFunc(
          text_editor_proxy_, &FinishTextPreview) == IME_ERR_OK;
  if (!callbacks_registered) {
    LOG(ERROR) << "HarmonyOS IME failed to register editor callbacks";
    OH_TextEditorProxy_Destroy(text_editor_proxy_);
    text_editor_proxy_ = nullptr;
    return false;
  }

  bool show_keyboard = false;
  InputMethod_RequestKeyboardReason request_reason = IME_REQUEST_REASON_OTHER;
  if (TextInputClient* client = GetTextInputClient()) {
    switch (client->GetFocusReason()) {
      case TextInputClient::FOCUS_REASON_TOUCH:
      case TextInputClient::FOCUS_REASON_PEN:
        show_keyboard = true;
        request_reason = IME_REQUEST_REASON_TOUCH;
        break;
      case TextInputClient::FOCUS_REASON_MOUSE:
        request_reason = IME_REQUEST_REASON_MOUSE;
        break;
      default:
        if (OhosEventSource::WasTouchInteractionRecent()) {
          show_keyboard = true;
          request_reason = IME_REQUEST_REASON_TOUCH;
        }
        break;
    }
  }

  attach_options_ = OH_AttachOptions_CreateWithRequestKeyboardReason(
      show_keyboard, request_reason);
  if (!attach_options_) {
    LOG(ERROR) << "HarmonyOS IME failed to create attach options";
    OH_TextEditorProxy_Destroy(text_editor_proxy_);
    text_editor_proxy_ = nullptr;
    return false;
  }

  ProxySnapshot initial_snapshot;
  initial_snapshot.window_id = GetOhosApplicationWindowIdForWidget(widget_);
  GetProxyRegistry().Register(
      text_editor_proxy_, {base::SingleThreadTaskRunner::GetCurrentDefault(),
                           weak_factory_.GetWeakPtr(), initial_snapshot});
  RefreshTextSnapshot(false);

  const InputMethod_ErrorCode result = OH_InputMethodController_Attach(
      text_editor_proxy_, attach_options_, &input_method_proxy_);
  if (result != IME_ERR_OK || !input_method_proxy_) {
    LOG(ERROR) << "HarmonyOS IME attach failed result=" << result;
    GetProxyRegistry().Unregister(text_editor_proxy_);
    OH_AttachOptions_Destroy(attach_options_);
    OH_TextEditorProxy_Destroy(text_editor_proxy_);
    attach_options_ = nullptr;
    text_editor_proxy_ = nullptr;
    input_method_proxy_ = nullptr;
    return false;
  }

  LOG(INFO) << "HarmonyOS IME attached windowId="
            << GetOhosApplicationWindowIdForWidget(widget_)
            << " showKeyboard=" << show_keyboard;
  OnInputMethodChanged();
  NotifyCursorRect();
  return true;
}

void OhosInputMethod::Detach() {
  if (!text_editor_proxy_) {
    return;
  }

  if (input_method_proxy_) {
    const InputMethod_ErrorCode result =
        OH_InputMethodController_Detach(input_method_proxy_);
    if (result != IME_ERR_OK) {
      LOG(WARNING) << "HarmonyOS IME detach failed result=" << result;
    }
  }

  GetProxyRegistry().Unregister(text_editor_proxy_);
  if (attach_options_) {
    OH_AttachOptions_Destroy(attach_options_);
  }
  OH_TextEditorProxy_Destroy(text_editor_proxy_);
  input_method_proxy_ = nullptr;
  attach_options_ = nullptr;
  text_editor_proxy_ = nullptr;
}

void OhosInputMethod::UpdateImeState() {
  if (!focused_ || IsTextInputTypeNone()) {
    Detach();
    return;
  }

  if (!input_method_proxy_ && !Attach()) {
    return;
  }

  RefreshTextSnapshot(true);
  const TextInputType type = GetTextInputType();
  const InputMethod_ErrorCode result =
      OH_InputMethodProxy_NotifyConfigurationChange(
          input_method_proxy_, ToOhosEnterKeyType(type), ToOhosInputType(type));
  if (result != IME_ERR_OK) {
    LOG(WARNING) << "HarmonyOS IME configuration update failed result="
                 << result;
  }
}

void OhosInputMethod::RefreshTextSnapshot(bool notify_input_method) {
  if (!text_editor_proxy_) {
    return;
  }

  ProxySnapshot snapshot;
  const TextInputType input_type = GetTextInputType();
  snapshot.input_type = ToOhosInputType(input_type);
  snapshot.enter_key_type = ToOhosEnterKeyType(input_type);
  snapshot.window_id = GetOhosApplicationWindowIdForWidget(widget_);

  TextInputClient* client = GetTextInputClient();
  gfx::Range text_range;
  gfx::Range selection_range;
  if (client && client->GetTextRange(&text_range) && text_range.IsValid() &&
      text_range.length() <= kMaximumImeTextLength) {
    client->GetTextFromRange(text_range, &snapshot.text);
  }
  if (client && client->GetEditableSelectionRange(&selection_range) &&
      selection_range.IsValid()) {
    snapshot.selection_start = ClampToInt32(selection_range.start());
    snapshot.selection_end = ClampToInt32(selection_range.end());
  }

  GetProxyRegistry().UpdateSnapshot(text_editor_proxy_, snapshot);
  if (!notify_input_method || !input_method_proxy_) {
    return;
  }

  const InputMethod_ErrorCode result =
      OH_InputMethodProxy_NotifySelectionChange(
          input_method_proxy_, snapshot.text.data(), snapshot.text.size(),
          snapshot.selection_start, snapshot.selection_end);
  if (result != IME_ERR_OK) {
    LOG(WARNING) << "HarmonyOS IME selection update failed result=" << result;
  }
}

void OhosInputMethod::NotifyCursorRect() {
  if (!input_method_proxy_ || !GetTextInputClient()) {
    return;
  }

  const gfx::Rect caret = GetTextInputClient()->GetCaretBounds();
  float density = 1.0f;
  if (std::optional<OhosDisplayMetrics> metrics = GetOhosDisplayMetrics()) {
    density = metrics->density;
  }
  InputMethod_CursorInfo* cursor_info =
      OH_CursorInfo_Create(caret.x() * density, caret.y() * density,
                           caret.width() * density, caret.height() * density);
  if (!cursor_info) {
    return;
  }
  OH_InputMethodProxy_NotifyCursorUpdate(input_method_proxy_, cursor_info);
  OH_CursorInfo_Destroy(cursor_info);
}

void OhosInputMethod::DispatchSyntheticKey(KeyboardCode key_code, int flags) {
  KeyEvent pressed(EventType::kKeyPressed, key_code, flags,
                   base::TimeTicks::Now());
  InputMethodMinimal::DispatchKeyEvent(&pressed);
  KeyEvent released(EventType::kKeyReleased, key_code, flags,
                    base::TimeTicks::Now());
  InputMethodMinimal::DispatchKeyEvent(&released);
}

void OhosInputMethod::HandleInsertText(std::u16string text) {
  if (text.empty() || IsTextInputTypeNone() || !GetTextInputClient()) {
    return;
  }
  GetTextInputClient()->InsertText(
      text, TextInputClient::InsertTextCursorBehavior::kMoveCursorAfterText);
  RefreshTextSnapshot(true);
  NotifyCursorRect();
}

void OhosInputMethod::HandleDelete(size_t before, size_t after) {
  if (IsTextInputTypeNone() || !GetTextInputClient()) {
    return;
  }
  GetTextInputClient()->ExtendSelectionAndDelete(before, after);
  RefreshTextSnapshot(true);
  NotifyCursorRect();
}

void OhosInputMethod::HandleEnterKey() {
  DispatchSyntheticKey(VKEY_RETURN);
}

void OhosInputMethod::HandleMoveCursor(InputMethod_Direction direction) {
  switch (direction) {
    case IME_DIRECTION_UP:
      DispatchSyntheticKey(VKEY_UP);
      break;
    case IME_DIRECTION_DOWN:
      DispatchSyntheticKey(VKEY_DOWN);
      break;
    case IME_DIRECTION_LEFT:
      DispatchSyntheticKey(VKEY_LEFT);
      break;
    case IME_DIRECTION_RIGHT:
      DispatchSyntheticKey(VKEY_RIGHT);
      break;
    case IME_DIRECTION_NONE:
      return;
  }
  RefreshTextSnapshot(true);
  NotifyCursorRect();
}

void OhosInputMethod::HandleSelection(int32_t start, int32_t end) {
  if (!GetTextInputClient() || start < 0 || end < 0) {
    return;
  }
  GetTextInputClient()->SetEditableSelectionRange(
      gfx::Range(static_cast<size_t>(start), static_cast<size_t>(end)));
  RefreshTextSnapshot(true);
  NotifyCursorRect();
}

void OhosInputMethod::HandleExtend(InputMethod_ExtendAction action) {
  switch (action) {
    case IME_EXTEND_ACTION_SELECT_ALL: {
      gfx::Range text_range;
      if (GetTextInputClient() &&
          GetTextInputClient()->GetTextRange(&text_range) &&
          text_range.IsValid()) {
        GetTextInputClient()->SetEditableSelectionRange(text_range);
      }
      break;
    }
    case IME_EXTEND_ACTION_CUT:
      DispatchSyntheticKey(VKEY_X, EF_CONTROL_DOWN);
      break;
    case IME_EXTEND_ACTION_COPY:
      DispatchSyntheticKey(VKEY_C, EF_CONTROL_DOWN);
      break;
    case IME_EXTEND_ACTION_PASTE:
      DispatchSyntheticKey(VKEY_V, EF_CONTROL_DOWN);
      break;
  }
  RefreshTextSnapshot(true);
}

void OhosInputMethod::HandlePreviewText(std::u16string text,
                                        int32_t start,
                                        int32_t end) {
  if (!GetTextInputClient() || IsTextInputTypeNone()) {
    return;
  }
  CompositionText composition;
  composition.text = std::move(text);
  const size_t selection_start = std::min(
      static_cast<size_t>(std::max(start, 0)), composition.text.size());
  const size_t selection_end =
      std::min(static_cast<size_t>(std::max(end, 0)), composition.text.size());
  composition.selection = gfx::Range(selection_start, selection_end);
  GetTextInputClient()->SetCompositionText(composition);
  NotifyCursorRect();
}

void OhosInputMethod::HandleFinishPreview() {
  if (!GetTextInputClient() || !GetTextInputClient()->HasCompositionText()) {
    return;
  }
  GetTextInputClient()->ConfirmCompositionText(false);
  RefreshTextSnapshot(true);
  NotifyCursorRect();
}

void OhosInputMethod::GetTextConfig(InputMethod_TextEditorProxy* proxy,
                                    InputMethod_TextConfig* config) {
  std::optional<ProxyEntry> entry = GetProxyRegistry().Lookup(proxy);
  if (!entry || !config) {
    return;
  }
  OH_TextConfig_SetInputType(config, entry->snapshot.input_type);
  OH_TextConfig_SetEnterKeyType(config, entry->snapshot.enter_key_type);
  OH_TextConfig_SetPreviewTextSupport(config, true);
  OH_TextConfig_SetSelection(config, entry->snapshot.selection_start,
                             entry->snapshot.selection_end);
  const int32_t window_id = entry->snapshot.window_id;
  if (window_id > 0) {
    OH_TextConfig_SetWindowId(config, window_id);
  }
}

void OhosInputMethod::InsertText(InputMethod_TextEditorProxy* proxy,
                                 const char16_t* text,
                                 size_t length) {
  if (!text || length == 0) {
    return;
  }
  std::u16string value(text, length);
  while (!value.empty() && value.back() == u'\0') {
    value.pop_back();
  }
  PostToInputMethod(
      proxy, base::BindOnce(
                 [](std::u16string value, OhosInputMethod* input_method) {
                   input_method->HandleInsertText(std::move(value));
                 },
                 std::move(value)));
}

void OhosInputMethod::DeleteForward(InputMethod_TextEditorProxy* proxy,
                                    int32_t length) {
  PostToInputMethod(proxy,
                    base::BindOnce(
                        [](size_t length, OhosInputMethod* input_method) {
                          input_method->HandleDelete(0, length);
                        },
                        static_cast<size_t>(std::max(length, 0))));
}

void OhosInputMethod::DeleteBackward(InputMethod_TextEditorProxy* proxy,
                                     int32_t length) {
  PostToInputMethod(proxy,
                    base::BindOnce(
                        [](size_t length, OhosInputMethod* input_method) {
                          input_method->HandleDelete(length, 0);
                        },
                        static_cast<size_t>(std::max(length, 0))));
}

void OhosInputMethod::SendKeyboardStatus(InputMethod_TextEditorProxy* proxy,
                                         InputMethod_KeyboardStatus status) {
  (void)proxy;
  (void)status;
}

void OhosInputMethod::SendEnterKey(InputMethod_TextEditorProxy* proxy,
                                   InputMethod_EnterKeyType type) {
  (void)type;
  PostToInputMethod(proxy, base::BindOnce([](OhosInputMethod* input_method) {
                      input_method->HandleEnterKey();
                    }));
}

void OhosInputMethod::MoveCursor(InputMethod_TextEditorProxy* proxy,
                                 InputMethod_Direction direction) {
  PostToInputMethod(proxy, base::BindOnce(
                               [](InputMethod_Direction direction,
                                  OhosInputMethod* input_method) {
                                 input_method->HandleMoveCursor(direction);
                               },
                               direction));
}

void OhosInputMethod::HandleSetSelection(InputMethod_TextEditorProxy* proxy,
                                         int32_t start,
                                         int32_t end) {
  PostToInputMethod(
      proxy, base::BindOnce(
                 [](int32_t start, int32_t end, OhosInputMethod* input_method) {
                   input_method->HandleSelection(start, end);
                 },
                 start, end));
}

void OhosInputMethod::HandleExtendAction(InputMethod_TextEditorProxy* proxy,
                                         InputMethod_ExtendAction action) {
  PostToInputMethod(proxy, base::BindOnce(
                               [](InputMethod_ExtendAction action,
                                  OhosInputMethod* input_method) {
                                 input_method->HandleExtend(action);
                               },
                               action));
}

void OhosInputMethod::GetLeftTextOfCursor(InputMethod_TextEditorProxy* proxy,
                                          int32_t number,
                                          char16_t text[],
                                          size_t* length) {
  std::optional<ProxyEntry> entry = GetProxyRegistry().Lookup(proxy);
  if (!entry) {
    if (length) {
      *length = 0;
    }
    return;
  }
  CopyTextForIme(entry->snapshot, true, number, text, length);
}

void OhosInputMethod::GetRightTextOfCursor(InputMethod_TextEditorProxy* proxy,
                                           int32_t number,
                                           char16_t text[],
                                           size_t* length) {
  std::optional<ProxyEntry> entry = GetProxyRegistry().Lookup(proxy);
  if (!entry) {
    if (length) {
      *length = 0;
    }
    return;
  }
  CopyTextForIme(entry->snapshot, false, number, text, length);
}

int32_t OhosInputMethod::GetTextIndexAtCursor(
    InputMethod_TextEditorProxy* proxy) {
  std::optional<ProxyEntry> entry = GetProxyRegistry().Lookup(proxy);
  return entry ? entry->snapshot.selection_end : 0;
}

int32_t OhosInputMethod::ReceivePrivateCommand(
    InputMethod_TextEditorProxy* proxy,
    InputMethod_PrivateCommand* private_command[],
    size_t size) {
  (void)proxy;
  (void)private_command;
  (void)size;
  return 0;
}

int32_t OhosInputMethod::SetPreviewText(InputMethod_TextEditorProxy* proxy,
                                        const char16_t text[],
                                        size_t length,
                                        int32_t start,
                                        int32_t end) {
  if (!text) {
    return IME_ERR_NULL_POINTER;
  }
  PostToInputMethod(proxy, base::BindOnce(
                               [](std::u16string text, int32_t start,
                                  int32_t end, OhosInputMethod* input_method) {
                                 input_method->HandlePreviewText(
                                     std::move(text), start, end);
                               },
                               std::u16string(text, length), start, end));
  return IME_ERR_OK;
}

void OhosInputMethod::FinishTextPreview(InputMethod_TextEditorProxy* proxy) {
  PostToInputMethod(proxy, base::BindOnce([](OhosInputMethod* input_method) {
                      input_method->HandleFinishPreview();
                    }));
}

}  // namespace ui
