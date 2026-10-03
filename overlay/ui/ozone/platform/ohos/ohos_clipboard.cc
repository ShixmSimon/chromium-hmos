// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/ohos/ohos_clipboard.h"

#include <database/pasteboard/oh_pasteboard.h>
#include <database/pasteboard/oh_pasteboard_err_code.h>
#include <database/udmf/udmf.h>
#include <database/udmf/uds.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

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

std::optional<std::vector<uint8_t>> ReadBytes(
    const PlatformClipboard::DataMap& data,
    const char* mime_type) {
  auto it = data.find(mime_type);
  if (it == data.end() || !it->second || it->second->size() == 0) {
    return std::nullopt;
  }
  const auto bytes = it->second->as_vector();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// A copied picture as the system's PixelMap, which is what other apps paste
// from: WeChat and the gallery read no PNG bytes. Null when it will not
// decode.
OH_PixelmapNative* DecodePng(std::vector<uint8_t>& png) {
  OH_ImageSourceNative* source = nullptr;
  if (OH_ImageSourceNative_CreateFromData(png.data(), png.size(), &source) !=
          IMAGE_SUCCESS ||
      !source) {
    return nullptr;
  }
  OH_DecodingOptions* options = nullptr;
  OH_PixelmapNative* pixelmap = nullptr;
  if (OH_DecodingOptions_Create(&options) == IMAGE_SUCCESS) {
    if (OH_ImageSourceNative_CreatePixelmap(source, options, &pixelmap) !=
        IMAGE_SUCCESS) {
      pixelmap = nullptr;
    }
    OH_DecodingOptions_Release(options);
  }
  OH_ImageSourceNative_Release(source);
  return pixelmap;
}

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
    // The pasteboard has called this with a null context (two crashes on
    // PLA-AL10 when another app copied). Nothing can be routed without one.
    if (!context) {
      return;
    }
    auto* relay = static_cast<ChangeRelay*>(context);
    relay->task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&OhosClipboard::OnPasteboardChanged, relay->clipboard));
  }

  // The relay is never freed (see above), so there is nothing to release.
  static void OnFinalize(void* context) {}
};

// Plain text, and HTML when the page copied rich content -- or a picture
// alone when one was copied; other apps pick whichever they understand.
// Returns whether anything was written.
bool WriteToPasteboard(OH_Pasteboard* pasteboard,
                       const std::optional<std::string>& text,
                       const std::optional<std::string>& html,
                       OH_PixelmapNative* image) {
  OH_UdmfRecord* record = OH_UdmfRecord_Create();
  if (!record) {
    return false;
  }
  OH_UdsPlainText* plain = nullptr;
  OH_UdsHtml* rich = nullptr;
  OH_UdsPixelMap* picture = nullptr;
  if (image) {
    // A copied picture goes alone. The HTML beside it is only an <img> that
    // points back at the page, and the pasteboard reports a record by its
    // first type: with the HTML first, every app saw text/html and pasted
    // nothing.
    picture = OH_UdsPixelMap_Create();
    const int set = picture ? OH_UdsPixelMap_SetPixelMap(picture, image) : -1;
    const int added = set == 0 ? OH_UdmfRecord_AddPixelMap(record, picture) : -1;
    if (added != 0) {
      LOG(WARNING) << "Copied image not added to the pasteboard: set=" << set
                   << " add=" << added;
    }
  } else if (text) {
    plain = OH_UdsPlainText_Create();
    if (plain) {
      OH_UdsPlainText_SetContent(plain, text->c_str());
      OH_UdmfRecord_AddPlainText(record, plain);
    }
  }
  if (html && !image) {
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
  if (picture) {
    OH_UdsPixelMap_Destroy(picture);
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
  // Only subscribe once the context is really attached; a null finalize is
  // one way to have SetData refuse it.
  if (OH_PasteboardObserver_SetData(observer, relay, &ChangeRelay::OnChanged,
                                    &ChangeRelay::OnFinalize) != ERR_OK) {
    LOG(ERROR) << "Pasteboard observer rejected its context; not listening";
    OH_PasteboardObserver_Destroy(observer);
    return;
  }
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
  // "Copy image" and a page's clipboard.write() of a picture both arrive as
  // PNG (ClipboardOzone::WriteBitmap).
  OH_PixelmapNative* image = nullptr;
  if (std::optional<std::vector<uint8_t>> png =
          ReadBytes(data_map, kMimeTypePng)) {
    image = DecodePng(*png);
    if (!image) {
      LOG(WARNING) << "Copied image did not decode; not on the pasteboard";
    }
  }
  if (!text && !html && !image) {
    // Files and custom formats stay inside the browser for now.
    owned_change_count_ = std::nullopt;
    return;
  }
  const bool written = WriteToPasteboard(pasteboard_, text, html, image);
  if (image) {
    OH_PixelmapNative_Release(image);
  }
  if (!written) {
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
