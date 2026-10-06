/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// The data a shell draws its own bookmark, history, download and settings
// pages from. Chromium keeps the data in its Profile; the shell asks with
// browser commands and gets these back in runtime events. See
// docs/SHELL_CONTRACT.md, "Profile data".
//
// Conventions: times are milliseconds since the Unix epoch (UTC), -1 when
// unknown; byte counts are numbers, -1 when unknown; Chromium's 64-bit ids are
// strings.

export interface BookmarkNode {
  id: string;
  parentId: string;
  type: 'url' | 'folder';
  title: string;
  url?: string;
  dateAdded: number;
  index: number;
  // Folders only: how many children they hold directly, not counting what is
  // nested deeper. Absent on an engine older than bookmarkApiVersion 2.
  childCount?: number;
  // Only the three permanent folders carry this. Their titles are localized,
  // so it is the only reliable way to tell the bookmark bar from the mobile
  // folder -- which is where a new bookmark goes by default.
  rootType?: 'mobile' | 'bookmarkBar' | 'other';
  // searchBookmarks only: the titles from the permanent folder down to this
  // node's parent, for the "where it lives" line under a flat search hit.
  path?: string[];
}

export interface HistoryItem {
  url: string;
  title: string;
  visitTime: number;
  visitCount: number;
}

// A visit to delete with removeHistoryItems: the url and visitTime exactly as
// historyResults reported them.
export interface HistoryVisitRef {
  url: string;
  visitTime: number;
}

export interface AutocompleteItem {
  type: 'history' | 'bookmark' | 'search' | 'openTab' | 'url';
  title: string;
  url: string;
  description: string;
}

export interface TopSite {
  url: string;
  title: string;
}

export type DownloadState = 'inProgress' | 'paused' | 'completed' | 'cancelled' | 'failed';

export type DownloadAction =
  'pause' | 'resume' | 'cancel' | 'retry' | 'remove' | 'removeAndDeleteFile' |
  // For a download Chromium flagged as dangerous: keep it anyway, or drop it.
  'keepDangerous' | 'discardDangerous';

export interface DownloadItem {
  id: string;
  url: string;
  fileName: string;
  // The final location once known; a temporary file before that.
  filePath: string;
  mimeType: string;
  receivedBytes: number;
  totalBytes: number;
  state: DownloadState;
  // Chromium's DownloadInterruptReason name; empty when there is none.
  failReason: string;
  canResume: boolean;
  // Chromium flagged the file as dangerous or insecure. It waits, as
  // 'inProgress', until the user picks keepDangerous or discardDangerous.
  dangerous: boolean;
  // Chromium's danger type name, e.g. 'DangerousFile'; empty when not dangerous.
  dangerType: string;
  startTime: number;
  endTime: number;
  // Only in the downloadUpdated sent when the item was removed.
  removed?: boolean;
}

export type BrowsingDataType =
  'history' | 'cookies' | 'cache' | 'siteSettings' | 'formData' | 'passwords' | 'downloads';

export type BrowsingDataTimeRange = 'lastHour' | 'lastDay' | 'lastWeek' | 'last4Weeks' | 'all';

export interface SearchEngine {
  id: string;
  name: string;
  keyword: string;
  // Chromium's raw template, {google:baseURL} and all. Nothing outside
  // Chromium can resolve those, so use searchUrl unless you want the
  // template itself.
  url: string;
  // The same URL with everything resolved except the query: substitute
  // {searchTerms} with the encoded search terms and navigate. Empty when the
  // engine has no search URL.
  searchUrl?: string;
  isDefault: boolean;
}

// The preferences a shell may read and write. Anything else is ignored.
export type ShellPrefKey =
  'blockThirdPartyCookies' | 'doNotTrack' | 'safeBrowsing' | 'preloadPages' |
  'popupsBlocked' | 'javascriptEnabled' | 'textScale' | 'autofillAddresses' |
  'autofillCards' | 'downloadAskWhereToSave' |
  // HarmonyOS's Password Vault fills and saves; Chromium keeps a silent copy.
  'passwordVault' |
  // Blink's auto dark mode for pages that have none; open tabs follow at once.
  'forceDarkWebContents';

export interface ShellPrefValue {
  key: ShellPrefKey;
  value: boolean | number | string;
}

// No 'autoplay': desktop Chromium does not enforce it; 'sound' is the control
// that works.
export type SiteSettingType =
  'location' | 'camera' | 'microphone' | 'notifications' | 'javascript' |
  'popups' | 'sound' | 'clipboard' | 'storageAccess';

// setSiteSetting also takes 'default', which removes the site's exception.
export type SiteSettingValue = 'allow' | 'block' | 'ask' | 'default';

export interface SiteSetting {
  // Chromium's pattern, e.g. 'https://a.com:443' or '[*.]a.com'.
  origin: string;
  type: SiteSettingType;
  setting: SiteSettingValue;
  // storageAccess only: the site the grant is embedded in.
  embeddingOrigin?: string;
}

export interface FaviconImage {
  url: string;
  pngBase64: string;
}

// --- The events, one interface each. ---------------------------------------
//
// They arrive through onRuntimeEvent as a BrowserRuntimeEvent whose `event`
// names them; read their fields through shellEvent<T>(), e.g.
//   if (event.event === 'historyResults') {
//     const results = shellEvent<HistoryResultsEvent>(event);
//   }
// Every one carries `incognito`: whether it concerns an incognito profile.

export interface ShellServiceEvent {
  event: string;
  incognito: boolean;
}

// A keyboard shortcut whose UI the shell owns. Chromium resolved the key and
// decided which command it means; the shell carries it out. Keys a page
// consumed with preventDefault never arrive.
export interface ShellAcceleratorEvent extends ShellServiceEvent {
  action: ShellAcceleratorAction;
}

export type ShellAcceleratorAction =
  | 'focusLocation'      // Ctrl+L, Alt+D
  | 'find'               // Ctrl+F
  | 'bookmarkPage'       // Ctrl+D
  | 'showBookmarks'      // Ctrl+Shift+O
  | 'toggleBookmarkBar'  // Ctrl+Shift+B
  | 'showHistory'        // Ctrl+H
  | 'showDownloads'      // Ctrl+J
  | 'showMenu'           // Alt+F, Alt+E
  // Esc. Sent when the page did not consume it. Chromium also stops the load,
  // which is what a browser does; close whatever the shell has open.
  | 'escape';

// What each action is called for a menu. One list, here, so the shell does not
// keep a second copy: the engine never sends key names, only the action.
// ArkTS wants a named interface for an object literal, so the map has one.
export interface ShellAcceleratorKeys {
  focusLocation: string;
  find: string;
  bookmarkPage: string;
  showBookmarks: string;
  toggleBookmarkBar: string;
  showHistory: string;
  showDownloads: string;
  showMenu: string;
  escape: string;
}

export const SHELL_ACCELERATOR_KEYS: ShellAcceleratorKeys = {
  focusLocation: 'Ctrl+L',
  find: 'Ctrl+F',
  bookmarkPage: 'Ctrl+D',
  showBookmarks: 'Ctrl+Shift+O',
  toggleBookmarkBar: 'Ctrl+Shift+B',
  showHistory: 'Ctrl+H',
  showDownloads: 'Ctrl+J',
  showMenu: 'Alt+F',
  escape: 'Esc'
};

// The link under the pointer, for the label a shell draws in the corner of the
// page. An empty url means the pointer left the link: hide the label.
export interface LinkHoveredEvent extends ShellServiceEvent {
  url: string;
}

// Where the shell drew a button that one of Chromium's bubbles points at, in
// the window's own coordinates (vp). Reported as a whole set on every layout
// change; an anchor left out is one the shell stopped drawing, and its bubble
// falls back to the top right of the page area.
export interface ShellAnchorRect {
  id: string;
  x: number;
  y: number;
  width: number;
  height: number;
}

// getTabThumbnails. Only tabs with a picture come back: the current one is
// captured now, a background one is whatever was last seen of it, and a tab
// never seen is simply absent -- draw a placeholder.
export interface TabThumbnailsEvent extends ShellServiceEvent {
  requestId: number;
  items: TabThumbnail[];
}

export interface TabThumbnail {
  id: string;
  pngBase64: string;
}

// getRecentlyClosed, newest first. Tabs only, and never incognito ones.
export interface RecentlyClosedEvent extends ShellServiceEvent {
  requestId: number;
  items: RecentlyClosedTab[];
}

export interface RecentlyClosedTab {
  id: string;
  title: string;
  url: string;
  closedTime: number;
}

// getExtensionActions: the buttons that would have been on Chromium's toolbar.
// Tapping one is runExtensionAction; the popup it opens is still Chromium's.
export interface ExtensionActionsEvent extends ShellServiceEvent {
  requestId: number;
  items: ExtensionActionItem[];
}

export interface ExtensionActionItem {
  id: string;
  name: string;
  // Empty when the extension supplied no icon; draw a letter tile.
  iconPngBase64: string;
  badgeText: string;
  // #AARRGGBB.
  badgeColor: string;
  // False when the extension has nothing to do on this page.
  enabled: boolean;
  pinned: boolean;
  // Whether the reader has the extension switched on -- the switch on an
  // extensions page. Not `enabled`, which is about the current page. Switched
  // off extensions are listed after the others.
  userEnabled: boolean;
  // Why a disabled extension is disabled, Chromium's own reasons by name --
  // 'userAction' is the reader's switch; 'notVerified', 'corrupted',
  // 'permissionsIncrease' and the rest are Chromium's doing. Empty when
  // enabled.
  disableReasons: string[];
  // Whether it has a popup (action.default_popup).
  hasPopup: boolean;
  // Its options page, or empty. openExtensionOptions opens it.
  optionsUrl: string;
  version: string;
  description: string;
  author: string;
}

// getExtensionDetails.
export interface ExtensionDetailsEvent extends ShellServiceEvent {
  requestId: number;
  id: string;
  // Chromium's own permission lines, in the reader's language. A line with
  // detail under it (sites, devices) keeps the detail on following lines.
  permissions: string[];
  siteAccess: ExtensionSiteAccess;
  // False when the extension asks for no sites, or policy decides for it.
  siteAccessChangeable: boolean;
  // 'notFound' when there is no such extension.
  error?: string;
}

export type ExtensionSiteAccess = 'onClick' | 'specificSites' | 'allSites';

// A setExtensionEnabled, uninstallExtension, setExtensionSiteAccess or
// openExtensionOptions that could not be carried out. A cancelled uninstall
// is not one.
export interface ExtensionCommandFailedEvent extends ShellServiceEvent {
  command: string;
  id: string;
  // notFound, notAllowed, stillDisabled (switched on but disabled for
  // another reason, such as new permissions), badMode, failed.
  reason: string;
  // Chromium's own sentence when it gave one, in the reader's language.
  message: string;
}

// A site is asking the user for a permission, and the shell draws the sheet
// that asks. Answer with the sitePermissionDecision command carrying the same
// requestId; answer exactly once.
//
// This is not permissionsRequested, which asks the shell to obtain a
// HarmonyOS application permission once the user has already agreed. Both
// happen, in that order.
export interface SitePermissionRequestedEvent extends ShellServiceEvent {
  requestId: number;
  // The site asking, as an origin: "https://map.example.com".
  origin: string;
  // Whether the origin is a secure context. Do not offer "always" when this
  // is false -- a grant written against plain http is a grant to whoever is
  // on the wire, and the engine downgrades one to allowOnce anyway.
  isSecure: boolean;
  // What it is asking for. More than one when a site asks for several at
  // once, for example camera and microphone for a call.
  webPermissions: SitePermission[];
}

export type SitePermission =
  'location' | 'camera' | 'microphone' | 'notifications' | 'clipboard';

export type SitePermissionDecision =
  // Allow this once, remember nothing; the site asks again next time.
  | 'allowOnce'
  // Allow and remember for this site. Downgraded to allowOnce on http.
  | 'allowAlways'
  // Refuse and remember; the site is not asked about again.
  | 'block'
  // The user closed the sheet or pressed back: refuse this time, remember
  // nothing.
  | 'dismiss';

// The engine gave up on a request -- the page navigated away, the tab closed
// -- while its sheet may still be on screen. Take the sheet down; do not
// answer.
export interface SitePermissionCancelledEvent extends ShellServiceEvent {
  requestId: number;
}

export interface BookmarkListEvent extends ShellServiceEvent {
  requestId: number;
  // Empty for the root list and for search results.
  parentId: string;
  nodes: BookmarkNode[];
}

export interface BookmarkCreatedEvent extends ShellServiceEvent {
  requestId: number;
  node: BookmarkNode;
}

// The answer to a bookmark command that changes something, sent only when the
// command carried a requestId. Without it a failed edit is indistinguishable
// from a slow one.
export interface BookmarkOpResultEvent extends ShellServiceEvent {
  requestId: number;
  ok: boolean;
  error?: BookmarkOpError;
  // moveBookmarks and removeBookmarks: the ids that were skipped. The rest of
  // the batch still went through.
  failedIds?: string[];
  // applyBookmarks: the UUIDs that were refused (with error 'someRefused').
  // The rest of the batch still went through.
  failedUuids?: string[];
}

export type BookmarkOpError =
  // The id names nothing -- deleted from another window, typically.
  | 'notFound'
  // One of the three permanent folders, which cannot be edited or removed.
  | 'permanentNode'
  // The target parentId does not exist or is not a folder.
  | 'invalidParent'
  // A folder cannot be moved into itself or its own subtree.
  | 'cycle'
  // Not a valid URL, or a URL was set on a folder.
  | 'invalidUrl'
  // Anything else, including a bookmark managed by policy.
  | 'unknown'
  // applyBookmarks: some ops were refused; see failedUuids.
  | 'someRefused';

// Cloud sync (bookmarkApiVersion 4). Nodes and writes keyed by Chromium's
// bookmark UUID, which is the same on every device, unlike the local id.

// getBookmarksForSync -> bookmarksForSync: every node, the three permanent
// folders included, parents before children and siblings in order. Managed
// bookmarks are left out.
export interface SyncBookmarkNode {
  // Lowercase, 36 characters.
  uuid: string;
  // '' for the three permanent folders.
  parentUuid: string;
  isFolder: boolean;
  title: string;
  // '' for a folder.
  url: string;
  // Position in the parent, from 0.
  index: number;
  // Milliseconds since the epoch.
  dateAdded: number;
  // Only on the three permanent folders, whose UUIDs are fixed: bookmarkBar
  // 0bc5d13f-2cba-5d74-951f-3f233fe6c908, other
  // 82b081ec-3dd3-529c-8475-ab6c344590dd, mobile
  // 4cf2e351-0e85-532b-bb37-df045d8f8d0f.
  rootType?: 'mobile' | 'bookmarkBar' | 'other';
}

export interface BookmarksForSyncEvent extends ShellServiceEvent {
  requestId: number;
  nodes: SyncBookmarkNode[];
}

// applyBookmarks: in array order, in one batch (one bookmarksChanged after).
// upsert creates the node with this UUID when it does not exist, otherwise
// edits it and moves it when the parent or index differ; a parentUuid that
// names nothing puts it in the mobile folder, which is not a failure. A
// different isFolder from the existing node is refused. remove takes a folder
// with everything in it; a UUID that names nothing is not a failure. The
// three permanent folders are only checked, never changed.
export interface SyncBookmarkUpsert {
  op: 'upsert';
  uuid: string;
  parentUuid: string;
  isFolder: boolean;
  title: string;
  url: string;
  index: number;
}

export interface SyncBookmarkRemove {
  op: 'remove';
  uuid: string;
}

export type SyncBookmarkOp = SyncBookmarkUpsert | SyncBookmarkRemove;

// getRecentBookmarks: the bookmarks added most recently, newest first. URLs
// only -- a folder is not somewhere to go back to.
export interface RecentBookmarksEvent extends ShellServiceEvent {
  requestId: number;
  nodes: BookmarkNode[];
}

// getBookmarkPath: the chain from the permanent folder down to the node
// asked for, both ends included; empty when the id names nothing.
export interface BookmarkPathEvent extends ShellServiceEvent {
  requestId: number;
  nodes: BookmarkNode[];
}

// exportBookmarks: the file at the path given has been written. The counts
// leave out the three permanent folders.
export interface BookmarkExportDoneEvent extends ShellServiceEvent {
  requestId: number;
  ok: boolean;
  bookmarkCount: number;
  folderCount: number;
  error?: BookmarkExportError;
}

// The path could not be written: the directory is missing, or the disk is
// full. 'unknown' is anything else.
export type BookmarkExportError = 'writeFailed' | 'unknown';

// importBookmarks: everything in the file has been added under one new
// folder, whose id is folderId. skippedCount counts entries that were not
// bookmarks this browser can hold -- Firefox's place: smart folders, mainly.
// The counts leave out that new folder itself.
export interface BookmarkImportDoneEvent extends ShellServiceEvent {
  requestId: number;
  ok: boolean;
  folderId: string;
  bookmarkCount: number;
  folderCount: number;
  skippedCount: number;
  error?: BookmarkImportError;
}

export type BookmarkImportError =
  // The path does not exist, or could not be read.
  | 'fileNotFound'
  // Readable, but nothing in it parsed as a bookmark.
  | 'notBookmarkFile'
  // Larger than 20 MB, which no real bookmarks file is.
  | 'tooLarge'
  // The parentId to import into does not exist or is not a folder.
  | 'invalidParent'
  | 'unknown';

// bookmarksChanged and historyChanged: something changed; reload what is
// showing. `revision` only ever grows.
export interface ShellDataChangedEvent extends ShellServiceEvent {
  revision: number;
}

export interface HistoryResultsEvent extends ShellServiceEvent {
  requestId: number;
  items: HistoryItem[];
  reachedEnd: boolean;
}

export interface AutocompleteResultsEvent extends ShellServiceEvent {
  requestId: number;
  items: AutocompleteItem[];
  // More updates for this requestId may follow until done is true.
  done: boolean;
}

export interface TopSitesEvent extends ShellServiceEvent {
  requestId: number;
  items: TopSite[];
}

export interface DownloadUpdatedEvent extends ShellServiceEvent, DownloadItem {
}

export interface DownloadListEvent extends ShellServiceEvent {
  requestId: number;
  items: DownloadItem[];
}

export interface ClearBrowsingDataDoneEvent extends ShellServiceEvent {
  requestId: number;
  // False when the request was rejected or some data could not be cleared.
  ok: boolean;
}

export interface BrowsingDataCountsEvent extends ShellServiceEvent {
  requestId: number;
  historyCount: number;
  cacheBytes: number;
  siteCount: number;
}

export interface SearchEnginesEvent extends ShellServiceEvent {
  requestId: number;
  items: SearchEngine[];
}

export interface PrefsEvent extends ShellServiceEvent {
  requestId: number;
  values: ShellPrefValue[];
}

export interface PrefsChangedEvent extends ShellServiceEvent {
  keys: ShellPrefKey[];
}

export interface SiteSettingsEvent extends ShellServiceEvent {
  requestId: number;
  items: SiteSetting[];
}

export interface AboutInfoEvent extends ShellServiceEvent {
  requestId: number;
  chromiumVersion: string;
  engineCommit: string;
  userAgent: string;
  // Which bookmark commands this engine understands: 1 is the original set,
  // 2 adds insertion positions, bookmarkOpResult, childCount, rootType, the
  // batch commands and the two lookups, 3 adds importBookmarks and
  // exportBookmarks, 4 adds getBookmarksForSync and applyBookmarks (by UUID,
  // for cloud sync). Absent means 1. Read it once at startup instead of
  // probing each command with a timeout.
  bookmarkApiVersion?: number;
  // Which password sync commands this engine understands: 1 is
  // passwordSyncAuthorize, getPasswordsForSync, applyPasswords and
  // passwordsChanged. Absent means none.
  passwordApiVersion?: number;
  // Whether the user has authorized password sync on this profile.
  passwordSyncAuthorized?: boolean;
  // Whether V8 runs with its JIT this launch: false when the launch config
  // asked for jitless or the process could not map executable memory.
  jitEnabled?: boolean;
}

export interface FaviconsEvent extends ShellServiceEvent {
  requestId: number;
  items: FaviconImage[];
}

// Reads a runtime event as the interface its `event` name says it is.
// Saved passwords, for a phone's own password pages. No item carries a
// password or a note; those come one at a time in PasswordRevealedEvent,
// inside the window passwordAuthGranted opened.
export interface SavedPasswordItem {
  id: string;
  origin: string;
  signonRealm: string;
  username: string;
  hasNote: boolean;
  // Milliseconds since the epoch; -1 when unknown.
  dateCreated: number;
  dateLastUsed: number;
  isAndroidCredential: boolean;
}

export interface SavedPasswordsEvent extends ShellServiceEvent {
  requestId: number;
  items: SavedPasswordItem[];
}

export interface PasswordRevealedEvent extends ShellServiceEvent {
  requestId: number;
  id: string;
  password: string;
  note: string;
}

export type PasswordCommandFailure =
  'authRequired' | 'notFound' | 'duplicate' | 'ioError' | 'badFormat' |
  'tooLarge' | 'unknown';

export interface PasswordCommandResultEvent extends ShellServiceEvent {
  requestId: number;
  command: 'revealPassword' | 'updatePassword' | 'deletePassword';
  ok: boolean;
  reason?: PasswordCommandFailure;
  // updatePassword: the credential's id from now on, when a new username
  // made it a different credential to the store.
  newId?: string;
}

export interface PasswordsExportedEvent extends ShellServiceEvent {
  requestId: number;
  ok: boolean;
  count: number;
  reason?: PasswordCommandFailure;
}

export interface PasswordsImportedEvent extends ShellServiceEvent {
  requestId: number;
  ok: boolean;
  // Rows now in the store, those that replaced a saved password included.
  imported: number;
  // Rows for a site and username already saved with another password, left
  // as they were because overwrite was not set.
  skipped: number;
  // Rows that could not be read: a missing or bad URL, a missing password,
  // a field too long.
  failed: number;
  reason?: PasswordCommandFailure;
}

// Cloud sync of saved passwords (passwordApiVersion 1). Entries are keyed by
// signonRealm and username, a "never save" entry by signonRealm alone. Only
// the profile's own store, web credentials only: no Android app credentials,
// no passkeys, no federated sign-ins. getPasswordsForSync and applyPasswords
// are refused with 'notAuthorized' until passwordSyncAuthorize {granted: true}.

// getPasswordsForSync -> passwordsForSync: one entry per signonRealm and
// username, the most recently changed when Chromium holds several.
export interface SyncPasswordEntry {
  signonRealm: string;
  // The page the form was on.
  origin: string;
  // '' for a "never save" entry, as are password and note.
  username: string;
  password: string;
  note: string;
  // The user chose "never save" for this site.
  blocked: boolean;
  // Milliseconds since the epoch; -1 when unknown.
  dateCreated: number;
  dateLastUsed: number;
}

export type PasswordSyncError = 'notAuthorized' | 'someRefused' | 'unknown';

export interface PasswordsForSyncEvent extends ShellServiceEvent {
  requestId: number;
  ok: boolean;
  error?: PasswordSyncError;
  entries: SyncPasswordEntry[];
}

// applyPasswords: in array order, told once by passwordsChanged after.
// upsert edits every form saved for the signonRealm and username -- the
// password, the note, a later dateLastUsed -- or creates one with this
// dateCreated; a blocked upsert only makes sure the site is never saved.
// remove deletes them; nothing to delete is not a failure.
export interface SyncPasswordUpsert {
  op: 'upsert';
  signonRealm: string;
  origin: string;
  username: string;
  password: string;
  note: string;
  blocked: boolean;
  dateCreated: number;
  dateLastUsed: number;
}

export interface SyncPasswordRemove {
  op: 'remove';
  signonRealm: string;
  username: string;
  blocked: boolean;
}

export type SyncPasswordOp = SyncPasswordUpsert | SyncPasswordRemove;

export interface PasswordOpResultEvent extends ShellServiceEvent {
  requestId: number;
  ok: boolean;
  error?: PasswordSyncError;
  // someRefused: signonRealm + '\n' + username of each op not written.
  failedKeys?: string[];
}

// Something in the store changed: saved, edited, deleted, imported or
// written by applyPasswords; a burst within 200 ms is told once. Not sent
// when a fill only moved a last-used time, nor while sync is not authorized.
// Carries no entries: read them with getPasswordsForSync.
export interface PasswordsChangedEvent extends ShellServiceEvent {
  // Only rises.
  revision: number;
}

export function shellEvent<T>(event: Object): T {
  return event as T;
}

// A page was opened into a group from Chromium's long-press menu. The reader
// was left on the page they were on, so the shell marks the group's button to
// say something arrived. `id` is the new tab's `id`.
export interface OpenedInGroupEvent {
  event: 'openedInGroup';
  id: string;
}

// Where the reader is in the page, pushed once each time scrolling stops.
// For HarmonyOS app continuation: onContinue is synchronous and wants an
// answer at once, so the shell keeps the last of these rather than asking.
export interface PageScrollSettledEvent {
  event: 'pageScrollSettled';
  url: string;
  scrollX: number;
  scrollY: number;
  pageWidth: number;
  pageHeight: number;
}

// The answer to getPageContinuation, for when the pushed value is too old to
// use. Reading the position means asking the renderer, so it arrives a frame
// or two after the request.
export interface PageContinuationEvent {
  event: 'pageContinuation';
  requestId: number;
  url: string;
  title: string;
  scrollX: number;
  scrollY: number;
  pageWidth: number;
  pageHeight: number;
}

// Where the long-pressed image was written, in answer to the imageToFile
// action. `path` is empty when the image could not be had -- never decoded,
// gone from the cache, or the write failed -- and the shell should say so
// rather than wait. The file lives until the next start.
export interface ContextMenuImageFileEvent {
  event: 'contextMenuImageFile';
  requestId: number;
  path: string;
  mimeType: string;
}

// A context-menu item became available after the menu was already open, so
// the list is sent again. Only "copy link to highlighted text" does this
// today: Chromium adds it disabled and enables it once the renderer has
// generated the text fragment. Replace the list you have; the requestId is
// the same one contextMenuRequested carried.
export interface ContextMenuActionsChangedEvent {
  event: 'contextMenuActionsChanged';
  requestId: number;
  supportedActions: string[];
}

// The answer to handleBack. `handled` true means the page dealt with it --
// it closed a dialog, left fullscreen, ran its own CloseWatcher -- and the
// shell should leave the tab where it is.
export interface BackHandledEvent {
  event: 'backHandled';
  requestId: number;
  handled: boolean;
  // What took it, when handled: 'fullscreen' when the page was fullscreen
  // and has left it, 'browserDialog' when one of the browser's own bubbles
  // or dialogs was open and was closed (an extension's "added" bubble, a
  // permission prompt), 'page' when the page took it.
  by?: 'fullscreen' | 'browserDialog' | 'page';
}

// Pull-to-refresh on a phone. Chromium decides when it starts (the page at
// its top, no inner scroller taking the pull, overscroll-behavior-y auto)
// and reloads the page itself when a release is far enough; the shell only
// draws the indicator. Distances are in vp, past the ~50vp a pull has to
// travel before it starts.
export interface PullToRefreshEvent {
  event: 'pullToRefresh';
  state: 'start' | 'pull' | 'release' | 'reset';
  // start: how far a pull has to reach for a release to refresh.
  threshold?: number;
  // pull: how far the page has been pulled.
  distance?: number;
  // release: true when the page is being reloaded, false when the pull was
  // too short, turned into a scroll, or was cut off (a tab switch).
  refresh?: boolean;
}

// The page made itself fullscreen (requestFullscreen() on a video or any
// other element), or left it. Chromium fills the page area; the shell hides
// its own bars and makes the window immersive, and puts them back when
// `fullscreen` is false. Back leaves fullscreen through handleBack.
export interface PageFullscreenChangedEvent {
  event: 'pageFullscreenChanged';
  fullscreen: boolean;
}
