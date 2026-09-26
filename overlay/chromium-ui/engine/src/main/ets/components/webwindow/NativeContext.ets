/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import { common } from '@kit.AbilityKit';
import { BluetoothBridge, BluetoothNativePort } from '../../utils/BluetoothBridge';
import { UsbBridge, UsbNativePort } from '../../utils/UsbBridge';
import { SystemServiceBridge, SystemServiceNativePort } from '../../utils/SystemServiceBridge';
import { registerSystemServices } from '../../utils/SystemServices';
import { AuraStartupConfig } from '../../utils/DeviceType';
import {
  BrowsingDataTimeRange,
  BrowsingDataType,
  DownloadAction,
  HistoryVisitRef,
  ShellAnchorRect,
  ShellPrefKey,
  SitePermissionDecision,
  SiteSettingType,
  SiteSettingValue
} from './ShellServiceTypes';

export interface WindowRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface PointerEventPayload {
  action: number;
  pointerType: string;
  dispatchSource?: string;
  pointerId: number;
  button: number;
  sourceTool: number;
  x: number;
  y: number;
  rootX?: number;
  rootY?: number;
  rootWindowRelative?: boolean;
  deltaX: number;
  deltaY: number;
  timestamp: number;
}

export interface KeyEventPayload {
  action: number;
  keyCode: number;
  keyText: string;
  dispatchSource?: string;
  sourceTool: number;
  timestamp: number;
}

export interface BrowserTabState {
  index: number;
  active: boolean;
  url: string;
  title: string;
  loading: boolean;
  // Stable for the tab's life and never reused, unlike index, which shifts
  // whenever a neighbour closes or moves. Name the tab by this in anything
  // asynchronous -- a thumbnail arriving, a drag finishing. Empty on an
  // engine older than browsingApiVersion 1.
  id?: string;
  // Making a sound right now. Separate from muted: a tab can be silenced
  // before it ever makes one.
  audible?: boolean;
  // Silenced by the user.
  muted?: boolean;
  // The group this tab belongs to, or '' when it belongs to none. A group is
  // a page and the pages it opened; on a phone the engine makes one whenever
  // a page opens another, and on a tablet or a PC it never does. Tabs in a
  // group are always next to each other and in the order they were opened.
  // Empty on an engine older than browsingApiVersion 2.
  groupId?: string;
  // The `id` of the tab that opened this one, or '' when nothing did or when
  // that tab has since closed. Survives a restart.
  openerId?: string;
}

export type PwaMenuItemType = 'command' | 'check' | 'radio' | 'separator' |
  'buttonRow' | 'submenu' | 'actionableSubmenu' | 'highlighted' | 'title';

export interface PwaMenuButtonState {
  type: 'space' | 'button' | 'label';
  commandId: number;
  label: string;
  enabled: boolean;
  dismissesMenu: boolean;
}

export interface PwaMenuItemState {
  type: PwaMenuItemType;
  commandId: number;
  label: string;
  secondaryLabel: string;
  minorText: string;
  enabled: boolean;
  checked: boolean;
  groupId: number;
  separatorType?: number;
  children?: PwaMenuItemState[];
  buttons?: PwaMenuButtonState[];
}

export interface BrowserStateSnapshot {
  version: number;
  mobileUi: boolean;
  uiFamily: string;
  ready: boolean;
  tabCount: number;
  activeTabIndex: number;
  sidePanelVisible: boolean;
  sidePanelEntryId: string;
  focusedTarget: 'webContents' | 'sidePanel';
  activationEpoch: number;
  tabs: BrowserTabState[];
  // 0..1, and 1 whenever nothing is loading, so one progress bar needs no
  // special case for the idle state.
  loadProgress?: number;
  // Whether the current tab is asking sites for their desktop pages.
  requestDesktopSite?: boolean;
  // Page zoom of the current tab, as a percentage; 100 is normal.
  zoomPercent?: number;
  // Whether the current tab is showing a distilled page.
  inReaderMode?: boolean;
  // Whether the distiller thinks the current page is an article. Drives
  // whether the menu offers reader mode at all.
  readerModeAvailable?: boolean;
  url: string;
  domain: string;
  title: string;
  loading: boolean;
  canGoBack: boolean;
  canGoForward: boolean;
  isPwaWindow: boolean;
  pwaAppId: string;
  pwaStartUrl: string;
}

// What a long-press menu can offer. contextMenuRequested lists the ones that
// apply in supportedActions; the shell sends one back in contextMenuAction.
export type ContextMenuAction =
  'openLinkInNewTab' | 'openLinkInIncognito' | 'copyLinkAddress' |
  'copyLinkText' | 'saveLinkAs' | 'copyLinkToHighlight' |
  'openImageInNewTab' | 'saveImageAs' | 'copyImage' | 'copyImageAddress' |
  'toggleLoop' | 'toggleControls' | 'openMediaInNewTab' | 'saveMediaAs' |
  'copyMediaAddress' | 'copyVideoFrame' | 'pictureInPicture' |
  'copy' | 'cut' | 'paste' | 'pasteAsPlainText' | 'selectAll' | 'delete' |
  'undo' | 'redo' | 'searchSelection' |
  // Passwords. suggestPassword offers a generated one on a new-password
  // field; selectSavedPassword opens the saved list through the ordinary
  // autofill popup, which asks for the reader's identity first when
  // passwordFillRequiresAuth is on.
  'suggestPassword' | 'selectSavedPassword' | 'managePasswords' |
  // Spelling. The numbered ones line up with spellSuggestions, and there are
  // never more than five.
  'spellSuggestion:0' | 'spellSuggestion:1' | 'spellSuggestion:2' |
  'spellSuggestion:3' | 'spellSuggestion:4' | 'addToDictionary' |
  // No Chromium command behind these two: the shell does the sharing itself,
  // and imageToFile answers with contextMenuImageFile rather than doing
  // anything to the page.
  'shareLink' | 'imageToFile';

// The long-pressed video or audio element's state.
export interface ContextMenuMediaFlags {
  paused: boolean;
  muted: boolean;
  loop: boolean;
  canLoop: boolean;
  controls: boolean;
  canToggleControls: boolean;
  canSave: boolean;
  hasAudio: boolean;
  inError: boolean;
}

export interface BrowserRuntimeEvent {
  version: number;
  event: 'systemPrintReady' | 'systemPrintFailed' | 'shareRequested' |
    'castRequested' | 'filePickerRequested' | 'pwaInstalled' | 'pwaMenuModel' |
    'pwaMenuClosed' | 'inputRecovered' | 'systemActionRequested' |
    'permissionsRequested' | 'sitePermissionRequested' |
    'shellAccelerator' | 'linkHovered' | 'tabThumbnails' |
    'openedInGroup' | 'pageScrollSettled' | 'pageContinuation' |
    'contextMenuImageFile' | 'contextMenuActionsChanged' |
    'backHandled' |
    'recentlyClosed' | 'recentlyClosedChanged' | 'extensionActions' |
    'recentBookmarks' |
    'extensionActionsChanged' |
    'sitePermissionCancelled' |
    'browserControlsRatio' | 'findResult' | 'pageText' |
    'contextMenuRequested' |
    // Profile data; read these through shellEvent<T>() (ShellServiceTypes).
    'bookmarkList' | 'bookmarkCreated' | 'bookmarksChanged' |
    'historyResults' | 'historyChanged' | 'autocompleteResults' | 'topSites' |
    'downloadUpdated' | 'downloadList' |
    'clearBrowsingDataDone' | 'browsingDataCounts' | 'searchEngines' |
    'prefs' | 'prefsChanged' | 'siteSettings' | 'aboutInfo' | 'favicons';
  // contextMenuRequested: where the press was, in vp in the window.
  x?: number;
  y?: number;
  // contextMenuRequested: the page context. Empty strings when absent.
  linkUrl?: string;
  linkText?: string;
  srcUrl?: string;
  mediaType?: 'image' | 'video' | 'audio' | 'none';
  mediaFlags?: ContextMenuMediaFlags;
  selectionText?: string;
  isEditable?: boolean;
  frameUrl?: string;
  pageUrl?: string;
  incognito?: boolean;
  supportedActions?: ContextMenuAction[];
  // contextMenuRequested, spelling: the word the spellchecker objected to and
  // what it suggests instead, at most five. Both absent unless the renderer
  // found something -- the spellchecker here is Hunspell and there is no
  // Hunspell dictionary for Chinese in any Chromium build, so on a zh-CN
  // device they never appear.
  misspelledWord?: string;
  spellSuggestions?: string[];
  // openedInGroup: the `id` of the tab that was just opened into the group.
  // The reader was left on the page they were on.
  id?: string;
  // backHandled: whether the page took the back gesture -- closed a dialog,
  // left fullscreen. True means the shell should not also go back.
  handled?: boolean;
  // contextMenuImageFile: where the long-pressed image was written, and what
  // it is. Both empty when there was no image to write.
  path?: string;
  mimeType?: string;
  // pageScrollSettled and pageContinuation: where the reader is in the page,
  // in CSS pixels, and how big the document is. Divide to get a fraction that
  // still means something on a screen of another width.
  scrollX?: number;
  scrollY?: number;
  pageWidth?: number;
  pageHeight?: number;
  // browserControlsRatio: how much of the shell's top bar the page shows, 0-1.
  ratio?: number;
  // findResult: the match count, the active match (1-based; 0 for none) and
  // whether this is the last update for the search.
  matches?: number;
  activeMatch?: number;
  finalUpdate?: boolean;
  // pageText: the page's visible text, at most 20000 characters.
  text?: string;
  action?: 'huaweiAccount' | 'huaweiWallet' | 'defaultBrowser' | 'checkForUpdates';
  filePath?: string;
  appId?: string;
  title?: string;
  url?: string;
  pwaStartUrl?: string;
  reason?: string;
  requestId?: number;
  permissions?: string[];
  pickerType?: 'open' | 'openMultiple' | 'save' | 'folder' | 'none';
  defaultName?: string;
  suffixFilters?: string[];
  fileTypeIndex?: number;
  presentation?: 'popup' | 'sheet';
  menuSessionId?: number;
  items?: PwaMenuItemState[];
  activated?: boolean;
  activationEpoch?: number;
  focusedTarget?: 'webContents' | 'sidePanel';
  sidePanelEntryId?: string;
  known?: boolean;
  isDefault?: boolean;
  huaweiWalletAvailable?: boolean;
}

export interface BrowserCommandPayload {
  command: string;
  url?: string;
  // contextMenuAction: the item chosen, one of the event's supportedActions.
  // downloadAction: what to do with the download `id`.
  action?: ContextMenuAction | DownloadAction;
  // Profile data commands (docs/SHELL_CONTRACT.md, "Profile data").
  // setDownloadDirectory: the directory URI from ShellDownloadDirectory.
  uri?: string;
  id?: string;
  parentId?: string;
  title?: string;
  query?: string;
  maxCount?: number;
  beforeTime?: number;
  beginTime?: number;
  endTime?: number;
  count?: number;
  items?: HistoryVisitRef[];
  types?: BrowsingDataType[];
  timeRange?: BrowsingDataTimeRange;
  keys?: ShellPrefKey[];
  key?: ShellPrefKey;
  value?: boolean | number | string;
  origin?: string;
  type?: SiteSettingType;
  setting?: SiteSettingValue;
  urls?: string[];
  // moveBookmarks and removeBookmarks: the bookmarks to act on, in order.
  // groupTabs: the tabs to put in one group, in order.
  ids?: string[];
  // groupTabs: for each tab in `ids`, the tab that opened it, or '' for none.
  // Same length as `ids` or it is ignored. Only needed after a restart, when
  // the engine cannot know: it tracks openers by session id and these tabs
  // are new.
  openerIds?: string[];
  // importBookmarks and exportBookmarks: an absolute path inside the app
  // sandbox. The shell owns the system file picker and the copy in or out;
  // the engine only reads and writes this path.
  path?: string;
  // moveTab: where the tab should end up, counting positions after the move.
  toIndex?: number;
  // setRequestDesktopSite: ask this tab's site for its desktop pages.
  enabled?: boolean;
  // setTabMuted: silence this tab, or let it speak again.
  muted?: boolean;
  // setZoom: the page zoom to apply, 25 to 500. Omitted means back to 100.
  percent?: number;
  // newTab: open into this group, at its end, opened by the current tab.
  // Omitted opens an ordinary tab outside any group.
  groupId?: string;
  // newTab: leave the reader where they are. Default false.
  background?: boolean;
  // newTab: how far down the document the reader was on the device they came
  // from, 0 to 1. Applied once, when the page has finished loading, and not at
  // all if they scroll first.
  scrollRatio?: number;
  // closeTabById: after closing, go back to the tab that opened it, if that
  // tab is still open. Omitted leaves the choice to Chromium's own rule.
  returnToOpener?: boolean;
  // getTabThumbnails: how wide the grid's cards are, so the capture is not
  // bigger than it needs to be.
  widthVp?: number;
  // setAnchorRects: where the shell drew the buttons Chromium's bubbles point
  // at, in this window's coordinates.
  anchors?: ShellAnchorRect[];
  // setExtensionPinned.
  pinned?: boolean;
  // setBrowserChrome: who draws the browser from now on.
  mode?: 'shell' | 'native';
  // sitePermissionDecision: the answer to sitePermissionRequested.
  decision?: SitePermissionDecision;
  sizeVp?: number;
  // setViewportInsets: how much of the page bottom the shell covers, in vp.
  bottom?: number;
  // setBrowserControls: the shell's top bar height, in vp; 0 for none.
  top?: number;
  // setBrowserControls: how much of the top bar stays on screen once it is
  // fully collapsed, in vp; omitted or 0 lets it hide completely.
  minTop?: number;
  // setBrowserControlsState: which state to drive the controls to.
  state?: 'shown' | 'hidden' | 'both';
  // setBrowserControlsState: whether the transition animates.
  animate?: boolean;
  // findInPage: the text to find, and the direction to step in.
  // insertText: what to put where the reader is typing. The shell reads the
  // system pasteboard with a paste control and sends it here, because
  // Chromium cannot read it -- that needs READ_PASTEBOARD, a restricted
  // permission this app does not declare.
  text?: string;
  forward?: boolean;
  index?: number;
  requestId?: number;
  paths?: string[];
  fileTypeIndex?: number;
  canceled?: boolean;
  granted?: string[];
  denied?: string[];
  location?: 'allowed' | 'denied' | 'notDetermined';
  presentation?: 'popup' | 'sheet';
  anchorX?: number;
  anchorY?: number;
  anchorWidth?: number;
  anchorHeight?: number;
  menuSessionId?: number;
  commandId?: number;
  eventFlags?: number;
  activationEpoch?: number;
  focusedTarget?: 'webContents' | 'sidePanel';
  sidePanelEntryId?: string;
}

export interface AuxiliaryWindowState {
  version: number;
  widget: number;
  componentId: string;
  x: number;
  y: number;
  width: number;
  height: number;
  visible: boolean;
  destroyed: boolean;
  stackingOrder: number;
  // Whether Chromium made this window modal. Absent when the engine could not
  // tell -- an older engine, or a report sent before the widget had a
  // delegate -- in which case the last known answer stands.
  modal?: boolean;
  // A popup, menu or tooltip Chromium placed next to page content: draw it
  // at x/y, not where the shell would put a window of its own.
  anchored?: boolean;
  // Chromium's picture-in-picture window. It floats over the page with no
  // close button of its own, which is also what a page's window.open() popup
  // looks like -- but back should close the popup and leave the video alone,
  // and the video wants a corner rather than the middle. Absent on an older
  // engine and on every window that is not one.
  pictureInPicture?: boolean;
  windowRole: 'auxiliary' | 'pwa' | 'browser';
  pwaAppId?: string;
  title?: string;
  url?: string;
  pwaStartUrl?: string;
  // Sent with a 'browser' event: whether the window's profile is incognito.
  incognito?: boolean;
}

export interface INativeContext extends BluetoothNativePort, UsbNativePort, SystemServiceNativePort {
  Initialize(xComponentId: string, configJson: string, onWindowAction: (action: string) => void): void;
  OnSurfaceCreated(xComponentId: string): void;
  OnSurfaceChanged(
    xComponentId: string,
    x: number,
    y: number,
    width: number,
    height: number,
    density: number
  ): void;
  OnSurfaceDestroyed(xComponentId: string): void;
  OnVisibilityChanged(xComponentId: string, visible: boolean): void;
  OnFocusChanged(xComponentId: string, focused: boolean): void;
  OnWindowIdChanged(xComponentId: string, windowId: number): void;
  OnThemeFontChanged(xComponentId: string, fontId: string): void;
  DispatchPointerEvent(xComponentId: string, eventJson: string): void;
  DispatchKeyEvent(xComponentId: string, eventJson: string): boolean;
  Navigate(xComponentId: string, url: string): void;
  SetBrowserEventCallback(xComponentId: string, callback: (stateJson: string) => void): void;
  SetBluetoothEventCallback(xComponentId: string, callback: (commandJson: string) => void): void;
  CompleteBluetoothMessage(xComponentId: string, messageJson: string): void;
  SetUsbEventCallback(xComponentId: string, callback: (commandJson: string) => void): void;
  CompleteUsbMessage(xComponentId: string, messageJson: string): void;
  ExecuteBrowserCommand(xComponentId: string, commandJson: string): boolean;
  SetAuxiliaryWindowEventCallback(xComponentId: string, callback: (stateJson: string) => void): void;
  DismissAuxiliaryWindow(xComponentId: string, widget: number): boolean;
  Shutdown(xComponentId: string): void;
}

export class NativeContext {
  private readonly xComponentId: string;
  private nativeContext: INativeContext | null = null;
  private bluetoothBridge: BluetoothBridge | null = null;
  private usbBridge: UsbBridge | null = null;
  private systemServiceBridge: SystemServiceBridge | null = null;
  private onWindowAction: (action: string) => void = (_action: string): void => {};
  private onBrowserState: (stateJson: string) => void = (_stateJson: string): void => {};
  private onAuxiliaryWindowState: (stateJson: string) => void = (_stateJson: string): void => {};
  // Set by callVoid when a native call is skipped or throws. Read by the caller
  // to tell "engine never came up" apart from "engine is up but idle".
  private lastError: string | null = null;

  takeLastError(): string | null {
    const e = this.lastError;
    this.lastError = null;
    return e;
  }

  constructor(xComponentId: string) {
    this.xComponentId = xComponentId;
  }

  init(
    context: INativeContext,
    abilityContext: common.UIAbilityContext,
    config: AuraStartupConfig,
    onWindowAction: (action: string) => void,
    onBrowserState: (stateJson: string) => void,
    onAuxiliaryWindowState: (stateJson: string) => void
  ): void {
    this.nativeContext = context;
    this.onWindowAction = onWindowAction;
    this.onBrowserState = onBrowserState;
    this.onAuxiliaryWindowState = onAuxiliaryWindowState;
    this.bluetoothBridge = new BluetoothBridge(abilityContext, this.xComponentId, context);
    this.usbBridge = new UsbBridge(this.xComponentId, context);
    this.systemServiceBridge = new SystemServiceBridge(this.xComponentId, context);
    registerSystemServices(this.systemServiceBridge, abilityContext);
    this.callVoid('Initialize', () => {
      this.nativeContext!.Initialize(this.xComponentId, JSON.stringify(config), this.onWindowAction);
      this.nativeContext!.SetBrowserEventCallback(this.xComponentId, this.onBrowserState);
      this.nativeContext!.SetAuxiliaryWindowEventCallback(this.xComponentId, this.onAuxiliaryWindowState);
    });
  }

  UpdateConfiguration(config: AuraStartupConfig): void {
    this.callVoid('UpdateConfiguration', () => {
      this.nativeContext!.Initialize(this.xComponentId, JSON.stringify(config), this.onWindowAction);
    });
  }

  OnSurfaceCreated(): void {
    this.callVoid('OnSurfaceCreated', () => {
      this.nativeContext!.OnSurfaceCreated(this.xComponentId);
    });
  }

  OnSurfaceChanged(rect: WindowRect, density: number): void {
    if (rect.width <= 0 || rect.height <= 0) {
      return;
    }

    this.callVoid('OnSurfaceChanged', () => {
      this.nativeContext!.OnSurfaceChanged(
        this.xComponentId,
        rect.x,
        rect.y,
        rect.width,
        rect.height,
        density
      );
    });
  }

  OnSurfaceDestroyed(): void {
    this.callVoid('OnSurfaceDestroyed', () => {
      this.nativeContext!.OnSurfaceDestroyed(this.xComponentId);
    });
  }

  OnVisibilityChanged(visible: boolean): void {
    this.callVoid('OnVisibilityChanged', () => {
      this.nativeContext!.OnVisibilityChanged(this.xComponentId, visible);
    });
  }

  OnFocusChanged(focused: boolean): void {
    this.callVoid('OnFocusChanged', () => {
      this.nativeContext!.OnFocusChanged(this.xComponentId, focused);
    });
  }

  OnWindowIdChanged(windowId: number): void {
    this.callVoid('OnWindowIdChanged', () => {
      this.nativeContext!.OnWindowIdChanged(this.xComponentId, windowId);
    });
  }

  OnThemeFontChanged(fontId: string): void {
    this.callVoid('OnThemeFontChanged', () => {
      this.nativeContext!.OnThemeFontChanged(this.xComponentId, fontId);
    });
  }

  DispatchPointerEvent(event: PointerEventPayload): void {
    this.callVoid('DispatchPointerEvent', () => {
      this.nativeContext!.DispatchPointerEvent(this.xComponentId, JSON.stringify(event));
    });
  }

  DispatchKeyEvent(event: KeyEventPayload): boolean {
    if (!this.nativeContext) {
      return false;
    }

    try {
      return this.nativeContext.DispatchKeyEvent(this.xComponentId, JSON.stringify(event));
    } catch (error) {
      console.error('DispatchKeyEvent failed:', error);
      return false;
    }
  }

  Navigate(url: string): void {
    if (!url || url.trim().length === 0) {
      return;
    }

    this.callVoid('Navigate', () => {
      this.nativeContext!.Navigate(this.xComponentId, url);
    });
  }

  ExecuteBrowserCommand(commandJson: string): boolean {
    if (!this.nativeContext || !commandJson || commandJson.trim().length === 0) {
      return false;
    }

    try {
      return this.nativeContext.ExecuteBrowserCommand(this.xComponentId, commandJson);
    } catch (error) {
      console.error('ExecuteBrowserCommand failed:', error);
      return false;
    }
  }

  DismissAuxiliaryWindow(widget: number): boolean {
    if (!this.nativeContext || widget <= 0) {
      return false;
    }

    try {
      return this.nativeContext.DismissAuxiliaryWindow(this.xComponentId, widget);
    } catch (error) {
      console.error('DismissAuxiliaryWindow failed:', error);
      return false;
    }
  }

  destroy(): void {
    if (!this.nativeContext) {
      return;
    }

    this.bluetoothBridge?.destroy();
    this.bluetoothBridge = null;
    this.usbBridge?.destroy();
    this.usbBridge = null;
    this.callVoid('Shutdown', () => {
      this.nativeContext!.Shutdown(this.xComponentId);
    });
    this.nativeContext = null;
    this.onWindowAction = (_action: string): void => {};
    this.onBrowserState = (_stateJson: string): void => {};
    this.onAuxiliaryWindowState = (_stateJson: string): void => {};
  }

  isInitialized(): boolean {
    return this.nativeContext !== null;
  }

  private callVoid(name: string, invoke: () => void): void {
    if (!this.nativeContext) {
      console.error(`${name} skipped because native context is not initialized`);
      this.lastError = `${name}: native context missing`;
      return;
    }

    try {
      invoke();
    } catch (error) {
      console.error(`${name} failed:`, error);
      const e = error as Error;
      this.lastError = `${name}: ${e && e.message ? e.message : String(error)}`;
    }
  }
}
