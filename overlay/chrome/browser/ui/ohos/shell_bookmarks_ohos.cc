// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The shell's bookmarks page: reads and edits the profile's BookmarkModel on
// behalf of ArkUI, and tells every shell window of the profile when the model
// changes so open pages can refetch.

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/containers/span.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "base/task/task_traits.h"
#include "chrome/browser/bookmarks/bookmark_html_writer.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/bookmarks/browser/bookmark_client.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_model_observer.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/common/bookmark_metrics.h"
#include "components/user_data_importer/common/imported_bookmark_entry.h"
#include "components/user_data_importer/content/content_bookmark_parser_utils.h"
#include "components/user_data_importer/utility/bookmark_parser.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

using bookmarks::BookmarkModel;
using bookmarks::BookmarkNode;

constexpr char kBookmarkListEvent[] = "bookmarkList";
constexpr char kBookmarkCreatedEvent[] = "bookmarkCreated";
constexpr char kBookmarksChangedEvent[] = "bookmarksChanged";
constexpr char kBookmarkOpResultEvent[] = "bookmarkOpResult";
constexpr char kBookmarkPathEvent[] = "bookmarkPath";
constexpr char kBookmarkExportDoneEvent[] = "bookmarkExportDone";
constexpr char kBookmarkImportDoneEvent[] = "bookmarkImportDone";

// Why an edit was refused. Without these a shell can only see that nothing
// happened, so it shows "saved" for a rejected URL and offers no reason when
// a folder cannot be dragged into itself.
constexpr char kErrNotFound[] = "notFound";
constexpr char kErrPermanentNode[] = "permanentNode";
constexpr char kErrInvalidParent[] = "invalidParent";
constexpr char kErrCycle[] = "cycle";
constexpr char kErrInvalidUrl[] = "invalidUrl";
constexpr char kErrUnknown[] = "unknown";

// Import and export, which fail in their own ways.
constexpr char kErrWriteFailed[] = "writeFailed";
constexpr char kErrFileNotFound[] = "fileNotFound";
constexpr char kErrNotBookmarkFile[] = "notBookmarkFile";
constexpr char kErrTooLarge[] = "tooLarge";

// A bookmarks file this big is not one the user meant to pick: Chrome's
// export of tens of thousands of bookmarks is a few megabytes.
constexpr int64_t kMaxImportBytes = 20 * 1024 * 1024;

// What the imported folder is called when the shell does not say. The shell
// normally passes a name carrying the date.
constexpr char kDefaultImportFolderTitle[] = "Imported bookmarks";

constexpr double kDefaultSearchCount = 50;
constexpr double kMinSearchCount = 1;
constexpr double kMaxSearchCount = 200;

// Every edit the shell makes is a direct user action.
constexpr bookmarks::metrics::BookmarkEditSource kEditSource =
    bookmarks::metrics::BookmarkEditSource::kUser;

// --- Chromium API access, one helper per call so a rename touches one place.

BookmarkModel* ModelFor(Profile* profile) {
  // The factory redirects an incognito profile to its original one, which is
  // what an incognito window's bookmarks page should show.
  return profile ? BookmarkModelFactory::GetForBrowserContext(profile)
                 : nullptr;
}

const BookmarkNode* NodeById(const BookmarkModel* model,
                             const std::string* id) {
  std::optional<int64_t> value = FromShellId(id);
  return value ? bookmarks::GetBookmarkNodeByID(model, *value) : nullptr;
}

bool IsManaged(BookmarkModel* model, const BookmarkNode* node) {
  return model->client() && model->client()->IsNodeManaged(node);
}

// Whether the shell may put nodes into `folder`. The invisible root only
// holds the permanent folders.
bool CanHoldChildren(BookmarkModel* model, const BookmarkNode* folder) {
  return folder && folder->is_folder() && !model->is_root_node(folder) &&
         !IsManaged(model, folder);
}

// Which of the three permanent folders this is, if any. Their titles are
// localized, so a shell cannot tell the bookmark bar from the mobile folder
// by name -- and it needs to, to pick where a new bookmark goes.
std::string_view RootTypeOf(BookmarkModel* model, const BookmarkNode* node) {
  if (node == model->mobile_node()) {
    return "mobile";
  }
  if (node == model->bookmark_bar_node()) {
    return "bookmarkBar";
  }
  if (node == model->other_node()) {
    return "other";
  }
  return {};
}

size_t IndexInParent(const BookmarkNode* node) {
  const BookmarkNode* parent = node->parent();
  return parent ? parent->GetIndexOf(node).value_or(0) : 0;
}

// --- Serialisation. -------------------------------------------------------

base::DictValue ToShellNode(BookmarkModel* model, const BookmarkNode* node) {
  const BookmarkNode* parent = node->parent();
  // The root is never shown, so the permanent folders read as top level.
  const bool top_level = !parent || model->is_root_node(parent);

  base::DictValue result;
  result.Set("id", ToShellId(node->id()));
  result.Set("parentId", top_level ? std::string() : ToShellId(parent->id()));
  result.Set("type", node->is_url() ? "url" : "folder");
  result.Set("title", std::u16string_view(node->GetTitle()));
  if (node->is_url()) {
    result.Set("url", node->url().spec());
  }
  result.Set("dateAdded", ToShellTime(node->date_added()));
  result.Set("index", static_cast<int>(IndexInParent(node)));
  if (node->is_folder()) {
    // Direct children only. A folder row shows "N items"; counting it here
    // saves the shell a round trip per folder on every list it draws.
    result.Set("childCount", static_cast<int>(node->children().size()));
    if (const std::string_view root = RootTypeOf(model, node); !root.empty()) {
      result.Set("rootType", root);
    }
  }
  return result;
}

// The titles from the permanent folder down to `node`'s parent. A search hit
// is shown flat, so this is what tells two same-named bookmarks apart.
base::ListValue AncestorTitles(BookmarkModel* model, const BookmarkNode* node) {
  std::vector<const BookmarkNode*> ancestors;
  for (const BookmarkNode* folder = node->parent();
       folder && !model->is_root_node(folder); folder = folder->parent()) {
    ancestors.push_back(folder);
  }
  base::ListValue path;
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
    path.Append(std::u16string_view((*it)->GetTitle()));
  }
  return path;
}

void ReplyNodeList(const ShellCommandContext& context,
                   const base::DictValue& command,
                   std::string_view parent_id,
                   base::ListValue nodes) {
  base::DictValue event;
  event.Set("event", kBookmarkListEvent);
  event.Set("requestId", ReadRequestId(command));
  event.Set("parentId", parent_id);
  event.Set("nodes", std::move(nodes));
  ReplyToShell(context, std::move(event));
}

// Answers a mutating command, but only one that asked to be answered: a
// shell built before this existed sends no requestId and would not know what
// to do with the event.
void ReplyOpResult(const ShellCommandContext& context,
                   const base::DictValue& command,
                   std::string_view error,
                   base::ListValue failed_ids = base::ListValue()) {
  const std::optional<int> request_id = command.FindInt("requestId");
  if (!request_id) {
    return;
  }
  base::DictValue event;
  event.Set("event", kBookmarkOpResultEvent);
  event.Set("requestId", *request_id);
  event.Set("ok", error.empty());
  if (!error.empty()) {
    event.Set("error", error);
  }
  if (!failed_ids.empty()) {
    event.Set("failedIds", std::move(failed_ids));
  }
  ReplyToShell(context, std::move(event));
}

// --- Input helpers. -------------------------------------------------------

// The "parentId" of an insertion; the mobile folder when absent, since that
// is where a phone's own bookmarks live. Null when it names nothing usable.
const BookmarkNode* ReadInsertionParent(BookmarkModel* model,
                                        const base::DictValue& command) {
  const std::string* id = command.FindString("parentId");
  if (!id || id->empty()) {
    return model->mobile_node();
  }
  const BookmarkNode* parent = NodeById(model, id);
  if (!CanHoldChildren(model, parent)) {
    LOG(WARNING) << "OHOS shell bookmarks: unusable parent " << *id;
    return nullptr;
  }
  return parent;
}

// Like ReadEditableNode, but says which way it failed so the caller can pass
// that on instead of dropping the command silently.
const BookmarkNode* ResolveEditableNode(BookmarkModel* model,
                                        const std::string* id,
                                        std::string_view* error) {
  const BookmarkNode* node = NodeById(model, id);
  if (!node) {
    *error = kErrNotFound;
    return nullptr;
  }
  if (model->is_permanent_node(node)) {
    *error = kErrPermanentNode;
    return nullptr;
  }
  if (IsManaged(model, node)) {
    // Set by policy, not by this user; nothing the shell can offer changes it.
    *error = kErrUnknown;
    return nullptr;
  }
  return node;
}

// Where an insertion lands. Absent still means the end, which is what a shell
// that cannot ask for a position expects; out of range clamps rather than
// fails, since a stale list is a normal thing for the shell to be holding.
size_t ReadInsertionIndex(const base::DictValue& command,
                          const BookmarkNode* parent) {
  const double last = static_cast<double>(parent->children().size());
  return static_cast<size_t>(
      std::clamp(command.FindDouble("index").value_or(last), 0.0, last));
}

std::optional<GURL> ReadUrl(const base::DictValue& command) {
  const std::string* spec = command.FindString("url");
  if (!spec) {
    LOG(WARNING) << "OHOS shell bookmarks: missing url";
    return std::nullopt;
  }
  GURL url(*spec);
  if (!url.is_valid()) {
    LOG(WARNING) << "OHOS shell bookmarks: invalid url";
    return std::nullopt;
  }
  return url;
}

std::u16string ReadTitle(const base::DictValue& command) {
  const std::string* title = command.FindString("title");
  return title ? base::UTF8ToUTF16(*title) : std::u16string();
}

size_t ReadSearchCount(const base::DictValue& command) {
  const double count =
      command.FindDouble("maxCount").value_or(kDefaultSearchCount);
  return static_cast<size_t>(
      std::clamp(count, kMinSearchCount, kMaxSearchCount));
}

// --- Commands. Each runs only once the model has loaded. -----------------

void GetBookmarks(const ShellCommandContext& context,
                  BookmarkModel* model,
                  const base::DictValue& command) {
  base::ListValue nodes;
  const std::string* parent_id = command.FindString("parentId");
  if (!parent_id || parent_id->empty()) {
    // Mobile first: on a phone that is where the user's own bookmarks live.
    const BookmarkNode* const top_level[] = {model->mobile_node(),
                                             model->bookmark_bar_node(),
                                             model->other_node()};
    for (const BookmarkNode* folder : top_level) {
      if (folder) {
        nodes.Append(ToShellNode(model, folder));
      }
    }
    ReplyNodeList(context, command, std::string_view(), std::move(nodes));
    return;
  }
  const BookmarkNode* parent = NodeById(model, parent_id);
  if (parent && parent->is_folder()) {
    for (const auto& child : parent->children()) {
      nodes.Append(ToShellNode(model, child.get()));
    }
  } else {
    // Answered anyway (empty) so the shell's request does not hang.
    LOG(WARNING) << "OHOS shell bookmarks: no folder " << *parent_id;
  }
  ReplyNodeList(context, command, *parent_id, std::move(nodes));
}

void SearchBookmarks(const ShellCommandContext& context,
                     BookmarkModel* model,
                     const base::DictValue& command) {
  base::ListValue nodes;
  const std::string* query = command.FindString("query");
  if (query && !query->empty()) {
    bookmarks::QueryFields fields;
    fields.word_phrase_query =
        std::make_unique<std::u16string>(base::UTF8ToUTF16(*query));
    for (const BookmarkNode* node : bookmarks::GetBookmarksMatchingProperties(
             model, fields, ReadSearchCount(command))) {
      base::DictValue entry = ToShellNode(model, node);
      // Results are shown flat, so each one carries where it lives.
      entry.Set("path", AncestorTitles(model, node));
      nodes.Append(std::move(entry));
    }
  }
  ReplyNodeList(context, command, std::string_view(), std::move(nodes));
}

void AddBookmark(const ShellCommandContext& context,
                 BookmarkModel* model,
                 const base::DictValue& command) {
  const BookmarkNode* parent = ReadInsertionParent(model, command);
  if (!parent) {
    ReplyOpResult(context, command, kErrInvalidParent);
    return;
  }
  std::optional<GURL> url = ReadUrl(command);
  if (!url) {
    ReplyOpResult(context, command, kErrInvalidUrl);
    return;
  }
  const BookmarkNode* node = model->AddNewURL(
      parent, ReadInsertionIndex(command, parent), ReadTitle(command), *url);
  if (!node) {
    ReplyOpResult(context, command, kErrUnknown);
    return;
  }
  // The shell needs the id to offer "edit" on the toast it shows next, and
  // finding the node again by URL picks the wrong one when the same page is
  // bookmarked twice in a folder.
  if (command.FindInt("requestId")) {
    base::DictValue event;
    event.Set("event", kBookmarkCreatedEvent);
    event.Set("requestId", ReadRequestId(command));
    event.Set("node", ToShellNode(model, node));
    ReplyToShell(context, std::move(event));
  }
}

// Every bookmark of a URL, newest first. The shell uses it to edit the one
// the current page is bookmarked as, and to say how many folders hold it
// before removing any.
void GetBookmarksForUrl(const ShellCommandContext& context,
                        BookmarkModel* model,
                        const base::DictValue& command) {
  base::ListValue nodes;
  if (std::optional<GURL> url = ReadUrl(command)) {
    std::vector<raw_ptr<const BookmarkNode, VectorExperimental>> matches =
        model->GetNodesByURL(*url);
    std::stable_sort(matches.begin(), matches.end(),
                     [](const BookmarkNode* a, const BookmarkNode* b) {
                       return a->date_added() > b->date_added();
                     });
    for (const BookmarkNode* node : matches) {
      nodes.Append(ToShellNode(model, node));
    }
  }
  ReplyNodeList(context, command, std::string_view(), std::move(nodes));
}

// The chain from the permanent folder down to `id` itself, both ends
// included. Reached from the page menu the shell has only a parent id, and
// walking up from the roots means reading the whole tree.
void GetBookmarkPath(const ShellCommandContext& context,
                     BookmarkModel* model,
                     const base::DictValue& command) {
  std::vector<const BookmarkNode*> chain;
  for (const BookmarkNode* node = NodeById(model, command.FindString("id"));
       node && !model->is_root_node(node); node = node->parent()) {
    chain.push_back(node);
  }
  base::ListValue nodes;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    nodes.Append(ToShellNode(model, *it));
  }
  base::DictValue event;
  event.Set("event", kBookmarkPathEvent);
  event.Set("requestId", ReadRequestId(command));
  event.Set("nodes", std::move(nodes));
  ReplyToShell(context, std::move(event));
}

void RemoveBookmarkByUrl(const ShellCommandContext& context,
                         BookmarkModel* model,
                         const base::DictValue& command) {
  std::optional<GURL> url = ReadUrl(command);
  if (!url) {
    ReplyOpResult(context, command, kErrInvalidUrl);
    return;
  }
  // Leaves policy-managed bookmarks of the URL alone.
  bookmarks::RemoveAllBookmarks(model, *url, FROM_HERE);
  ReplyOpResult(context, command, std::string_view());
}

void UpdateBookmark(const ShellCommandContext& context,
                    BookmarkModel* model,
                    const base::DictValue& command) {
  std::string_view error;
  const BookmarkNode* node =
      ResolveEditableNode(model, command.FindString("id"), &error);
  if (!node) {
    ReplyOpResult(context, command, error);
    return;
  }
  // A URL that will be refused must not leave the title changed behind it,
  // so check before writing either.
  std::optional<GURL> url;
  if (command.FindString("url")) {
    url = ReadUrl(command);
    if (!url || !node->is_url()) {
      ReplyOpResult(context, command, kErrInvalidUrl);
      return;
    }
  }
  // An empty title is allowed: Chromium permits it and the shell shows the
  // URL in its place.
  if (command.FindString("title")) {
    model->SetTitle(node, ReadTitle(command), kEditSource);
  }
  if (url) {
    model->SetURL(node, *url, kEditSource);
  }
  ReplyOpResult(context, command, std::string_view());
}

void MoveBookmark(const ShellCommandContext& context,
                  BookmarkModel* model,
                  const base::DictValue& command) {
  std::string_view error;
  const BookmarkNode* node =
      ResolveEditableNode(model, command.FindString("id"), &error);
  if (!node) {
    ReplyOpResult(context, command, error);
    return;
  }
  const BookmarkNode* parent =
      NodeById(model, command.FindString("parentId"));
  if (!CanHoldChildren(model, parent)) {
    ReplyOpResult(context, command, kErrInvalidParent);
    return;
  }
  // A folder moved into itself or its own subtree would detach the subtree.
  if (parent == node || bookmarks::IsDescendantOf(parent, node)) {
    ReplyOpResult(context, command, kErrCycle);
    return;
  }
  model->Move(node, parent, ReadInsertionIndex(command, parent));
  ReplyOpResult(context, command, std::string_view());
}

// The ids a batch command names, resolved in the order given. Ids that name
// nothing editable are collected rather than failing the batch: a selection
// drawn before someone else's edit will have stale entries in it, and the
// user meant the rest.
std::vector<const BookmarkNode*> ReadBatchNodes(BookmarkModel* model,
                                                const base::DictValue& command,
                                                base::ListValue* failed_ids,
                                                std::string_view* error) {
  std::vector<const BookmarkNode*> nodes;
  const base::ListValue* ids = command.FindList("ids");
  if (!ids) {
    *error = kErrUnknown;
    return nodes;
  }
  for (const base::Value& entry : *ids) {
    const std::string* id = entry.GetIfString();
    std::string_view reason;
    if (const BookmarkNode* node = ResolveEditableNode(model, id, &reason)) {
      nodes.push_back(node);
      continue;
    }
    if (error->empty()) {
      *error = reason;
    }
    failed_ids->Append(id ? *id : std::string());
  }
  return nodes;
}

// Drops a node that another node in the same batch already carries. Moving a
// folder takes its subtree with it, so moving a descendant separately would
// pull it back out.
void DropNodesCarriedByOthers(std::vector<const BookmarkNode*>* nodes) {
  std::erase_if(*nodes, [&nodes](const BookmarkNode* node) {
    for (const BookmarkNode* other : *nodes) {
      if (other != node && bookmarks::IsDescendantOf(node, other)) {
        return true;
      }
    }
    return false;
  });
}

void MoveBookmarks(const ShellCommandContext& context,
                   BookmarkModel* model,
                   const base::DictValue& command) {
  const BookmarkNode* parent =
      NodeById(model, command.FindString("parentId"));
  if (!CanHoldChildren(model, parent)) {
    ReplyOpResult(context, command, kErrInvalidParent);
    return;
  }
  base::ListValue failed_ids;
  std::string_view error;
  std::vector<const BookmarkNode*> nodes =
      ReadBatchNodes(model, command, &failed_ids, &error);
  DropNodesCarriedByOthers(&nodes);
  std::erase_if(nodes, [&](const BookmarkNode* node) {
    if (parent != node && !bookmarks::IsDescendantOf(parent, node)) {
      return false;
    }
    if (error.empty()) {
      error = kErrCycle;
    }
    failed_ids.Append(ToShellId(node->id()));
    return true;
  });

  {
    // One bookmarksChanged for the batch. Twenty of them redraw the shell's
    // list twenty times, which is what the user sees as the flicker.
    model->BeginExtensiveChanges();
    size_t index = ReadInsertionIndex(command, parent);
    for (const BookmarkNode* node : nodes) {
      index = std::min(index, parent->children().size());
      model->Move(node, parent, index);
      // Read the landing position back rather than assuming index + 1: within
      // one parent, Move renumbers around the node it just removed.
      index = parent->GetIndexOf(node).value_or(index) + 1;
    }
    model->EndExtensiveChanges();
  }
  ReplyOpResult(context, command, error, std::move(failed_ids));
}

void RemoveBookmarks(const ShellCommandContext& context,
                     BookmarkModel* model,
                     const base::DictValue& command) {
  base::ListValue failed_ids;
  std::string_view error;
  std::vector<const BookmarkNode*> nodes =
      ReadBatchNodes(model, command, &failed_ids, &error);
  DropNodesCarriedByOthers(&nodes);
  {
    model->BeginExtensiveChanges();
    for (const BookmarkNode* node : nodes) {
      model->Remove(node, kEditSource, FROM_HERE);
    }
    model->EndExtensiveChanges();
  }
  ReplyOpResult(context, command, error, std::move(failed_ids));
}

void RemoveBookmark(const ShellCommandContext& context,
                    BookmarkModel* model,
                    const base::DictValue& command) {
  std::string_view error;
  const BookmarkNode* node =
      ResolveEditableNode(model, command.FindString("id"), &error);
  if (!node) {
    ReplyOpResult(context, command, error);
    return;
  }
  model->Remove(node, kEditSource, FROM_HERE);
  ReplyOpResult(context, command, std::string_view());
}

void CreateBookmarkFolder(const ShellCommandContext& context,
                          BookmarkModel* model,
                          const base::DictValue& command) {
  const BookmarkNode* parent = ReadInsertionParent(model, command);
  if (!parent) {
    ReplyOpResult(context, command, kErrInvalidParent);
    return;
  }
  const BookmarkNode* folder = model->AddFolder(
      parent, ReadInsertionIndex(command, parent), ReadTitle(command));
  if (!folder) {
    ReplyOpResult(context, command, kErrUnknown);
    return;
  }
  base::DictValue event;
  event.Set("event", kBookmarkCreatedEvent);
  event.Set("requestId", ReadRequestId(command));
  event.Set("node", ToShellNode(model, folder));
  ReplyToShell(context, std::move(event));
}

// --- Import and export. ---------------------------------------------------
//
// Both work on a plain path inside the app sandbox. The shell owns the system
// file picker and the copy in or out; this end never sees a document URI and
// needs no storage permission, because the engine and the shell share one
// process.

// Everything under the three permanent folders, which is what the writer
// writes and what the shell wants counted.
void CountTree(const BookmarkNode* node, int* urls, int* folders) {
  for (const auto& child : node->children()) {
    if (child->is_url()) {
      ++*urls;
    } else if (child->is_folder()) {
      ++*folders;
      CountTree(child.get(), urls, folders);
    }
  }
}

void ReplyExportDone(const ShellCommandContext& context,
                     int request_id,
                     std::string_view error,
                     int bookmark_count,
                     int folder_count) {
  base::DictValue event;
  event.Set("event", kBookmarkExportDoneEvent);
  event.Set("requestId", request_id);
  event.Set("ok", error.empty());
  event.Set("bookmarkCount", bookmark_count);
  event.Set("folderCount", folder_count);
  if (!error.empty()) {
    event.Set("error", error);
  }
  ReplyToShell(context, std::move(event));
}

void OnExportFinished(ShellCommandContext context,
                      int request_id,
                      int bookmark_count,
                      int folder_count,
                      bookmark_html_writer::Result result) {
  const bool ok = result == bookmark_html_writer::Result::kSuccess;
  ReplyExportDone(context, request_id, ok ? std::string_view() : kErrWriteFailed,
                  ok ? bookmark_count : 0, ok ? folder_count : 0);
}

void ExportBookmarks(const ShellCommandContext& context,
                     BookmarkModel* model,
                     const base::DictValue& command) {
  const int request_id = ReadRequestId(command);
  const std::string* path = command.FindString("path");
  if (!path || path->empty()) {
    ReplyExportDone(context, request_id, kErrWriteFailed, 0, 0);
    return;
  }
  // Counted before writing: the writer reports only whether it succeeded, and
  // the tree cannot change underneath it because both run on this sequence
  // until the write is handed to the thread pool.
  int urls = 0;
  int folders = 0;
  for (const BookmarkNode* root : {model->bookmark_bar_node(),
                                   model->other_node(), model->mobile_node()}) {
    if (root) {
      CountTree(root, &urls, &folders);
    }
  }
  // The writer takes a Profile and resolves the model from it; an incognito
  // one resolves to its original, the same way every other command here does.
  Profile* profile = context.profile;
  if (!profile) {
    ReplyExportDone(context, request_id, kErrUnknown, 0, 0);
    return;
  }
  bookmark_html_writer::WriteBookmarks(
      profile, base::FilePath(*path),
      base::BindPostTask(
          base::SequencedTaskRunner::GetCurrentDefault(),
          base::BindOnce(&OnExportFinished, context, request_id, urls,
                         folders)));
}

void ReplyImportDone(const ShellCommandContext& context,
                     int request_id,
                     std::string_view error,
                     std::string_view folder_id,
                     int bookmark_count,
                     int folder_count,
                     int skipped_count) {
  base::DictValue event;
  event.Set("event", kBookmarkImportDoneEvent);
  event.Set("requestId", request_id);
  event.Set("ok", error.empty());
  event.Set("folderId", folder_id);
  event.Set("bookmarkCount", bookmark_count);
  event.Set("folderCount", folder_count);
  event.Set("skippedCount", skipped_count);
  if (!error.empty()) {
    event.Set("error", error);
  }
  ReplyToShell(context, std::move(event));
}

struct ParsedImport {
  std::string error;
  std::vector<user_data_importer::ImportedBookmarkEntry> entries;
};

// Reads and parses off the UI thread. The parser is Chromium's own; upstream
// runs it in a sandboxed utility process, which this platform does not give a
// phone, so it runs here instead (see ohos-bookmark-html-parsing.patch).
ParsedImport ReadAndParse(base::FilePath path) {
  ParsedImport result;
  const std::optional<int64_t> size = base::GetFileSize(path);
  if (!size) {
    result.error = kErrFileNotFound;
    return result;
  }
  if (*size > kMaxImportBytes) {
    result.error = kErrTooLarge;
    return result;
  }
  std::string html;
  if (!base::ReadFileToString(path, &html) || html.empty()) {
    result.error = kErrFileNotFound;
    return result;
  }
  user_data_importer::BookmarkParser::ParsedBookmarks parsed =
      user_data_importer::ParseBookmarksUnsafe(html);
  if (parsed.bookmarks.empty()) {
    // Search engines and reading-list entries alone do not make this a
    // bookmarks file as far as the user is concerned.
    result.error = kErrNotBookmarkFile;
    return result;
  }
  result.entries = std::move(parsed.bookmarks);
  return result;
}

// The folder an entry belongs in, creating the chain under `root` as needed.
// `path` is the entry's own folder chain as the file recorded it, including
// the file's "Bookmarks bar" folder, which stays a folder here rather than
// merging into ours.
const BookmarkNode* FolderForPath(BookmarkModel* model,
                                  const BookmarkNode* root,
                                  const std::vector<std::u16string>& path) {
  const BookmarkNode* parent = root;
  for (const std::u16string& name : path) {
    const BookmarkNode* next = nullptr;
    for (const auto& child : parent->children()) {
      if (child->is_folder() && child->GetTitle() == name) {
        next = child.get();
        break;
      }
    }
    parent = next ? next
                  : model->AddFolder(parent, parent->children().size(), name);
    if (!parent) {
      return nullptr;
    }
  }
  return parent;
}

void OnImportParsed(ShellCommandContext context,
                    int request_id,
                    std::string parent_id,
                    std::u16string folder_title,
                    ParsedImport parsed) {
  BookmarkModel* model = ModelFor(context.profile);
  if (!model || !model->loaded()) {
    ReplyImportDone(context, request_id, kErrUnknown, std::string_view(), 0, 0,
                    0);
    return;
  }
  if (!parsed.error.empty()) {
    ReplyImportDone(context, request_id, parsed.error, std::string_view(), 0, 0,
                    0);
    return;
  }
  const BookmarkNode* parent = model->mobile_node();
  if (!parent_id.empty()) {
    parent = NodeById(model, &parent_id);
    if (!CanHoldChildren(model, parent)) {
      ReplyImportDone(context, request_id, kErrInvalidParent,
                      std::string_view(), 0, 0, 0);
      return;
    }
  }

  int skipped = 0;
  std::string folder_id;
  {
    // Everything under one batch: five thousand bookmarks must not redraw the
    // shell five thousand times.
    model->BeginExtensiveChanges();
    // Imported into a folder of its own rather than merged, so the user can
    // undo the whole thing by deleting one folder -- the same bargain Chrome
    // offers. At the top, where the shell puts everything new.
    const BookmarkNode* root = model->AddFolder(parent, 0, folder_title);
    if (root) {
      folder_id = ToShellId(root->id());
      for (const user_data_importer::ImportedBookmarkEntry& entry :
           parsed.entries) {
        const BookmarkNode* holder = FolderForPath(model, root, entry.path);
        if (!holder) {
          ++skipped;
          continue;
        }
        if (entry.is_folder) {
          if (!FolderForPath(model, holder, {entry.title})) {
            ++skipped;
          }
          continue;
        }
        // Firefox writes place: URLs for its smart folders, and any file
        // can carry a malformed href.
        if (!entry.url.is_valid()) {
          ++skipped;
          continue;
        }
        const base::Time added = entry.creation_time.is_null()
                                     ? base::Time::Now()
                                     : entry.creation_time;
        if (!model->AddURL(holder, holder->children().size(), entry.title,
                           entry.url, nullptr, added)) {
          ++skipped;
        }
      }
    }
    model->EndExtensiveChanges();
    if (!root) {
      ReplyImportDone(context, request_id, kErrUnknown, std::string_view(), 0,
                      0, 0);
      return;
    }
  }
  // Folders the file's own hierarchy implied are counted too, so the number
  // matches what the user sees.
  int created_urls = 0;
  int created_folders = 0;
  if (const BookmarkNode* root = NodeById(model, &folder_id)) {
    CountTree(root, &created_urls, &created_folders);
  }
  ReplyImportDone(context, request_id, std::string_view(), folder_id,
                  created_urls, created_folders, skipped);
}

void ImportBookmarks(const ShellCommandContext& context,
                     BookmarkModel* model,
                     const base::DictValue& command) {
  const int request_id = ReadRequestId(command);
  const std::string* path = command.FindString("path");
  if (!path || path->empty()) {
    ReplyImportDone(context, request_id, kErrFileNotFound, std::string_view(),
                    0, 0, 0);
    return;
  }
  const std::string* parent_id = command.FindString("parentId");
  const std::string* title = command.FindString("title");
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadAndParse, base::FilePath(*path)),
      base::BindOnce(&OnImportParsed, context, request_id,
                     parent_id ? *parent_id : std::string(),
                     title && !title->empty()
                         ? base::UTF8ToUTF16(*title)
                         : base::UTF8ToUTF16(std::string_view(
                               kDefaultImportFolderTitle))));
}

using CommandRunner = void (*)(const ShellCommandContext&,
                               BookmarkModel*,
                               const base::DictValue&);

struct BookmarksCommand {
  std::string_view name;
  CommandRunner run;
};

constexpr BookmarksCommand kCommands[] = {
    {"getBookmarks", &GetBookmarks},
    {"searchBookmarks", &SearchBookmarks},
    {"addBookmark", &AddBookmark},
    {"removeBookmarkByUrl", &RemoveBookmarkByUrl},
    {"updateBookmark", &UpdateBookmark},
    {"moveBookmark", &MoveBookmark},
    {"removeBookmark", &RemoveBookmark},
    {"createBookmarkFolder", &CreateBookmarkFolder},
    {"getBookmarksForUrl", &GetBookmarksForUrl},
    {"getBookmarkPath", &GetBookmarkPath},
    {"moveBookmarks", &MoveBookmarks},
    {"removeBookmarks", &RemoveBookmarks},
    {"exportBookmarks", &ExportBookmarks},
    {"importBookmarks", &ImportBookmarks},
};

base::span<const std::string_view> CommandNames() {
  static const base::NoDestructor<std::vector<std::string_view>> names([] {
    std::vector<std::string_view> result;
    for (const BookmarksCommand& command : kCommands) {
      result.push_back(command.name);
    }
    return result;
  }());
  return *names;
}

CommandRunner FindCommand(std::string_view name) {
  for (const BookmarksCommand& command : kCommands) {
    if (command.name == name) {
      return command.run;
    }
  }
  return nullptr;
}

// --- The per-profile watcher. ---------------------------------------------

// Tells the profile's shell windows when bookmarks change, and holds commands
// that arrived before the model finished loading: answering those from an
// empty model would show the user no bookmarks at all.
class BookmarksWatcher : public bookmarks::BookmarkModelObserver {
 public:
  // Keyed on the profile that owns the model. An incognito window shares its
  // original profile's bookmarks, so both must hear the same changes.
  explicit BookmarksWatcher(Profile* profile)
      : profile_(profile ? profile->GetOriginalProfile() : nullptr),
        model_(ModelFor(profile)) {
    if (model_) {
      observation_.Observe(model_.get());
    }
  }
  BookmarksWatcher(const BookmarksWatcher&) = delete;
  BookmarksWatcher& operator=(const BookmarksWatcher&) = delete;
  ~BookmarksWatcher() override = default;

  void Defer(const ShellCommandContext& context,
             std::string_view name,
             const base::DictValue& command) {
    pending_.push_back({context, std::string(name), command.Clone()});
  }

  // bookmarks::BookmarkModelObserver:
  void BookmarkModelLoaded(bool ids_reassigned) override {
    // Replayed from a fresh task: edits made inside this notification would
    // reach observers that have not yet heard the model loaded.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&BookmarksWatcher::RunPending,
                                  weak_factory_.GetWeakPtr()));
    NotifyChanged();
  }
  void BookmarkModelBeingDeleted() override {
    observation_.Reset();
    model_ = nullptr;
    pending_.clear();
  }
  void BookmarkNodeMoved(const BookmarkNode* old_parent,
                         size_t old_index,
                         const BookmarkNode* new_parent,
                         size_t new_index) override {
    NotifyChanged();
  }
  void BookmarkNodeAdded(const BookmarkNode* parent,
                         size_t index,
                         bool added_by_user) override {
    NotifyChanged();
  }
  void BookmarkNodeRemoved(const BookmarkNode* parent,
                           size_t old_index,
                           const BookmarkNode* node,
                           const std::set<GURL>& no_longer_bookmarked,
                           const base::Location& location) override {
    NotifyChanged();
  }
  void BookmarkNodeChanged(const BookmarkNode* node) override {
    NotifyChanged();
  }
  // The shell fetches favicons itself; a bookmark's cached icon loading is
  // not a change to anything it shows.
  void BookmarkNodeFaviconChanged(const BookmarkNode* node) override {}
  void BookmarkNodeChildrenReordered(const BookmarkNode* node) override {
    NotifyChanged();
  }
  void ExtensiveBookmarkChangesEnded() override { NotifyChanged(); }
  void BookmarkAllUserNodesRemoved(const std::set<GURL>& removed_urls,
                                   const base::Location& location) override {
    NotifyChanged();
  }

 private:
  struct PendingCommand {
    ShellCommandContext context;
    std::string name;
    base::DictValue command;
  };

  void NotifyChanged() {
    // One event for a whole import or sync batch, sent when it ends, instead
    // of one refetch per node.
    if (!model_ || model_->IsDoingExtensiveChanges()) {
      return;
    }
    base::DictValue event;
    event.Set("event", kBookmarksChangedEvent);
    event.Set("revision", ++revision_);
    // Incognito windows show these same bookmarks, and the dispatcher matches
    // a window's profile by pointer, so the off-the-record ones are named
    // separately or they never hear that anything changed.
    for (Profile* otr : profile_->GetAllOffTheRecordProfiles()) {
      BroadcastToShell(otr, event.Clone());
    }
    BroadcastToShell(profile_, std::move(event));
  }

  void RunPending() {
    std::vector<PendingCommand> pending = std::move(pending_);
    pending_.clear();
    if (!model_ || !model_->loaded()) {
      return;
    }
    for (const PendingCommand& entry : pending) {
      if (CommandRunner run = FindCommand(entry.name)) {
        run(entry.context, model_.get(), entry.command);
      }
    }
  }

  const raw_ptr<Profile> profile_;
  raw_ptr<BookmarkModel> model_;
  int revision_ = 0;
  std::vector<PendingCommand> pending_;
  base::ScopedObservation<BookmarkModel, bookmarks::BookmarkModelObserver>
      observation_{this};
  base::WeakPtrFactory<BookmarksWatcher> weak_factory_{this};
};

std::unique_ptr<BookmarksWatcher> CreateWatcher(Profile* profile) {
  return std::make_unique<BookmarksWatcher>(profile);
}

PerProfile<BookmarksWatcher>& Watchers() {
  static base::NoDestructor<PerProfile<BookmarksWatcher>> watchers(
      base::BindRepeating(&CreateWatcher));
  return *watchers;
}

}  // namespace

base::span<const std::string_view> BookmarksCommandNames() {
  return CommandNames();
}

bool HandleBookmarksCommand(const ShellCommandContext& context,
                            std::string_view name,
                            const base::DictValue& command) {
  CommandRunner run = FindCommand(name);
  if (!run) {
    return false;
  }
  BookmarkModel* model = ModelFor(context.profile);
  if (!model) {
    LOG(WARNING) << "OHOS shell bookmarks: profile has no bookmark model";
    return true;
  }
  if (!model->loaded()) {
    Watchers().Get(context.profile->GetOriginalProfile())
        ->Defer(context, name, command);
    return true;
  }
  run(context, model, command);
  return true;
}

void EnsureBookmarksObserver(Profile* profile) {
  if (ModelFor(profile)) {
    Watchers().Get(profile->GetOriginalProfile());
  }
}

bool IsUrlBookmarked(Profile* profile, const GURL& url) {
  BookmarkModel* model = ModelFor(profile);
  return model && model->loaded() && url.is_valid() &&
         model->IsBookmarked(url);
}

}  // namespace chrome::ohos
