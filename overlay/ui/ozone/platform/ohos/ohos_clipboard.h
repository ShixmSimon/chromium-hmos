// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_CLIPBOARD_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_CLIPBOARD_H_

#include <cstdint>
#include <optional>

#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "ui/ozone/public/platform_clipboard.h"

struct OH_Pasteboard;

namespace ui {

// Copies from the browser into the HarmonyOS pasteboard, so text copied in a
// page can be pasted in other apps.
//
// It never reads the pasteboard back: that needs READ_PASTEBOARD, a
// restricted permission the engine does not declare. Pasting inside the
// browser still works because ClipboardOzone serves reads from the data it
// offered for as long as this reports it owns the clipboard -- which is
// until another app writes to the pasteboard. After that the clipboard reads
// as empty rather than as the browser's stale copy.
class OhosClipboard : public PlatformClipboard {
 public:
  OhosClipboard();
  OhosClipboard(const OhosClipboard&) = delete;
  OhosClipboard& operator=(const OhosClipboard&) = delete;
  ~OhosClipboard() override;

  // PlatformClipboard:
  void OfferClipboardData(ClipboardBuffer buffer,
                          const DataMap& data_map) override;
  void RequestClipboardData(ClipboardBuffer buffer,
                            const std::string& mime_type,
                            RequestDataClosure callback) override;
  void GetAvailableMimeTypes(ClipboardBuffer buffer,
                             GetMimeTypesClosure callback) override;
  void IsSelectionOwner(ClipboardBuffer buffer,
                        IsSelectionOwnerClosure callback) override;
  void SetClipboardDataChangedCallback(
      ClipboardDataChangedCallback callback) override;
  bool IsSelectionBufferAvailable() const override;

  // Another app (or this one) changed the pasteboard.
  void OnPasteboardChanged();

 private:
  bool OwnsPasteboard() const;

  SEQUENCE_CHECKER(sequence_checker_);

  OH_Pasteboard* pasteboard_ = nullptr;
  // The pasteboard's change count right after our last write.
  std::optional<uint32_t> owned_change_count_;
  ClipboardDataChangedCallback data_changed_callback_;

  base::WeakPtrFactory<OhosClipboard> weak_factory_{this};
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_OHOS_OHOS_CLIPBOARD_H_
