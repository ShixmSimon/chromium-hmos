// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/ohos/ohos_clipboard.h"

#include <database/pasteboard/oh_pasteboard.h>
#include <database/pasteboard/oh_pasteboard_err_code.h>
#include <database/udmf/udmf.h>
#include <database/udmf/uds.h>

#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/task/sequenced_task_runner.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/clipboard_constants.h"

namespace ui {

namespace {

std::optional<std::string> ReadString(const PlatformClipboard::DataMap& data,
                                      const char* mime_type) {
  auto it = data.find(mime_type);
  if (it == data.end() || !it->second) {
    return std::nullopt;
  }
  const auto bytes = it->second->as_vector();
  return std::string(bytes.begin(), bytes.end());
}

// The pasteboard observer runs on a pasteboard IPC thread and may fire after
// the clipboard is gone, so what it points at is never freed. There is one
// clipboard per process.
struct ChangeRelay {
  scoped_refptr<base::SequencedTaskRunner> task_runner;
  base::WeakPtr<OhosClipboard> clipboard;

  static void OnChanged(void* context, Pasteboard_NotifyType type) {
    auto* relay = static_cast<ChangeRelay*>(context);
    relay->task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&OhosClipboard::OnPasteboardChanged, relay->clipboard));
  }
};

// Plain text, and HTML when the page copied rich content; other apps pick
// whichever they understand. Returns whether anything was written.
bool WriteToPasteboard(OH_Pasteboard* pasteboard,
                       const std::optional<std::string>& text,
                       const std::optional<std::string>& html) {
  OH_UdmfRecord* record = OH_UdmfRecord_Create();
  if (!record) {
    return false;
  }
  OH_UdsPlainText* plain = nullptr;
  OH_UdsHtml* rich = nullptr;
  if (text) {
    plain = OH_UdsPlainText_Create();
    if (plain) {
      OH_UdsPlainText_SetContent(plain, text->c_str());
      OH_UdmfRecord_AddPlainText(record, plain);
    }
  }
  if (html) {
    rich = OH_UdsHtml_Create();
    if (rich) {
      OH_UdsHtml_SetContent(rich, html->c_str());
      if (text) {
        OH_UdsHtml_SetPlainContent(rich, text->c_str());
      }
      OH_UdmfRecord_AddHtml(record, rich);
    }
  }

  bool written = false;
  if (OH_UdmfData* data = OH_UdmfData_Create()) {
    if (OH_UdmfData_AddRecord(data, record) == 0) {
      const int result = OH_Pasteboard_SetData(pasteboard, data);
      written = result == ERR_OK;
      if (!written) {
        LOG(WARNING) << "OH_Pasteboard_SetData failed: " << result;
      }
    }
    OH_UdmfData_Destroy(data);
  }
  if (plain) {
    OH_UdsPlainText_Destroy(plain);
  }
  if (rich) {
    OH_UdsHtml_Destroy(rich);
  }
  OH_UdmfRecord_Destroy(record);
  return written;
}

}  // namespace

OhosClipboard::OhosClipboard() : pasteboard_(OH_Pasteboard_Create()) {
  if (!pasteboard_) {
    LOG(ERROR) << "OH_Pasteboard_Create failed; copy stays inside the browser";
    return;
  }
  OH_PasteboardObserver* observer = OH_PasteboardObserver_Create();
  if (!observer) {
    return;
  }
  auto* relay = new ChangeRelay{base::SequencedTaskRunner::GetCurrentDefault(),
                                weak_factory_.GetWeakPtr()};
  OH_PasteboardObserver_SetData(observer, relay, &ChangeRelay::OnChanged,
                                /*finalize=*/nullptr);
  OH_Pasteboard_Subscribe(pasteboard_, NOTIFY_LOCAL_DATA_CHANGE, observer);
  OH_Pasteboard_Subscribe(pasteboard_, NOTIFY_REMOTE_DATA_CHANGE, observer);
}

OhosClipboard::~OhosClipboard() = default;

void OhosClipboard::OfferClipboardData(ClipboardBuffer buffer,
                                       const DataMap& data_map) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (buffer != ClipboardBuffer::kCopyPaste || !pasteboard_) {
    return;
  }
  std::optional<std::string> text = ReadString(data_map, kMimeTypePlainText);
  if (!text) {
    text = ReadString(data_map, kMimeTypeUtf8PlainText);
  }
  const std::optional<std::string> html = ReadString(data_map, kMimeTypeHtml);
  if (!text && !html) {
    // Images, files and custom formats stay inside the browser for now.
    owned_change_count_ = std::nullopt;
    return;
  }
  if (!WriteToPasteboard(pasteboard_, text, html)) {
    owned_change_count_ = std::nullopt;
    return;
  }
  owned_change_count_ = OH_Pasteboard_GetChangeCount(pasteboard_);
}

void OhosClipboard::RequestClipboardData(ClipboardBuffer buffer,
                                         const std::string& mime_type,
                                         RequestDataClosure callback) {
  // Only reached when another app owns the pasteboard, which this does not
  // read (see the header).
  std::move(callback).Run({});
}

void OhosClipboard::GetAvailableMimeTypes(ClipboardBuffer buffer,
                                          GetMimeTypesClosure callback) {
  std::move(callback).Run({});
}

void OhosClipboard::IsSelectionOwner(ClipboardBuffer buffer,
                                     IsSelectionOwnerClosure callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::move(callback).Run(buffer == ClipboardBuffer::kCopyPaste &&
                          OwnsPasteboard());
}

void OhosClipboard::SetClipboardDataChangedCallback(
    ClipboardDataChangedCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  data_changed_callback_ = std::move(callback);
}

bool OhosClipboard::IsSelectionBufferAvailable() const {
  return false;
}

void OhosClipboard::OnPasteboardChanged() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Our own write notifies too; only someone else's changes what we own.
  if (OwnsPasteboard()) {
    return;
  }
  owned_change_count_ = std::nullopt;
  if (data_changed_callback_) {
    data_changed_callback_.Run(ClipboardBuffer::kCopyPaste);
  }
}

bool OhosClipboard::OwnsPasteboard() const {
  return owned_change_count_ && pasteboard_ &&
         OH_Pasteboard_GetChangeCount(pasteboard_) == *owned_change_count_;
}

}  // namespace ui
