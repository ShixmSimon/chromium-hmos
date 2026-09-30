// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The passwords area of the shell services. A phone draws its own password
// pages -- chrome://password-manager is a desktop page -- and asks the engine
// for what they show and do:
//
//   getSavedPasswords {requestId}
//     -> "savedPasswords" {requestId, items: [{id, origin, signonRealm,
//          username, hasNote, dateCreated, dateLastUsed,
//          isAndroidCredential}]}
//   passwordAuthGranted {validMs}
//   revealPassword {requestId, id}
//     -> "passwordRevealed" {requestId, id, password, note}
//   updatePassword {requestId, id, username?, password?, note?}
//     -> "passwordCommandResult" {requestId, command, ok, reason?, newId?}
//   deletePassword {requestId, id}
//     -> "passwordCommandResult" {requestId, command, ok, reason?}
//   exportPasswords {requestId, path}
//     -> "passwordsExported" {requestId, ok, count, reason?}
//   importPasswords {requestId, path, overwrite?}
//     -> "passwordsImported" {requestId, ok, imported, skipped, failed,
//          reason?}
//   (broadcast) "savedPasswordsChanged"
//
// The shell verifies the user itself before it asks for a plain-text
// password or changes one, and says so with passwordAuthGranted; the five
// sensitive commands are refused with authRequired outside that window.
// Nothing here keeps a password, a username or a note, and nothing here logs
// one: ids are handed out per sign-on realm and username, keyed by a hash.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/affiliations/affiliation_service_factory.h"
#include "chrome/browser/password_manager/factories/account_password_store_factory.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/browser/ui/ohos/device_authenticator_ohos.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/affiliations/core/browser/affiliation_utils.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/password_manager/core/browser/export/export_progress_status.h"
#include "components/password_manager/core/browser/export/password_manager_exporter.h"
#include "components/password_manager/core/browser/import/import_results.h"
#include "components/password_manager/core/browser/import/password_importer.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/ui/credential_ui_entry.h"
#include "components/password_manager/core/browser/ui/saved_passwords_presenter.h"
#include "crypto/sha2.h"

namespace chrome::ohos {

namespace {

using password_manager::CredentialUIEntry;
using password_manager::SavedPasswordsPresenter;

constexpr int kDefaultGrantMs = 60 * 1000;

// Where a reply goes, kept by value: a reply may wait for the password store
// to load, and the browser window or incognito profile the command came from
// may be gone by then.
struct ReplyTarget {
  gfx::AcceleratedWidget widget = gfx::kNullAcceleratedWidget;
  bool incognito = false;
  int request_id = 0;

  void Send(base::DictValue event) const {
    event.Set("requestId", request_id);
    event.Set("incognito", incognito);
    DispatchAuraShellRuntimeEventToWidget(widget, std::move(event));
  }
};

void SendCommandResult(const ReplyTarget& target,
                       std::string_view command,
                       bool ok,
                       std::string_view reason = std::string_view(),
                       const std::string& new_id = std::string()) {
  base::DictValue event;
  event.Set("event", "passwordCommandResult");
  event.Set("command", command);
  event.Set("ok", ok);
  if (!reason.empty()) {
    event.Set("reason", reason);
  }
  if (!new_id.empty()) {
    event.Set("newId", new_id);
  }
  target.Send(std::move(event));
}

void SendExported(const ReplyTarget& target,
                  bool ok,
                  size_t count,
                  std::string_view reason = std::string_view()) {
  base::DictValue event;
  event.Set("event", "passwordsExported");
  event.Set("ok", ok);
  event.Set("count", static_cast<int>(count));
  if (!reason.empty()) {
    event.Set("reason", reason);
  }
  target.Send(std::move(event));
}

void SendImported(const ReplyTarget& target,
                  bool ok,
                  size_t imported,
                  size_t skipped,
                  size_t failed,
                  std::string_view reason = std::string_view()) {
  base::DictValue event;
  event.Set("event", "passwordsImported");
  event.Set("ok", ok);
  event.Set("imported", static_cast<int>(imported));
  event.Set("skipped", static_cast<int>(skipped));
  event.Set("failed", static_cast<int>(failed));
  if (!reason.empty()) {
    event.Set("reason", reason);
  }
  target.Send(std::move(event));
}

// The shell's reason for an import that did not finish.
std::string_view ImportFailureReason(
    password_manager::ImportResults::Status status) {
  using password_manager::ImportResults;
  switch (status) {
    case ImportResults::IO_ERROR:
      return "ioError";
    case ImportResults::BAD_FORMAT:
      return "badFormat";
    case ImportResults::MAX_FILE_SIZE:
    case ImportResults::NUM_PASSWORDS_EXCEEDED:
      return "tooLarge";
    default:
      return "unknown";
  }
}

// What an id is handed out for: one sign-on realm and one username. Hashed,
// so the map that remembers ids holds no usernames. A password change keeps
// the id; a username change is a different credential to the store, and gets
// a new one, which updatePassword reports as newId.
std::string IdKey(const CredentialUIEntry& entry) {
  return base::HexEncode(crypto::SHA256HashString(
      entry.GetFirstSignonRealm() + '\x1f' + base::UTF16ToUTF8(entry.username)));
}

// A sandbox path the shell handed over, to export to or import from.
// Absolute and without parent references; where it points is the shell's to
// choose.
bool IsUsableSandboxPath(const std::string& path) {
  const base::FilePath file_path(path);
  return !path.empty() && file_path.IsAbsolute() &&
         !file_path.ReferencesParent();
}

class ShellPasswords : public SavedPasswordsPresenter::Observer {
 public:
  explicit ShellPasswords(Profile* profile)
      : profile_(profile),
        presenter_(AffiliationServiceFactory::GetForProfile(profile),
                   ProfilePasswordStoreFactory::GetForProfile(
                       profile, ServiceAccessType::EXPLICIT_ACCESS),
                   AccountPasswordStoreFactory::GetForProfile(
                       profile, ServiceAccessType::EXPLICIT_ACCESS)) {
    presenter_.AddObserver(this);
    presenter_.Init(base::BindOnce(&ShellPasswords::OnReady,
                                   weak_factory_.GetWeakPtr()));
  }
  ShellPasswords(const ShellPasswords&) = delete;
  ShellPasswords& operator=(const ShellPasswords&) = delete;
  ~ShellPasswords() override { presenter_.RemoveObserver(this); }

  // Runs `task` once the stores have loaded, which the first command after
  // startup has to wait for.
  void WhenReady(base::OnceClosure task) {
    if (ready_) {
      std::move(task).Run();
      return;
    }
    pending_.push_back(std::move(task));
  }

  void List(const ReplyTarget& target) {
    base::ListValue items;
    for (const CredentialUIEntry& entry : presenter_.GetSavedPasswords()) {
      base::DictValue item;
      item.Set("id", IdFor(entry));
      item.Set("origin", entry.GetURL().DeprecatedGetOriginAsURL().spec());
      item.Set("signonRealm", entry.GetFirstSignonRealm());
      item.Set("username", base::UTF16ToUTF8(entry.username));
      item.Set("hasNote", !entry.note.empty());
      item.Set("dateCreated",
               entry.creation_time ? ToShellTime(*entry.creation_time) : -1.0);
      item.Set("dateLastUsed", ToShellTime(entry.last_used_time));
      item.Set("isAndroidCredential",
               affiliations::IsValidAndroidFacetURI(
                   entry.GetFirstSignonRealm()));
      items.Append(std::move(item));
    }
    LOG(WARNING) << "OHOS passwords: list answered " << items.size()
                 << " items";
    base::DictValue event;
    event.Set("event", "savedPasswords");
    event.Set("items", std::move(items));
    target.Send(std::move(event));
  }

  void Reveal(const ReplyTarget& target, const std::string& id) {
    const std::optional<CredentialUIEntry> entry = Find(id);
    if (!entry) {
      LOG(WARNING) << "OHOS passwords: reveal id=" << id
                   << " failed(notFound)";
      SendCommandResult(target, "revealPassword", false, "notFound");
      return;
    }
    LOG(WARNING) << "OHOS passwords: reveal id=" << id << " allowed";
    base::DictValue event;
    event.Set("event", "passwordRevealed");
    event.Set("id", id);
    event.Set("password", base::UTF16ToUTF8(entry->password));
    event.Set("note", base::UTF16ToUTF8(entry->note));
    target.Send(std::move(event));
  }

  void Update(const ReplyTarget& target,
              const std::string& id,
              const base::DictValue& command) {
    const std::optional<CredentialUIEntry> original = Find(id);
    if (!original) {
      LOG(WARNING) << "OHOS passwords: update id=" << id
                   << " failed(notFound)";
      SendCommandResult(target, "updatePassword", false, "notFound");
      return;
    }
    CredentialUIEntry updated = *original;
    if (const std::string* username = command.FindString("username")) {
      updated.username = base::UTF8ToUTF16(*username);
    }
    if (const std::string* password = command.FindString("password")) {
      updated.password = base::UTF8ToUTF16(*password);
    }
    if (const std::string* note = command.FindString("note")) {
      updated.note = base::UTF8ToUTF16(*note);
    }
    std::string_view reason;
    switch (presenter_.EditSavedCredentials(*original, updated)) {
      case SavedPasswordsPresenter::EditResult::kSuccess:
      case SavedPasswordsPresenter::EditResult::kNothingChanged:
        break;
      case SavedPasswordsPresenter::EditResult::kNotFound:
        reason = "notFound";
        break;
      case SavedPasswordsPresenter::EditResult::kAlreadyExisits:
        reason = "duplicate";
        break;
      case SavedPasswordsPresenter::EditResult::kEmptyPassword:
        reason = "unknown";
        break;
    }
    if (!reason.empty()) {
      LOG(WARNING) << "OHOS passwords: update id=" << id << " failed("
                   << reason << ")";
      SendCommandResult(target, "updatePassword", false, reason);
      return;
    }
    const std::string new_id = IdFor(updated);
    LOG(WARNING) << "OHOS passwords: update id=" << id << " ok"
                 << (new_id != id ? " newId=" + new_id : std::string());
    SendCommandResult(target, "updatePassword", true, std::string_view(),
                      new_id != id ? new_id : std::string());
  }

  void Delete(const ReplyTarget& target, const std::string& id) {
    const std::optional<CredentialUIEntry> entry = Find(id);
    if (!entry) {
      LOG(WARNING) << "OHOS passwords: delete id=" << id
                   << " failed(notFound)";
      SendCommandResult(target, "deletePassword", false, "notFound");
      return;
    }
    const bool ok = presenter_.RemoveCredential(*entry);
    LOG(WARNING) << "OHOS passwords: delete id=" << id
                 << (ok ? " ok" : " failed(unknown)");
    SendCommandResult(target, "deletePassword", ok,
                      ok ? std::string_view() : "unknown");
  }

  void Export(const ReplyTarget& target, const std::string& path) {
    if (exporter_) {
      LOG(WARNING) << "OHOS passwords: export failed(busy)";
      SendExported(target, false, 0, "unknown");
      return;
    }
    export_target_ = target;
    export_count_ = presenter_.GetSavedPasswords().size();
    exporter_ = std::make_unique<password_manager::PasswordManagerExporter>(
        presenter_,
        base::BindRepeating(&ShellPasswords::OnExportProgress,
                            weak_factory_.GetWeakPtr()),
        base::DoNothing());
    exporter_->PreparePasswordsForExport();
    exporter_->SetDestination(base::FilePath(path));
  }

  // Chromium's own importer, the one chrome://password-manager uses: the
  // file is parsed in its sandboxed CSV parser and read into memory only --
  // nothing here keeps a copy, and the shell deletes its own. A row for a
  // site and username already saved with another password is a conflict,
  // which the importer stops on: kept as it is unless `overwrite`.
  void Import(const ReplyTarget& target,
              const std::string& path,
              bool overwrite) {
    if (importer_) {
      LOG(WARNING) << "OHOS passwords: import failed(busy)";
      SendImported(target, false, 0, 0, 0, "unknown");
      return;
    }
    import_target_ = target;
    import_overwrite_ = overwrite;
    import_skipped_ = 0;
    importer_ =
        std::make_unique<password_manager::PasswordImporter>(presenter_);
    importer_->Import(base::FilePath(path),
                      password_manager::PasswordForm::Store::kProfileStore,
                      base::BindOnce(&ShellPasswords::OnImportResults,
                                     weak_factory_.GetWeakPtr()));
  }

  // SavedPasswordsPresenter::Observer:
  void OnSavedPasswordsChanged(
      const password_manager::PasswordStoreChangeList& changes) override {
    base::DictValue event;
    event.Set("event", "savedPasswordsChanged");
    BroadcastToShell(profile_, event.Clone());
    // Incognito windows show the same saved passwords.
    if (profile_->HasPrimaryOTRProfile()) {
      BroadcastToShell(
          profile_->GetPrimaryOTRProfile(/*create_if_needed=*/false),
          std::move(event));
    }
  }

 private:
  void OnReady() {
    ready_ = true;
    std::vector<base::OnceClosure> pending = std::move(pending_);
    for (base::OnceClosure& task : pending) {
      std::move(task).Run();
    }
  }

  std::string IdFor(const CredentialUIEntry& entry) {
    auto [it, inserted] = ids_.try_emplace(IdKey(entry));
    if (inserted) {
      it->second = base::NumberToString(next_id_++);
    }
    return it->second;
  }

  std::optional<CredentialUIEntry> Find(const std::string& id) {
    for (const CredentialUIEntry& entry : presenter_.GetSavedPasswords()) {
      if (IdFor(entry) == id) {
        return entry;
      }
    }
    return std::nullopt;
  }

  void OnExportProgress(const password_manager::PasswordExportInfo& info) {
    using password_manager::ExportProgressStatus;
    if (info.status == ExportProgressStatus::kNotStarted ||
        info.status == ExportProgressStatus::kInProgress) {
      return;
    }
    const bool ok = info.status == ExportProgressStatus::kSucceeded;
    LOG(WARNING) << "OHOS passwords: export count=" << export_count_
                 << (ok ? " ok" : " failed(ioError)");
    SendExported(export_target_, ok, ok ? export_count_ : 0,
                 ok ? std::string_view() : "ioError");
    // The exporter is still on the stack.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(exporter_));
  }

  void OnImportResults(const password_manager::ImportResults& results) {
    using password_manager::ImportEntry;
    using password_manager::ImportResults;
    if (results.status == ImportResults::CONFLICTS) {
      std::vector<int> replace;
      if (import_overwrite_) {
        for (const ImportEntry& entry : results.displayed_entries) {
          replace.push_back(entry.id);
        }
      } else {
        import_skipped_ = results.displayed_entries.size();
      }
      // Not from inside the importer's own callback.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&ShellPasswords::ContinueImport,
                                    weak_factory_.GetWeakPtr(),
                                    std::move(replace)));
      return;
    }

    // Rows that could not be imported: a missing or bad URL, a missing
    // password, a field too long.
    size_t failed = 0;
    for (const ImportEntry& entry : results.displayed_entries) {
      if (entry.status != ImportEntry::VALID) {
        failed++;
      }
    }
    const bool ok = results.status == ImportResults::SUCCESS;
    const std::string_view reason =
        ok ? std::string_view() : ImportFailureReason(results.status);
    LOG(WARNING) << "OHOS passwords: import imported="
                 << results.number_imported << " skipped=" << import_skipped_
                 << " failed=" << failed
                 << (ok ? std::string() : " (" + std::string(reason) + ")");
    SendImported(import_target_, ok, results.number_imported, import_skipped_,
                 failed, reason);
    // The importer is still on the stack.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(importer_));
  }

  void ContinueImport(std::vector<int> replace) {
    if (!importer_) {
      return;
    }
    importer_->ContinueImport(replace,
                              base::BindOnce(&ShellPasswords::OnImportResults,
                                             weak_factory_.GetWeakPtr()));
  }

  const raw_ptr<Profile> profile_;
  SavedPasswordsPresenter presenter_;
  bool ready_ = false;
  std::vector<base::OnceClosure> pending_;
  std::map<std::string, std::string> ids_;
  int next_id_ = 1;
  std::unique_ptr<password_manager::PasswordManagerExporter> exporter_;
  ReplyTarget export_target_;
  size_t export_count_ = 0;
  std::unique_ptr<password_manager::PasswordImporter> importer_;
  ReplyTarget import_target_;
  bool import_overwrite_ = false;
  size_t import_skipped_ = 0;
  base::WeakPtrFactory<ShellPasswords> weak_factory_{this};
};

ShellPasswords* GetShellPasswords(Profile* profile) {
  static base::NoDestructor<PerProfile<ShellPasswords>> passwords(
      base::BindRepeating([](Profile* profile) {
        return std::make_unique<ShellPasswords>(profile);
      }));
  return passwords->Get(profile);
}

// The five commands that hand out or change passwords, refused outside the
// window a verification opened.
bool RequireAuthentication(const ReplyTarget& target,
                           std::string_view command,
                           const std::string& id) {
  if (IsAuthenticationFresh()) {
    return true;
  }
  if (command == "revealPassword") {
    LOG(WARNING) << "OHOS passwords: reveal id=" << id
                 << " denied(authRequired)";
  } else {
    LOG(WARNING) << "OHOS passwords: " << command << " denied(authRequired)";
  }
  if (command == "exportPasswords") {
    SendExported(target, false, 0, "authRequired");
  } else if (command == "importPasswords") {
    SendImported(target, false, 0, 0, 0, "authRequired");
  } else {
    SendCommandResult(target, command, false, "authRequired");
  }
  return false;
}

}  // namespace

bool HandlePasswordsCommand(const ShellCommandContext& context,
                            std::string_view name,
                            const base::DictValue& command) {
  if (name == "passwordAuthGranted") {
    GrantAuthenticationFromShell(base::Milliseconds(
        command.FindInt("validMs").value_or(kDefaultGrantMs)));
    return true;
  }

  // Incognito windows read and edit the ordinary profile's passwords, as
  // Chromium's own page does.
  Profile* profile = context.profile->GetOriginalProfile();
  ShellPasswords* passwords = GetShellPasswords(profile);
  if (!passwords) {
    return false;
  }
  const ReplyTarget target{context.widget, context.profile->IsOffTheRecord(),
                           ReadRequestId(command)};
  const std::string* id_value = command.FindString("id");
  const std::string id = id_value ? *id_value : std::string();

  if (name == "getSavedPasswords") {
    passwords->WhenReady(base::BindOnce(
        [](ShellPasswords* passwords, ReplyTarget target) {
          passwords->List(target);
        },
        base::Unretained(passwords), target));
    return true;
  }
  if (name != "revealPassword" && name != "updatePassword" &&
      name != "deletePassword" && name != "exportPasswords" &&
      name != "importPasswords") {
    return false;
  }
  if (!RequireAuthentication(target, name, id)) {
    return true;
  }
  if (name == "revealPassword") {
    passwords->WhenReady(base::BindOnce(&ShellPasswords::Reveal,
                                        base::Unretained(passwords), target,
                                        id));
  } else if (name == "updatePassword") {
    passwords->WhenReady(base::BindOnce(&ShellPasswords::Update,
                                        base::Unretained(passwords), target,
                                        id, command.Clone()));
  } else if (name == "deletePassword") {
    passwords->WhenReady(base::BindOnce(&ShellPasswords::Delete,
                                        base::Unretained(passwords), target,
                                        id));
  } else if (name == "importPasswords") {
    const std::string* path = command.FindString("path");
    if (!path || !IsUsableSandboxPath(*path)) {
      LOG(WARNING) << "OHOS passwords: import failed(ioError)";
      SendImported(target, false, 0, 0, 0, "ioError");
      return true;
    }
    passwords->WhenReady(base::BindOnce(
        &ShellPasswords::Import, base::Unretained(passwords), target, *path,
        command.FindBool("overwrite").value_or(false)));
  } else {
    const std::string* path = command.FindString("path");
    if (!path || !IsUsableSandboxPath(*path)) {
      LOG(WARNING) << "OHOS passwords: export failed(ioError)";
      SendExported(target, false, 0, "ioError");
      return true;
    }
    passwords->WhenReady(base::BindOnce(&ShellPasswords::Export,
                                        base::Unretained(passwords), target,
                                        *path));
  }
  return true;
}

}  // namespace chrome::ohos
