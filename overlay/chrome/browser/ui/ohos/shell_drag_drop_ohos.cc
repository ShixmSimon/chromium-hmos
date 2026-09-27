// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ohos/shell_drag_drop_ohos.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/common/chrome_paths.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/clipboard/file_info.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/base/dragdrop/os_exchange_data_provider_factory.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

// Where the shell puts files it copied out of the sending app.
base::FilePath DroppedFileDirectory() {
  base::FilePath dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &dir)) {
    return base::FilePath();
  }
  return dir.AppendASCII("dropped");
}

// Only files the shell put in that directory are accepted.
//
// A path arriving over this channel becomes a path the renderer is granted
// read access to, so it decides what a page can upload. The shell is trusted
// -- it is the same app -- but a bug there should not turn into "any file
// this process can open", and the check costs a string compare.
bool IsDroppedFile(const base::FilePath& path) {
  const base::FilePath dir = DroppedFileDirectory();
  return !dir.empty() && !path.ReferencesParent() && dir.IsParent(path);
}

ui::OhosDragStage StageFromName(const std::string& name) {
  if (name == "enter") {
    return ui::OhosDragStage::kEnter;
  }
  if (name == "leave") {
    return ui::OhosDragStage::kLeave;
  }
  if (name == "drop") {
    return ui::OhosDragStage::kDrop;
  }
  return ui::OhosDragStage::kMove;
}

const char* OperationName(int operation) {
  if (operation & ui::DragDropTypes::DRAG_COPY) {
    return "copy";
  }
  if (operation & ui::DragDropTypes::DRAG_MOVE) {
    return "move";
  }
  if (operation & ui::DragDropTypes::DRAG_LINK) {
    return "link";
  }
  return "none";
}

// The dragged data, in the shape Chromium's drop target reads.
//
// Nothing here is the page's to see until it is dropped: Blink puts the
// DataTransfer in protected mode while a drag is in flight, so a file's name
// and bytes stay hidden and only the list of types shows. That is why the
// summary sent on enter can carry placeholder names without leaking them.
std::unique_ptr<ui::OSExchangeData> BuildExchangeData(
    const base::DictValue& event) {
  auto data = std::make_unique<ui::OSExchangeData>(
      ui::OSExchangeDataProviderFactory::CreateProvider());
  bool carries_anything = false;

  if (const std::string* text = event.FindString("text");
      text && !text->empty()) {
    data->SetString(base::UTF8ToUTF16(*text));
    carries_anything = true;
  }
  if (const std::string* html = event.FindString("html");
      html && !html->empty()) {
    data->SetHtml(base::UTF8ToUTF16(*html), GURL());
    carries_anything = true;
  }
  if (const base::ListValue* urls = event.FindList("urls")) {
    for (const base::Value& value : *urls) {
      if (!value.is_string()) {
        continue;
      }
      const GURL url(value.GetString());
      if (url.is_valid()) {
        // The first one wins for SetURL; a page reading text/uri-list wants
        // the one the user actually dragged, which is the first.
        data->SetURL(url, std::u16string());
        carries_anything = true;
        break;
      }
    }
  }

  std::vector<ui::FileInfo> files;
  if (const base::ListValue* file_values = event.FindList("files")) {
    for (const base::Value& value : *file_values) {
      const base::DictValue* file = value.GetIfDict();
      if (!file) {
        continue;
      }
      const std::string* path_string = file->FindString("path");
      if (!path_string || path_string->empty()) {
        continue;
      }
      const base::FilePath path =
          base::FilePath::FromUTF8Unsafe(*path_string);
      if (!IsDroppedFile(path)) {
        LOG(WARNING) << "OHOS drag: refusing a file outside the dropped "
                        "directory: " << path;
        continue;
      }
      const std::string* name = file->FindString("name");
      files.emplace_back(path, base::FilePath::FromUTF8Unsafe(
                                   name ? *name : path.BaseName().value()));
    }
  }
  if (!files.empty()) {
    data->SetFilenames(files);
    carries_anything = true;
  }

  return carries_anything ? std::move(data) : nullptr;
}

}  // namespace

std::string HandleShellDragEvent(gfx::AcceleratedWidget widget,
                                 const base::DictValue& event) {
  const std::string* stage_name = event.FindString("stage");
  ui::OhosDragEvent drag;
  drag.stage = StageFromName(stage_name ? *stage_name : std::string());
  drag.location = gfx::PointF(
      static_cast<float>(event.FindDouble("x").value_or(0.0)),
      static_cast<float>(event.FindDouble("y").value_or(0.0)));
  // What the source allows. Copy unless the shell says otherwise: HarmonyOS
  // drags between apps are copies, and a page that wants to refuse says so
  // itself through the operation that comes back.
  drag.operations = ui::DragDropTypes::DRAG_COPY;

  if (drag.stage == ui::OhosDragStage::kEnter ||
      drag.stage == ui::OhosDragStage::kDrop) {
    drag.data = BuildExchangeData(event);
  }

  const int accepted = ui::DispatchOhosDragEvent(widget, std::move(drag));
  return OperationName(accepted);
}

namespace {

int DragOperationFromName(const std::string* name) {
  if (!name) {
    return ui::DragDropTypes::DRAG_NONE;
  }
  if (*name == "copy") {
    return ui::DragDropTypes::DRAG_COPY;
  }
  if (*name == "move") {
    return ui::DragDropTypes::DRAG_MOVE;
  }
  if (*name == "link") {
    return ui::DragDropTypes::DRAG_LINK;
  }
  return ui::DragDropTypes::DRAG_NONE;
}

}  // namespace

void FinishPageDragOut(gfx::AcceleratedWidget widget,
                       const std::string* operation) {
  ui::OhosDragEvent finished;
  finished.stage = ui::OhosDragStage::kSourceFinished;
  finished.operations = DragOperationFromName(operation);
  ui::DispatchOhosDragEvent(widget, std::move(finished));
}

// Writes a dragged image where the app side can hand it to the system.
//
// Next to the files dragged in, under the same rule: emptied at startup, and
// the only directory the engine will take a file path from. One directory per
// drag so two drags of the same picture do not collide.
//
// Written on this thread on purpose. StartDrag is about to block waiting for
// the drag to finish, so the file has to exist before it returns -- and a
// page's image is a few hundred kilobytes, not a download.
std::string WriteDraggedImage(const std::string& name,
                              base::span<const uint8_t> contents) {
  const base::FilePath dir = DroppedFileDirectory();
  if (dir.empty() || contents.empty()) {
    return std::string();
  }
  static int sequence = 0;
  const base::FilePath target =
      dir.AppendASCII(base::StringPrintf("out-%d", ++sequence))
          .Append(base::FilePath::FromUTF8Unsafe(name).BaseName());
  if (!base::CreateDirectory(target.DirName()) ||
      !base::WriteFile(target, contents)) {
    LOG(WARNING) << "OHOS drag: could not write the dragged image to "
                 << target;
    return std::string();
  }
  return target.AsUTF8Unsafe();
}

void WatchPageDragsOut() {
  ui::SetOhosDragOutCallback(
      base::BindRepeating([](ui::OhosDragOutRequest request) -> bool {
        base::DictValue event;
        event.Set("event", "pageDragStarted");
        // Empty unless an image is being dragged.
        event.Set("filePath", WriteDraggedImage(request.file_name,
                                                request.file_contents));
        event.Set("text", request.text);
        event.Set("url", request.url);
        event.Set("html", request.html);
        event.Set("operation", OperationName(request.operations));
        DispatchAuraShellRuntimeEventToWidget(request.widget,
                                              std::move(event));
        // Always true: the event went to the window's shell, and if nothing
        // is listening the drag simply ends with nothing taken -- reported
        // through pageDragFinished like any other ending. Returning false
        // here would leave Chromium believing the drag never started while
        // the page had already been told it had.
        return true;
      }));
}

void ClearDroppedFileDirectory() {
  const base::FilePath dir = DroppedFileDirectory();
  if (dir.empty()) {
    return;
  }
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::GetDeletePathRecursivelyCallback(dir));
}

}  // namespace chrome::ohos
