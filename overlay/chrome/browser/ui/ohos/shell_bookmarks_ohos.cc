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

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/bookmarks/browser/bookmark_client.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_model_observer.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/common/bookmark_metrics.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

using bookmarks::BookmarkModel;
using bookmarks::BookmarkNode;

constexpr char kBookmarkListEvent[] = "bookmarkList";
constexpr char kBookmarkCreatedEvent[] = "bookmarkCreated";
constexpr char kBookmarksChangedEvent[] = "bookmarksChanged";

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

// Permanent folders and policy-managed bookmarks are not the user's to edit.
bool IsEditable(BookmarkModel* model, const BookmarkNode* node) {
  return node && !model->is_permanent_node(node) && !IsManaged(model, node);
}

// Whether the shell may put nodes into `folder`. The invisible root only
// holds the permanent folders.
bool CanHoldChildren(BookmarkModel* model, const BookmarkNode* folder) {
  return folder && folder->is_folder() && !model->is_root_node(folder) &&
         !IsManaged(model, folder);
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
  return result;
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

// The node a command wants to change, or null (logged) when it names nothing
// editable -- a stale id from a page drawn before another edit, typically.
const BookmarkNode* ReadEditableNode(BookmarkModel* model,
                                     const base::DictValue& command) {
  const std::string* id = command.FindString("id");
  const BookmarkNode* node = NodeById(model, id);
  if (!IsEditable(model, node)) {
    LOG(WARNING) << "OHOS shell bookmarks: no editable node "
                 << (id ? *id : std::string("<missing>"));
    return nullptr;
  }
  return node;
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
      nodes.Append(ToShellNode(model, node));
    }
  }
  ReplyNodeList(context, command, std::string_view(), std::move(nodes));
}

void AddBookmark(const ShellCommandContext& context,
                 BookmarkModel* model,
                 const base::DictValue& command) {
  std::optional<GURL> url = ReadUrl(command);
  const BookmarkNode* parent = ReadInsertionParent(model, command);
  if (!url || !parent) {
    return;
  }
  model->AddNewURL(parent, parent->children().size(), ReadTitle(command),
                   *url);
}

void RemoveBookmarkByUrl(const ShellCommandContext& context,
                         BookmarkModel* model,
                         const base::DictValue& command) {
  std::optional<GURL> url = ReadUrl(command);
  if (url) {
    // Leaves policy-managed bookmarks of the URL alone.
    bookmarks::RemoveAllBookmarks(model, *url, FROM_HERE);
  }
}

void UpdateBookmark(const ShellCommandContext& context,
                    BookmarkModel* model,
                    const base::DictValue& command) {
  const BookmarkNode* node = ReadEditableNode(model, command);
  if (!node) {
    return;
  }
  if (command.FindString("title")) {
    model->SetTitle(node, ReadTitle(command), kEditSource);
  }
  if (!command.FindString("url")) {
    return;
  }
  std::optional<GURL> url = ReadUrl(command);
  if (!node->is_url()) {
    LOG(WARNING) << "OHOS shell bookmarks: a folder has no url to set";
  } else if (url) {
    model->SetURL(node, *url, kEditSource);
  }
}

void MoveBookmark(const ShellCommandContext& context,
                  BookmarkModel* model,
                  const base::DictValue& command) {
  const BookmarkNode* node = ReadEditableNode(model, command);
  const BookmarkNode* parent =
      NodeById(model, command.FindString("parentId"));
  if (!node || !CanHoldChildren(model, parent)) {
    LOG_IF(WARNING, node) << "OHOS shell bookmarks: unusable move target";
    return;
  }
  // A folder moved into itself or its own subtree would detach the subtree.
  if (parent == node || bookmarks::IsDescendantOf(parent, node)) {
    LOG(WARNING) << "OHOS shell bookmarks: cannot move a folder into itself";
    return;
  }
  const double last = static_cast<double>(parent->children().size());
  const double index =
      std::clamp(command.FindDouble("index").value_or(last), 0.0, last);
  model->Move(node, parent, static_cast<size_t>(index));
}

void RemoveBookmark(const ShellCommandContext& context,
                    BookmarkModel* model,
                    const base::DictValue& command) {
  const BookmarkNode* node = ReadEditableNode(model, command);
  if (node) {
    model->Remove(node, kEditSource, FROM_HERE);
  }
}

void CreateBookmarkFolder(const ShellCommandContext& context,
                          BookmarkModel* model,
                          const base::DictValue& command) {
  base::DictValue event;
  event.Set("event", kBookmarkCreatedEvent);
  event.Set("requestId", ReadRequestId(command));
  const BookmarkNode* parent = ReadInsertionParent(model, command);
  // Without a usable parent the answer carries no "node", which the shell
  // reads as failure rather than waiting forever.
  if (parent) {
    const BookmarkNode* folder = model->AddFolder(
        parent, parent->children().size(), ReadTitle(command));
    if (folder) {
      event.Set("node", ToShellNode(model, folder));
    }
  }
  ReplyToShell(context, std::move(event));
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
};

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
  explicit BookmarksWatcher(Profile* profile)
      : profile_(profile), model_(ModelFor(profile)) {
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
    Watchers().Get(context.profile)->Defer(context, name, command);
    return true;
  }
  run(context, model, command);
  return true;
}

void EnsureBookmarksObserver(Profile* profile) {
  if (ModelFor(profile)) {
    Watchers().Get(profile);
  }
}

bool IsUrlBookmarked(Profile* profile, const GURL& url) {
  BookmarkModel* model = ModelFor(profile);
  return model && model->loaded() && url.is_valid() &&
         model->IsBookmarked(url);
}

}  // namespace chrome::ohos
