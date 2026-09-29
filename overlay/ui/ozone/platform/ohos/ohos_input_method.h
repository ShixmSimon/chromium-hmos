// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_INPUT_METHOD_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_INPUT_METHOD_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "base/memory/weak_ptr.h"
#include "inputmethod/inputmethod_controller_capi.h"
#include "ui/base/ime/input_method_minimal.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/native_ui_types.h"

namespace ui {

// Bridges Chromium's TextInputClient to the HarmonyOS native input method.
// Raw hardware keys continue through InputMethodMinimal; committed and preview
// text from an IME is delivered through libohinputmethod.
class OhosInputMethod : public InputMethodMinimal {
 public:
  OhosInputMethod(ImeKeyEventDispatcher* ime_key_event_dispatcher,
                  gfx::AcceleratedWidget widget);
  OhosInputMethod(const OhosInputMethod&) = delete;
  OhosInputMethod& operator=(const OhosInputMethod&) = delete;
  ~OhosInputMethod() override;

  void OnFocus() override;
  void OnBlur() override;
  void OnTextInputTypeChanged(TextInputClient* client) override;
  void OnCaretBoundsChanged(const TextInputClient* client) override;
  void CancelComposition(const TextInputClient* client) override;
  void SetVirtualKeyboardVisibilityIfEnabled(bool should_show) override;

 protected:
  void OnDidChangeFocusedClient(TextInputClient* focused_before,
                                TextInputClient* focused) override;

 private:
  static void GetTextConfig(InputMethod_TextEditorProxy* proxy,
                            InputMethod_TextConfig* config);
  static void InsertText(InputMethod_TextEditorProxy* proxy,
                         const char16_t* text,
                         size_t length);
  static void DeleteForward(InputMethod_TextEditorProxy* proxy, int32_t length);
  static void DeleteBackward(InputMethod_TextEditorProxy* proxy,
                             int32_t length);
  static void SendKeyboardStatus(InputMethod_TextEditorProxy* proxy,
                                 InputMethod_KeyboardStatus status);
  static void SendEnterKey(InputMethod_TextEditorProxy* proxy,
                           InputMethod_EnterKeyType type);
  static void MoveCursor(InputMethod_TextEditorProxy* proxy,
                         InputMethod_Direction direction);
  static void HandleSetSelection(InputMethod_TextEditorProxy* proxy,
                                 int32_t start,
                                 int32_t end);
  static void HandleExtendAction(InputMethod_TextEditorProxy* proxy,
                                 InputMethod_ExtendAction action);
  static void GetLeftTextOfCursor(InputMethod_TextEditorProxy* proxy,
                                  int32_t number,
                                  char16_t text[],
                                  size_t* length);
  static void GetRightTextOfCursor(InputMethod_TextEditorProxy* proxy,
                                   int32_t number,
                                   char16_t text[],
                                   size_t* length);
  static int32_t GetTextIndexAtCursor(InputMethod_TextEditorProxy* proxy);
  static int32_t ReceivePrivateCommand(
      InputMethod_TextEditorProxy* proxy,
      InputMethod_PrivateCommand* private_command[],
      size_t size);
  static int32_t SetPreviewText(InputMethod_TextEditorProxy* proxy,
                                const char16_t text[],
                                size_t length,
                                int32_t start,
                                int32_t end);
  static void FinishTextPreview(InputMethod_TextEditorProxy* proxy);

  bool Attach();
  void Detach();
  void UpdateImeState();
  void RefreshTextSnapshot(bool notify_input_method);
  void NotifyCursorRect();
  void DispatchSyntheticKey(KeyboardCode key_code, int flags = 0);

  void HandleInsertText(std::u16string text);
  void HandleDelete(size_t before, size_t after);
  void HandleEnterKey();
  void HandleMoveCursor(InputMethod_Direction direction);
  void HandleSelection(int32_t start, int32_t end);
  void HandleExtend(InputMethod_ExtendAction action);
  void HandlePreviewText(std::u16string text, int32_t start, int32_t end);
  void HandleFinishPreview();

  bool focused_ = false;
  const gfx::AcceleratedWidget widget_;
  InputMethod_TextEditorProxy* text_editor_proxy_ = nullptr;
  InputMethod_AttachOptions* attach_options_ = nullptr;
  InputMethod_InputMethodProxy* input_method_proxy_ = nullptr;

  base::WeakPtrFactory<OhosInputMethod> weak_factory_{this};
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_INPUT_METHOD_H_
