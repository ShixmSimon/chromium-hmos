// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Cloud sync of saved passwords (passwordApiVersion 1). The shell keeps a
// table that HarmonyOS syncs with Cloud Space between the user's devices, and
// carries entries between that table and Chromium:
//
//   passwordSyncAuthorize {granted}
//   getPasswordsForSync {requestId}
//     -> "passwordsForSync" {requestId, ok, error?, entries: [{signonRealm,
//          origin, username, password, note, blocked, dateCreated,
//          dateLastUsed}]}
//   applyPasswords {requestId, origin: 'sync', ops: [upsert | remove]}
//     -> "passwordOpResult" {requestId, ok, error?, failedKeys?}
//   (broadcast) "passwordsChanged" {revision}
//
// The shell verifies the user once, when they turn sync on, and says so with
// passwordSyncAuthorize; the grant is kept in a profile pref until they turn
// it off or sign out. Without it the two data commands are refused.
//
// Entries are the profile store's web credentials and "never save" entries,
// keyed by sign-on realm and username (a "never save" entry by realm alone).
// Android credentials and federated sign-ins stay on the device. Plain text
// goes to the shell over the runtime channel and nowhere else; nothing here
// logs a username, a password or a note.

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/ohos/aura_shell_runtime_bridge.h"
#include "chrome/browser/ui/ohos/shell_services_ohos.h"
#include "components/affiliations/core/browser/affiliation_utils.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/password_form_converters.h"
#include "components/password_manager/core/browser/password_store/password_store_change.h"
#include "components/password_manager/core/browser/password_store/password_store_consumer.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/password_manager/core/browser/password_store/stored_credential.h"
#include "components/password_manager/core/browser/password_string.h"
#include "components/prefs/pref_service.h"
#include "crypto/sha2.h"
#include "url/gurl.h"

namespace chrome::ohos {

namespace {

using password_manager::PasswordForm;
using password_manager::PasswordStoreChange;
using password_manager::PasswordStoreChangeList;
using password_manager::PasswordStoreConsumer;
using password_manager::PasswordStoreInterface;
using password_manager::StoredCredential;

constexpr char kAuthorizedPref[] = "ohos.password_sync_authorized";

// Changes that come close together are told as one.
constexpr base::TimeDelta kChangeDelay = base::Milliseconds(200);

// Where a reply goes, kept by value: it waits for the password store, and the
// window the command came from may be gone by then.
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

// Web credentials with a password, and "never save" entries. Not Android
// apps' credentials, which belong to that device's apps, and not federated
// sign-ins, which have no password to carry.
bool IsSyncable(const StoredCredential& credential) {
  if (affiliations::IsValidAndroidFacetURI(credential.signon_realm) ||
      credential.federation_origin.IsValid()) {
    return false;
  }
  return credential.blocked_by_user || !credential.password_value.empty();
}

// What an entry is to sync: a realm and a username, or a realm that is never
// saved. Not a string the shell sees.
std::string SyncKey(const std::string& signon_realm,
                    const std::u16string& username,
                    bool blocked) {
  return blocked ? "B" + signon_realm
                 : "C" + signon_realm + '\x1f' + base::UTF16ToUTF8(username);
}

std::string SyncKey(const StoredCredential& credential) {
  return SyncKey(credential.signon_realm, credential.username_value,
                 credential.blocked_by_user);
}

// How a failed op is named back to the shell.
std::string FailedKey(const std::string& signon_realm,
                      const std::string& username) {
  return signon_realm + "\n" + username;
}

// The scheme a realm the shell sends was saved under: a web form's realm is
// its origin, an HTTP authentication realm adds the realm's name after it.
PasswordForm::Scheme SchemeForRealm(const std::string& signon_realm) {
  const GURL url(signon_realm);
  return url.is_valid() && url.GetWithEmptyPath().spec() == signon_realm
             ? PasswordForm::Scheme::kHtml
             : PasswordForm::Scheme::kBasic;
}

class PasswordSync;

// One read of everything in the store.
class LoginsRequest : public PasswordStoreConsumer {
 public:
  using Callback =
      base::OnceCallback<void(std::optional<std::vector<StoredCredential>>)>;

  LoginsRequest(base::WeakPtr<PasswordSync> owner, Callback callback)
      : owner_(std::move(owner)), callback_(std::move(callback)) {}
  ~LoginsRequest() override = default;

  base::WeakPtr<PasswordStoreConsumer> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // PasswordStoreConsumer:
  void OnGetPasswordStoreResultsOrErrorFrom(
      PasswordStoreInterface* store,
      password_manager::LoginsResultOrError results_or_error) override;

 private:
  const base::WeakPtr<PasswordSync> owner_;
  Callback callback_;
  base::WeakPtrFactory<LoginsRequest> weak_factory_{this};
};

// The sync side of one profile's password store: the authorization, the
// snapshot and writes, and the change notice.
class PasswordSync : public PasswordStoreInterface::Observer {
 public:
  explicit PasswordSync(Profile* profile)
      : profile_(profile),
        salt_(base::RandBytesAsString(16)) {}
  PasswordSync(const PasswordSync&) = delete;
  PasswordSync& operator=(const PasswordSync&) = delete;
  ~PasswordSync() override;

  bool IsAuthorized() const {
    return profile_->GetPrefs()->GetBoolean(kAuthorizedPref);
  }

  void SetAuthorized(bool granted) {
    profile_->GetPrefs()->SetBoolean(kAuthorizedPref, granted);
    LOG(WARNING) << "OHOS password sync: "
                 << (granted ? "authorized" : "authorization revoked");
    if (granted) {
      StartWatching();
    } else {
      StopWatching();
    }
  }

  // Watches the store for changes to tell the shell about, while sync is
  // authorized: nothing listens for passwordsChanged otherwise.
  void StartWatching() {
    if (!IsAuthorized() || observation_.IsObserving()) {
      return;
    }
    PasswordStoreInterface* store = Store();
    if (!store) {
      return;
    }
    observation_.Observe(store);
    // What is in the store now, so that an update which only touched the
    // last-used time can be told from a real one.
    ReadAll(base::BindOnce(&PasswordSync::OnBaseline,
                           weak_factory_.GetWeakPtr()));
  }

  void StopWatching() {
    observation_.Reset();
    fingerprints_.reset();
    change_timer_.Stop();
  }

  void Snapshot(const ReplyTarget& target) {
    if (!IsAuthorized()) {
      LOG(WARNING) << "OHOS password sync: snapshot refused(notAuthorized)";
      SendSnapshot(target, false, "notAuthorized", base::ListValue());
      return;
    }
    ReadAll(base::BindOnce(&PasswordSync::OnSnapshotRead,
                           weak_factory_.GetWeakPtr(), target));
  }

  void Apply(const ReplyTarget& target, base::ListValue ops) {
    if (!IsAuthorized()) {
      LOG(WARNING) << "OHOS password sync: apply refused(notAuthorized)";
      SendOpResult(target, false, "notAuthorized", base::ListValue());
      return;
    }
    if (applying_) {
      // One batch at a time: each works from what the store holds.
      pending_applies_.emplace_back(target, std::move(ops));
      return;
    }
    applying_ = true;
    ReadAll(base::BindOnce(&PasswordSync::OnApplyRead,
                           weak_factory_.GetWeakPtr(), target,
                           std::move(ops)));
  }

  // PasswordStoreInterface::Observer:
  void OnLoginsChanged(PasswordStoreInterface* store,
                       const PasswordStoreChangeList& changes) override {
    bool changed = !fingerprints_;
    if (fingerprints_) {
      for (const PasswordStoreChange& change : changes) {
        const StoredCredential& credential = change.credential();
        if (!IsSyncable(credential)) {
          continue;
        }
        const std::string key = FormKey(credential);
        if (change.type() == PasswordStoreChange::REMOVE) {
          changed |= fingerprints_->erase(key) > 0;
          continue;
        }
        std::string& fingerprint = (*fingerprints_)[key];
        const std::string now = Fingerprint(credential);
        // Filling a password only moves its last-used time, which is not
        // worth a sync round -- and every fill would be one.
        changed |= fingerprint != now;
        fingerprint = now;
      }
    }
    if (changed) {
      ScheduleChanged();
    }
  }

  void OnLoginsRetained(
      PasswordStoreInterface* store,
      const std::vector<StoredCredential>& retained) override {
    if (Rebaseline(retained)) {
      ScheduleChanged();
    }
  }

  // The request that read the store, done with.
  void Forget(LoginsRequest* request);

  base::WeakPtr<PasswordSync> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  using Logins = std::optional<std::vector<StoredCredential>>;
  using LoginsCallback = base::OnceCallback<void(Logins)>;

  PasswordStoreInterface* Store() {
    return ProfilePasswordStoreFactory::GetForProfile(
               profile_, ServiceAccessType::EXPLICIT_ACCESS)
        .get();
  }

  void ReadAll(LoginsCallback callback);

  // One stored form: its unique key in the store, hashed.
  std::string FormKey(const StoredCredential& credential) const {
    return crypto::SHA256HashString(
        salt_ + credential.signon_realm + '\x1f' + credential.url.spec() +
        '\x1f' + base::UTF16ToUTF8(credential.username_element) + '\x1f' +
        base::UTF16ToUTF8(credential.username_value) + '\x1f' +
        base::UTF16ToUTF8(credential.password_element));
  }

  // Everything sync carries of a form, hashed with a per-process salt so the
  // map holds nothing a password could be read back from.
  std::string Fingerprint(const StoredCredential& credential) const {
    return crypto::SHA256HashString(
        salt_ + credential.url.spec() + '\x1f' +
        base::UTF16ToUTF8(credential.password_value.value()) + '\x1f' +
        base::UTF16ToUTF8(credential.GetPasswordNote()) + '\x1f' +
        (credential.blocked_by_user ? "1" : "0") + '\x1f' +
        base::NumberToString(ToShellTime(credential.date_created)));
  }

  // Replaces the fingerprints with `logins`; whether anything differed.
  bool Rebaseline(const std::vector<StoredCredential>& logins) {
    std::map<std::string, std::string> fingerprints;
    for (const StoredCredential& credential : logins) {
      if (IsSyncable(credential)) {
        fingerprints[FormKey(credential)] = Fingerprint(credential);
      }
    }
    const bool changed = !fingerprints_ || *fingerprints_ != fingerprints;
    fingerprints_ = std::move(fingerprints);
    return changed;
  }

  void OnBaseline(Logins logins) {
    if (logins && observation_.IsObserving()) {
      Rebaseline(*logins);
    }
  }

  void ScheduleChanged() {
    if (!IsAuthorized() || applying_) {
      // A batch being written is told once, when it is done.
      return;
    }
    change_timer_.Start(FROM_HERE, kChangeDelay,
                        base::BindOnce(&PasswordSync::SendChanged,
                                       base::Unretained(this)));
  }

  void SendChanged() {
    // Rises across restarts too: it starts from the clock.
    revision_ = std::max(revision_ + 1, ToShellTime(base::Time::Now()));
    LOG(WARNING) << "OHOS password sync: passwordsChanged";
    base::DictValue event;
    event.Set("event", "passwordsChanged");
    event.Set("revision", revision_);
    BroadcastToShell(profile_, event.Clone());
    if (profile_->HasPrimaryOTRProfile()) {
      BroadcastToShell(
          profile_->GetPrimaryOTRProfile(/*create_if_needed=*/false),
          std::move(event));
    }
  }

  void SendSnapshot(const ReplyTarget& target,
                    bool ok,
                    std::string_view error,
                    base::ListValue entries) {
    base::DictValue event;
    event.Set("event", "passwordsForSync");
    event.Set("ok", ok);
    if (!error.empty()) {
      event.Set("error", error);
    }
    event.Set("entries", std::move(entries));
    target.Send(std::move(event));
  }

  void SendOpResult(const ReplyTarget& target,
                    bool ok,
                    std::string_view error,
                    base::ListValue failed_keys) {
    base::DictValue event;
    event.Set("event", "passwordOpResult");
    event.Set("ok", ok);
    if (!error.empty()) {
      event.Set("error", error);
    }
    if (!failed_keys.empty()) {
      event.Set("failedKeys", std::move(failed_keys));
    }
    target.Send(std::move(event));
  }

  void OnSnapshotRead(const ReplyTarget& target, Logins logins) {
    if (!logins) {
      LOG(WARNING) << "OHOS password sync: snapshot failed(unknown)";
      SendSnapshot(target, false, "unknown", base::ListValue());
      return;
    }
    // One entry per realm and username: the most recently changed form when
    // the store holds several (one per page the login was saved on).
    std::map<std::string, const StoredCredential*> newest;
    for (const StoredCredential& credential : *logins) {
      if (!IsSyncable(credential)) {
        continue;
      }
      const StoredCredential*& slot = newest[SyncKey(credential)];
      if (!slot || Modified(credential) > Modified(*slot)) {
        slot = &credential;
      }
    }
    base::ListValue entries;
    for (const auto& [key, credential] : newest) {
      const bool blocked = credential->blocked_by_user;
      base::DictValue entry;
      entry.Set("signonRealm", credential->signon_realm);
      entry.Set("origin", credential->url.spec());
      entry.Set("username",
                blocked ? std::string()
                        : base::UTF16ToUTF8(credential->username_value));
      entry.Set("password",
                blocked ? std::string()
                        : base::UTF16ToUTF8(credential->password_value.value()));
      entry.Set("note", blocked ? std::string()
                                : base::UTF16ToUTF8(
                                      credential->GetPasswordNote()));
      entry.Set("blocked", blocked);
      entry.Set("dateCreated", ToShellTime(credential->date_created));
      entry.Set("dateLastUsed", ToShellTime(credential->date_last_used));
      entries.Append(std::move(entry));
    }
    LOG(WARNING) << "OHOS password sync: snapshot answered " << entries.size()
                 << " entries";
    SendSnapshot(target, true, std::string_view(), std::move(entries));
  }

  static base::Time Modified(const StoredCredential& credential) {
    return std::max(credential.date_password_modified,
                    credential.date_created);
  }

  void OnApplyRead(const ReplyTarget& target, base::ListValue ops, Logins logins);
  void OnApplyWritten(const ReplyTarget& target,
                      base::ListValue failed_keys,
                      size_t written,
                      Logins logins);
  // The batch that waited for the one just finished, if any.
  void RunNextApply();

  const raw_ptr<Profile> profile_;
  const std::string salt_;
  base::ScopedObservation<PasswordStoreInterface,
                          PasswordStoreInterface::Observer>
      observation_{this};
  // FormKey -> Fingerprint of every syncable form, once read.
  std::optional<std::map<std::string, std::string>> fingerprints_;
  base::OneShotTimer change_timer_;
  double revision_ = 0;
  bool applying_ = false;
  std::vector<std::pair<ReplyTarget, base::ListValue>> pending_applies_;
  std::vector<std::unique_ptr<LoginsRequest>> requests_;
  base::WeakPtrFactory<PasswordSync> weak_factory_{this};
};

PasswordSync::~PasswordSync() = default;

void LoginsRequest::OnGetPasswordStoreResultsOrErrorFrom(
    PasswordStoreInterface* store,
    password_manager::LoginsResultOrError results_or_error) {
  std::optional<std::vector<StoredCredential>> logins;
  if (auto* result =
          std::get_if<password_manager::LoginsResult>(&results_or_error)) {
    logins = std::move(*result);
  }
  Callback callback = std::move(callback_);
  // Not from inside the store's own callback.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&PasswordSync::Forget, owner_, base::Unretained(this)));
  std::move(callback).Run(std::move(logins));
}

void PasswordSync::Forget(LoginsRequest* request) {
  std::erase_if(requests_, [request](const auto& entry) {
    return entry.get() == request;
  });
}

void PasswordSync::ReadAll(LoginsCallback callback) {
  PasswordStoreInterface* store = Store();
  if (!store) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  requests_.push_back(
      std::make_unique<LoginsRequest>(GetWeakPtr(), std::move(callback)));
  store->GetAllLogins(requests_.back()->GetWeakPtr());
}

void PasswordSync::OnApplyRead(const ReplyTarget& target,
                               base::ListValue ops,
                               Logins logins) {
  PasswordStoreInterface* store = Store();
  if (!logins || !store) {
    LOG(WARNING) << "OHOS password sync: apply failed(unknown)";
    applying_ = false;
    SendOpResult(target, false, "unknown", base::ListValue());
    RunNextApply();
    return;
  }

  // What the store holds, by sync key, kept up to date as ops are written so
  // that later ops in the batch see the earlier ones. The store runs writes
  // in the order they are made.
  std::map<std::string, std::vector<StoredCredential>> forms;
  for (StoredCredential& credential : *logins) {
    if (IsSyncable(credential)) {
      std::string key = SyncKey(credential);
      forms[std::move(key)].push_back(std::move(credential));
    }
  }

  const base::Time now = base::Time::Now();
  base::ListValue failed_keys;
  size_t written = 0;
  size_t upserts = 0;
  size_t removes = 0;
  for (const base::Value& value : ops) {
    const base::DictValue* op = value.GetIfDict();
    const std::string* kind = op ? op->FindString("op") : nullptr;
    const std::string* realm = op ? op->FindString("signonRealm") : nullptr;
    const std::string* username_value = op ? op->FindString("username") : nullptr;
    const bool blocked = op && op->FindBool("blocked").value_or(false);
    const std::string username =
        username_value && !blocked ? *username_value : std::string();
    const GURL realm_url(realm ? *realm : std::string());
    if (!kind || !realm || !realm_url.is_valid() ||
        !realm_url.SchemeIsHTTPOrHTTPS() ||
        (*kind != "upsert" && *kind != "remove")) {
      failed_keys.Append(FailedKey(realm ? *realm : std::string(), username));
      continue;
    }
    const std::u16string username16 = base::UTF8ToUTF16(username);
    std::vector<StoredCredential>& existing =
        forms[SyncKey(*realm, username16, blocked)];

    if (*kind == "remove") {
      ++removes;
      for (const StoredCredential& credential : existing) {
        store->RemoveLogin(FROM_HERE, credential);
        ++written;
      }
      existing.clear();
      continue;
    }

    ++upserts;
    const std::string* password_value = op->FindString("password");
    const std::string* note_value = op->FindString("note");
    const std::u16string password =
        password_value && !blocked ? base::UTF8ToUTF16(*password_value)
                                   : std::u16string();
    const std::u16string note =
        note_value && !blocked ? base::UTF8ToUTF16(*note_value)
                               : std::u16string();
    const base::Time last_used =
        FromShellTime(op->FindDouble("dateLastUsed"));
    if (!blocked && password.empty()) {
      failed_keys.Append(FailedKey(*realm, username));
      continue;
    }

    if (!existing.empty()) {
      // Every form saved for this realm and username gets the password: a
      // login saved on two pages is one login.
      for (StoredCredential& credential : existing) {
        if (blocked) {
          continue;
        }
        bool changed = false;
        if (credential.password_value != password) {
          credential.password_value =
              password_manager::PasswordString(std::u16string(password));
          credential.date_password_modified = now;
          changed = true;
        }
        if (credential.GetPasswordNote() != note) {
          credential.SetPasswordNote(note);
          changed = true;
        }
        if (last_used > credential.date_last_used) {
          credential.date_last_used = last_used;
          changed = true;
        }
        if (changed) {
          store->UpdateLogin(password_manager::CloneStoredCredential(credential));
          ++written;
        }
      }
      continue;
    }

    StoredCredential credential;
    credential.scheme = SchemeForRealm(*realm);
    credential.signon_realm = *realm;
    const std::string* origin = op->FindString("origin");
    const GURL url(origin ? *origin : std::string());
    credential.url = blocked || !url.is_valid() || !url.SchemeIsHTTPOrHTTPS()
                         ? realm_url.GetWithEmptyPath()
                         : url;
    credential.blocked_by_user = blocked;
    credential.username_value = username16;
    credential.password_value =
        password_manager::PasswordString(std::u16string(password));
    if (!note.empty()) {
      credential.SetPasswordNote(note);
    }
    const base::Time created = FromShellTime(op->FindDouble("dateCreated"));
    credential.date_created = created.is_null() ? now : created;
    credential.date_last_used = last_used;
    credential.date_password_modified = now;
    credential.type = PasswordForm::Type::kImported;
    credential.in_store = PasswordForm::Store::kProfileStore;
    store->AddLogin(password_manager::CloneStoredCredential(credential));
    existing.push_back(std::move(credential));
    ++written;
  }

  LOG(WARNING) << "OHOS password sync: apply ops=" << ops.size()
               << " upserts=" << upserts << " removes=" << removes
               << " written=" << written << " failed=" << failed_keys.size();
  // Read once more: it runs after the writes, so its answer means they are
  // done, and it is the new baseline for telling changes apart.
  ReadAll(base::BindOnce(&PasswordSync::OnApplyWritten,
                         weak_factory_.GetWeakPtr(), target,
                         std::move(failed_keys), written));
}

void PasswordSync::OnApplyWritten(const ReplyTarget& target,
                                  base::ListValue failed_keys,
                                  size_t written,
                                  Logins logins) {
  applying_ = false;
  if (logins && observation_.IsObserving()) {
    Rebaseline(*logins);
  }
  const bool ok = failed_keys.empty();
  SendOpResult(target, ok, ok ? std::string_view() : "someRefused",
               std::move(failed_keys));
  // The batch is told once, as any other change -- the shell compares what it
  // reads back, so its own writes do not come back round.
  if (written > 0) {
    ScheduleChanged();
  }
  RunNextApply();
}

void PasswordSync::RunNextApply() {
  if (pending_applies_.empty()) {
    return;
  }
  auto [target, ops] = std::move(pending_applies_.front());
  pending_applies_.erase(pending_applies_.begin());
  Apply(target, std::move(ops));
}

PerProfile<PasswordSync>& AllPasswordSync() {
  static base::NoDestructor<PerProfile<PasswordSync>> sync(
      base::BindRepeating([](Profile* profile) {
        return std::make_unique<PasswordSync>(profile);
      }));
  return *sync;
}

}  // namespace

bool HandlePasswordSyncCommand(const ShellCommandContext& context,
                               std::string_view name,
                               const base::DictValue& command) {
  if (name == "passwordSyncAuthorize") {
    // Only an ordinary window turns sync on or off.
    if (context.profile->IsOffTheRecord()) {
      LOG(WARNING) << "OHOS password sync: authorize from incognito ignored";
      return true;
    }
    AllPasswordSync()
        .Get(context.profile)
        ->SetAuthorized(command.FindBool("granted").value_or(false));
    return true;
  }
  if (name != "getPasswordsForSync" && name != "applyPasswords") {
    return false;
  }
  PasswordSync* sync =
      AllPasswordSync().Get(context.profile->GetOriginalProfile());
  const ReplyTarget target{context.widget, context.profile->IsOffTheRecord(),
                           ReadRequestId(command)};
  if (name == "getPasswordsForSync") {
    sync->Snapshot(target);
    return true;
  }
  const base::ListValue* ops = command.FindList("ops");
  sync->Apply(target, ops ? ops->Clone() : base::ListValue());
  return true;
}

void EnsurePasswordSyncObserver(Profile* profile) {
  if (!profile || profile->IsOffTheRecord() ||
      !profile->GetPrefs()->GetBoolean(kAuthorizedPref)) {
    return;
  }
  AllPasswordSync().Get(profile)->StartWatching();
}

bool IsPasswordSyncAuthorized(Profile* profile) {
  return profile &&
         profile->GetOriginalProfile()->GetPrefs()->GetBoolean(kAuthorizedPref);
}

}  // namespace chrome::ohos
