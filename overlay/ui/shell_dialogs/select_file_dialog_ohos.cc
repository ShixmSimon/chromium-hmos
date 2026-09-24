// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/shell_dialogs/select_file_dialog_ohos.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <utility>

#include <filemanagement/file_uri/oh_file_uri.h>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"

namespace ui {
namespace {

class SelectFileDialogOhos;

struct OhosFileDialogState {
  base::Lock lock;
  int next_request_id GUARDED_BY(lock) = 1;
  OhosSelectFileDialogRequestCallback request_callback GUARDED_BY(lock);
  std::map<int, scoped_refptr<SelectFileDialogOhos>> pending GUARDED_BY(lock);
};

OhosFileDialogState& GetOhosFileDialogState() {
  static base::NoDestructor<OhosFileDialogState> state;
  return *state;
}

base::FilePath PickerUriToPath(const std::string& uri) {
  if (!OH_FileUri_IsValidUri(uri.c_str(), uri.size())) {
    return base::FilePath(uri);
  }

  char* converted_path = nullptr;
  const FileManagement_ErrCode result =
      OH_FileUri_GetPathFromUri(uri.c_str(), uri.size(), &converted_path);
  if (result != ERR_OK || !converted_path) {
    return base::FilePath();
  }
  // The string comes from the system allocator, but free() here is
  // PartitionAlloc's shim, which crashes (FreeInUnknownRoot) on a pointer it
  // did not hand out -- every "save as" answered by the shell hit it. It is
  // left alone instead: one path per file picked.
  return base::FilePath(converted_path);
}

std::vector<std::string> BuildSuffixFilters(
    const SelectFileDialog::FileTypeInfo* file_types) {
  std::vector<std::string> filters;
  if (!file_types) {
    return filters;
  }
  filters.reserve(file_types->extensions.size());
  for (size_t index = 0; index < file_types->extensions.size(); ++index) {
    std::string filter;
    if (index < file_types->extension_description_overrides.size() &&
        !file_types->extension_description_overrides[index].empty()) {
      filter =
          base::UTF16ToUTF8(file_types->extension_description_overrides[index]);
      filter.push_back('|');
    }
    bool first_extension = true;
    for (const base::FilePath::StringType& extension :
         file_types->extensions[index]) {
      if (!first_extension) {
        filter.push_back(',');
      }
      first_extension = false;
      filter.append(".");
      filter.append(extension);
    }
    if (!filter.empty() && filter.back() != '|') {
      filters.push_back(std::move(filter));
    }
  }
  return filters;
}

class SelectFileDialogOhos final : public SelectFileDialog {
 public:
  SelectFileDialogOhos(Listener* listener,
                       std::unique_ptr<SelectFilePolicy> policy)
      : SelectFileDialog(listener, std::move(policy)) {}

  void Complete(const std::vector<std::string>& selected_uris,
                int file_type_index,
                bool canceled) {
    request_id_ = 0;
    if (!listener_) {
      return;
    }
    if (canceled || selected_uris.empty()) {
      listener_->FileSelectionCanceled();
      listener_ = nullptr;
      return;
    }

    std::vector<SelectedFileInfo> files;
    files.reserve(selected_uris.size());
    for (const std::string& uri : selected_uris) {
      base::FilePath path = PickerUriToPath(uri);
      if (!path.empty()) {
        files.emplace_back(path);
      }
    }
    if (files.empty()) {
      listener_->FileSelectionCanceled();
    } else if (type_ == SELECT_OPEN_MULTI_FILE && files.size() > 1) {
      listener_->MultiFilesSelected(files);
    } else {
      listener_->FileSelected(files.front(), file_type_index);
    }
    listener_ = nullptr;
  }

  bool IsRunning(gfx::NativeWindow owning_window) const override {
    return request_id_ != 0;
  }

  void ListenerDestroyed() override { listener_ = nullptr; }

 private:
  ~SelectFileDialogOhos() override = default;

  void SelectFileImpl(Type type,
                      const std::u16string& title,
                      const base::FilePath& default_path,
                      const FileTypeInfo* file_types,
                      int file_type_index,
                      const base::FilePath::StringType& default_extension,
                      gfx::NativeWindow owning_window,
                      const GURL* caller) override {
    type_ = type;
    multiple_file_type_choices_ =
        file_types && file_types->extensions.size() > 1;

    OhosSelectFileDialogRequest request;
    request.type = type;
    request.title = base::UTF16ToUTF8(title);
    request.default_name = default_path.BaseName().value();
    request.suffix_filters = BuildSuffixFilters(file_types);
    request.file_type_index = file_type_index;

    OhosSelectFileDialogRequestCallback callback;
    {
      OhosFileDialogState& state = GetOhosFileDialogState();
      base::AutoLock lock(state.lock);
      request.request_id = state.next_request_id++;
      request_id_ = request.request_id;
      state.pending.emplace(request.request_id, this);
      callback = state.request_callback;
    }
    if (callback) {
      callback.Run(request);
      return;
    }
    CompleteOhosSelectFileDialog(request.request_id, {}, file_type_index,
                                 /*canceled=*/true);
  }

  bool HasMultipleFileTypeChoicesImpl() override {
    return multiple_file_type_choices_;
  }

  Type type_ = SELECT_NONE;
  int request_id_ = 0;
  bool multiple_file_type_choices_ = false;
};

}  // namespace

void SetOhosSelectFileDialogRequestCallback(
    OhosSelectFileDialogRequestCallback callback) {
  OhosFileDialogState& state = GetOhosFileDialogState();
  base::AutoLock lock(state.lock);
  state.request_callback = std::move(callback);
}

bool CompleteOhosSelectFileDialog(int request_id,
                                  const std::vector<std::string>& selected_uris,
                                  int file_type_index,
                                  bool canceled) {
  scoped_refptr<SelectFileDialogOhos> dialog;
  {
    OhosFileDialogState& state = GetOhosFileDialogState();
    base::AutoLock lock(state.lock);
    auto pending = state.pending.find(request_id);
    if (pending == state.pending.end()) {
      return false;
    }
    dialog = std::move(pending->second);
    state.pending.erase(pending);
  }
  dialog->Complete(selected_uris, file_type_index, canceled);
  return true;
}

void CancelAllOhosSelectFileDialogs() {
  std::map<int, scoped_refptr<SelectFileDialogOhos>> pending;
  {
    OhosFileDialogState& state = GetOhosFileDialogState();
    base::AutoLock lock(state.lock);
    pending.swap(state.pending);
  }
  for (auto& [request_id, dialog] : pending) {
    dialog->Complete({}, 0, /*canceled=*/true);
  }
}

SelectFileDialog* CreateSelectFileDialog(
    SelectFileDialog::Listener* listener,
    std::unique_ptr<SelectFilePolicy> policy) {
  return new SelectFileDialogOhos(listener, std::move(policy));
}

}  // namespace ui
