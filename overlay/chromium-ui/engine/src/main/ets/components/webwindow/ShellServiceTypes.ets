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
  url: string;
  isDefault: boolean;
}

// The preferences a shell may read and write. Anything else is ignored.
export type ShellPrefKey =
  'blockThirdPartyCookies' | 'doNotTrack' | 'safeBrowsing' | 'preloadPages' |
  'popupsBlocked' | 'javascriptEnabled' | 'textScale' | 'autofillAddresses' |
  'autofillCards' | 'downloadAskWhereToSave';

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
  | 'unknown';

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
  // exportBookmarks. Absent means 1. Read it once at startup instead of
  // probing each command with a timeout.
  bookmarkApiVersion?: number;
}

export interface FaviconsEvent extends ShellServiceEvent {
  requestId: number;
  items: FaviconImage[];
}

// Reads a runtime event as the interface its `event` name says it is.
export function shellEvent<T>(event: Object): T {
  return event as T;
}
