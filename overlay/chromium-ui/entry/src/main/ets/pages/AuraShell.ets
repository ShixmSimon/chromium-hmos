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
import { WebWindow } from 'engine';
import { MobileBrowserOverlay } from '../components/mobile/MobileBrowserOverlay';
import { CurrentTabCastSession } from '../components/mobile/CurrentTabCastSession';
import { showContextMenuSheet } from '../components/mobile/ContextMenuSheet';
import { BrowserOnboarding } from '../components/onboarding/BrowserOnboarding';
import {
  AuxiliaryWindowState,
  BrowserCommandPayload,
  BrowserRuntimeEvent,
  BrowserStateSnapshot
} from 'engine';
import { CHROMIUM_HOME_URL } from 'engine';
import { HuaweiKnockShareSession, HuaweiShareAdapter } from 'engine';
import { HarmonyFilePickerAdapter, HarmonyFilePickerResult } from 'engine';
import { PermissionRequestAdapter, PermissionRequestResultPayload } from 'engine';
import {
  BrowserCommandChannel,
  DownloadNotifier,
  DownloadUpdatedEvent,
  ShellDownloadDirectory,
  shellEvent
} from 'engine';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { SystemPrintAdapter } from 'engine';
import { ChromiumSystemAction, SystemIntegrationAdapter } from 'engine';
import { PwaWindowLauncher } from '../utils/PwaWindowLauncher';
import {
  AUXILIARY_WINDOW_MARGIN_VP,
  AuxiliaryWindowView,
  layoutAuxiliaryWindow,
  ScreenSizeVp
} from 'engine';
import { BrowserProfileState } from '../utils/profile/BrowserProfileService';

const EMPTY_BROWSER_STATE: BrowserStateSnapshot = {
  version: 1,
  mobileUi: false,
  uiFamily: '',
  ready: false,
  tabCount: 0,
  activeTabIndex: -1,
  sidePanelVisible: false,
  sidePanelEntryId: '',
  focusedTarget: 'webContents',
  activationEpoch: 0,
  tabs: [],
  url: CHROMIUM_HOME_URL,
  domain: '',
  title: 'Chromium',
  loading: false,
  canGoBack: false,
  canGoForward: false,
  isPwaWindow: false,
  pwaAppId: '',
  pwaStartUrl: ''
};

@Entry
@Component
struct AuraShell {
  @StorageLink('auraRoutingRevision') routingRevision: number = 0;
  @StorageLink('auraStartupConfig') startupConfigText: string = '';
  @StorageLink('auraStartUrl') startUrl: string = CHROMIUM_HOME_URL;
  @StorageLink('auraAppVisible') appVisible: boolean = true;
  @StorageLink('auraThemeFontId') themeFontId: string = '';
  @StorageLink('auraThemeFontRevision') themeFontRevision: number = 0;
  @StorageLink('auraOnboardingReady') onboardingReady: boolean = false;
  @StorageLink('auraOnboardingComplete') onboardingComplete: boolean = false;
  @StorageLink('auraProfileId') profileId: string = 'local';
  @StorageLink('auraProfileKind') profileKind: string = 'local';
  @StorageLink('auraSyncMode') syncMode: string = 'off';
  @State private browserState: BrowserStateSnapshot = EMPTY_BROWSER_STATE;
  @StorageLink('auraBrowserCommandText') browserCommandText: string = '';
  @StorageLink('auraBrowserCommandRevision') browserCommandRevision: number = 0;
  @State private auxiliaryWindows: AuxiliaryWindowView[] = [];
  @State private screenSizeVp: ScreenSizeVp = { width: 0, height: 0 };
  @State private activeAuxiliaryWidget: number = 0;
  @State private auxiliaryDismissWidget: number = 0;
  @State private auxiliaryDismissRevision: number = 0;
  private readonly currentTabCastSession: CurrentTabCastSession = new CurrentTabCastSession();
  private readonly knockShareSession: HuaweiKnockShareSession = new HuaweiKnockShareSession();
  private launchingPwaWidgets: number[] = [];

  aboutToAppear(): void {
    this.startKnockShare();
    this.useSystemDownloadDirectory();
  }

  // Downloads go to Download/<bundle>, where the Files app shows them. The
  // directory can only be had once this page is up, after Chromium started.
  private downloadNotifier: DownloadNotifier | undefined = undefined;

  private useSystemDownloadDirectory(): void {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    this.downloadNotifier = new DownloadNotifier(context);
    ShellDownloadDirectory.prepare(context).then((uri: string): void => {
      if (uri.length > 0) {
        this.executeBrowserCommand({ command: 'setDownloadDirectory', uri: uri });
      }
    });
  }

  onPageShow(): void {
    this.startKnockShare();
  }

  onPageHide(): void {
    this.knockShareSession.stop();
  }

  aboutToDisappear(): void {
    PermissionRequestAdapter.stopObservingLocation();
    this.knockShareSession.stop();
    this.currentTabCastSession.stop().catch((error: Error): void => {
      console.error('Failed to stop current-tab cast session:', error);
    });
  }

  build() {
    Stack({ alignContent: Alignment.Bottom }) {
      if (!this.onboardingReady) {
        LoadingProgress()
          .width(40)
          .height(40)
          .color($r('app.color.accent_primary'))
          .align(Alignment.Center)
      } else if (!this.onboardingComplete) {
        BrowserOnboarding({
          onComplete: (state: BrowserProfileState): void => {
            this.profileId = state.profileId;
            this.profileKind = state.profileKind;
            this.syncMode = state.syncMode;
            this.onboardingComplete = state.onboardingComplete;
          }
        })
      } else {
        WebWindow({
        url: this.startUrl,
        profileId: this.profileId,
        startupConfigText: this.startupConfigText,
        routingRevision: this.routingRevision,
        appVisible: this.appVisible,
        themeFontId: this.themeFontId,
        themeFontRevision: this.themeFontRevision,
        browserCommandText: this.browserCommandText,
        browserCommandRevision: this.browserCommandRevision,
        auxiliaryDismissWidget: this.auxiliaryDismissWidget,
        auxiliaryDismissRevision: this.auxiliaryDismissRevision,
        onBrowserState: (state: BrowserStateSnapshot): void => {
          this.browserState = state;
          if (!this.systemPermissionReported) {
            this.systemPermissionReported = true;
            this.reportSystemPermissionState();
          }
          this.currentTabCastSession.update(state);
          this.knockShareSession.update(state);
        },
        onRuntimeEvent: (event: BrowserRuntimeEvent): void => {
          this.handleRuntimeEvent(event);
        },
        onAuxiliaryWindowState: (state: AuxiliaryWindowState): void => {
          this.handleAuxiliaryWindowState(state);
        }
        })
          .width('100%')
          .height('100%')
        if (this.isMobilePhoneUi()) {
          MobileBrowserOverlay({
            browserState: this.browserState,
            onCommand: (command: BrowserCommandPayload): void => {
              // Back is asked of the page before it is asked of the tab. A
              // <dialog>, a popover, fullscreen or a page with a CloseWatcher
              // of its own expects back to close that rather than leave the
              // page, which is what back does in every other browser. The
              // engine answers backHandled; only a no goes on to the tab.
              if (command.command === 'back') {
                this.executeBrowserCommand({
                  command: 'handleBack', requestId: ++this.backRequestId });
                return;
              }
              this.executeBrowserCommand(command);
            }
          })
        }
        if (this.auxiliaryWindows.length > 0) {
          this.AuxiliaryWindowLayer()
        }
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.browser_canvas'))
    .onAreaChange((_oldValue: Area, newValue: Area): void => {
      this.onScreenSizeChanged(Number(newValue.width), Number(newValue.height));
    })
  }

  // Close the topmost modal window on back, the way a phone dismisses a
  // dialog. Chromium's windows carry their own Cancel and Stop buttons, so
  // the shell no longer adds a close button of its own; this is the one
  // system gesture that still means "dismiss".
  onBackPress(): boolean {
    for (let index = this.auxiliaryWindows.length - 1; index >= 0; index--) {
      const view = this.auxiliaryWindows[index];
      if (view.modal) {
        this.requestAuxiliaryWindowDismiss(view.state.widget);
        return true;
      }
    }
    return false;
  }

  private startKnockShare(): void {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext | undefined;
    if (!context) {
      return;
    }
    this.knockShareSession.start(context).catch((error: Error): void => {
      console.error(`Failed to start HarmonyOS knock share: ${error.message}`);
    });
  }

  // Each auxiliary window is drawn at its own size, clamped to the screen, and
  // hangs from the top centre. Nothing wraps the XComponent: its box is exactly
  // the size the window reported, because the surface size is fed back to the
  // widget as its bounds and anything added here would shrink it every round.
  //
  // Only a modal window -- the media source picker -- gets a scrim that blocks
  // the page. A non-modal one, such as the "sharing your screen / stop" bar,
  // covers only its own box and the page around it stays usable.
  @Builder
  private AuxiliaryWindowLayer() {
    if (this.auxiliaryWindows.some((view: AuxiliaryWindowView): boolean => view.modal)) {
      Column()
        .width('100%')
        .height('100%')
        .backgroundColor($r('app.color.modal_scrim'))
        .hitTestBehavior(HitTestMode.Block)
    }
    ForEach(this.auxiliaryWindows, (view: AuxiliaryWindowView) => {
      Stack() {
        XComponent({
          id: view.state.componentId,
          type: XComponentType.SURFACE,
          libraryname: 'web_engine'
        })
          .id(view.state.componentId)
          .width('100%')
          .height('100%')
          // A SURFACE XComponent's own background is opaque black by default,
          // and it shows through wherever the buffer is transparent. Popups
          // paint their margin transparent (rounded corners, the arrow, the
          // shadow), so without this each one sat in a black rectangle.
          .backgroundColor(Color.Transparent)
          // A popup anchored to the page -- the autofill list under a field
          // -- is not activatable in Chromium either. Taking focus here moved
          // it off the page's XComponent, which closed the keyboard the user
          // was typing with and could leave it unable to reopen.
          .focusable(view.state.anchored !== true)
          .defaultFocus(view.state.widget === this.activeAuxiliaryWidget)
      }
      // Read through this.auxiliaryWindows rather than `view`: the item is keyed
      // by component id so that a resize does not recreate the surface, which
      // means the builder keeps the `view` it was first given.
      .width(this.auxiliaryWidthVp(view.state.widget))
      .height(this.auxiliaryHeightVp(view.state.widget))
      .position({
        x: this.auxiliaryLeftVp(view.state.widget),
        y: this.auxiliaryTopVp(view.state.widget)
      })
      // Popups Chromium anchors to the page draw their own shadow and border.
      .shadow(view.state.anchored === true ? { radius: 0 } :
        { radius: 16, color: $r('app.color.shadow'), offsetY: 4 })
      // Default, not Block: Block keeps the touch test away from this Stack's
      // own children, and the Stack is not a touch target itself, so every
      // press inside the window came back "Touch test result is empty" and
      // never reached the XComponent -- the Stop button did nothing. Default
      // lets the XComponent take the press and still keeps it from the page
      // beneath this box.
    }, (view: AuxiliaryWindowView): string => view.state.componentId)
  }

  private findAuxiliaryWindow(widget: number): AuxiliaryWindowView | undefined {
    return this.auxiliaryWindows.find(
      (view: AuxiliaryWindowView): boolean => view.state.widget === widget);
  }

  private auxiliaryWidthVp(widget: number): number {
    return this.findAuxiliaryWindow(widget)?.widthVp ?? 0;
  }

  private auxiliaryHeightVp(widget: number): number {
    return this.findAuxiliaryWindow(widget)?.heightVp ?? 0;
  }

  private auxiliaryLeftVp(widget: number): number {
    return this.findAuxiliaryWindow(widget)?.leftVp ?? 0;
  }

  private auxiliaryTopVp(widget: number): number {
    return this.findAuxiliaryWindow(widget)?.topVp ?? AUXILIARY_WINDOW_MARGIN_VP;
  }

  private onScreenSizeChanged(width: number, height: number): void {
    if (width <= 0 || height <= 0 ||
      (width === this.screenSizeVp.width && height === this.screenSizeVp.height)) {
      return;
    }
    this.screenSizeVp = { width: width, height: height };
    // A rotation or a window resize changes the clamp, so every window is laid
    // out again against the new screen.
    this.auxiliaryWindows = this.auxiliaryWindows.map(
      (view: AuxiliaryWindowView): AuxiliaryWindowView => this.layoutAuxiliary(view.state, view.modal, view));
  }

  private layoutAuxiliary(
    state: AuxiliaryWindowState,
    modal: boolean,
    previous?: AuxiliaryWindowView
  ): AuxiliaryWindowView {
    const ui = this.getUIContext();
    // Before the first layout pass there is no screen size yet; the window's
    // own size stands in, and onScreenSizeChanged clamps it once one arrives.
    const screen: ScreenSizeVp = this.screenSizeVp.width > 0 ? this.screenSizeVp : {
      width: ui.px2vp(state.width) + 2 * AUXILIARY_WINDOW_MARGIN_VP,
      height: ui.px2vp(state.height) * 2
    };
    return layoutAuxiliaryWindow(state, modal, screen,
      (px: number): number => ui.px2vp(px), previous);
  }

  // Commands are batched rather than written straight to the state pair, so
  // that two sent from one handler both arrive. See BrowserCommandChannel.
  private readonly commandChannel: BrowserCommandChannel =
    new BrowserCommandChannel((text: string): void => {
      this.browserCommandText = text;
      this.browserCommandRevision += 1;
    });

  private executeBrowserCommand(command: BrowserCommandPayload): void {
    this.commandChannel.send(command);
  }

  // Counts handleBack requests, so a late answer to an earlier one is not
  // acted on twice.
  private backRequestId: number = 0;

  private handleRuntimeEvent(event: BrowserRuntimeEvent): void {
    // The page had first refusal on the back gesture and did not take it, so
    // the tab goes back after all. An answer to an older request is ignored:
    // the reader may have pressed back twice before the first one came.
    if (event.event === 'backHandled') {
      if ((event.requestId ?? 0) === this.backRequestId &&
        !(event.handled ?? false)) {
        this.executeBrowserCommand({ command: 'back' });
      }
      return;
    }
    if (event.event === 'pwaInstalled' && this.isDesktopAuraUi()) {
      AlertDialog.show({
        title: '\u5b89\u88c5\u5b8c\u6210',
        message: '\u606d\u559c\u60a8\uff0c\u60a8\u5df2\u7ecf\u5c06\u5b83\u4f5c\u4e3a\u5c0f\u7a0b\u5e8f\u5b89\u88c5\u5230\u672c\u673a\u4e86\u3002\u60a8\u53ef\u4ee5\u6dfb\u52a0\u670d\u52a1\u5361\u7247\uff0c\u5728\u684c\u9762\u663e\u793a\u5c0f\u7a0b\u5e8f\uff01',
        confirm: {
          value: '\u5b8c\u6210',
          action: (): void => {
          }
        }
      });
      return;
    }

    if (event.event === 'downloadUpdated') {
      this.downloadNotifier?.update(shellEvent<DownloadUpdatedEvent>(event));
      return;
    }

    if (event.event === 'contextMenuRequested') {
      showContextMenuSheet(this.getUIContext(), event, (command: BrowserCommandPayload): void => {
        this.executeBrowserCommand(command);
      });
      return;
    }

    const context = this.getUIContext().getHostContext() as common.UIAbilityContext | undefined;
    if (!context) {
      if (event.event === 'filePickerRequested') {
        this.completeFilePicker({
          requestId: event.requestId ?? 0,
          paths: [],
          fileTypeIndex: event.fileTypeIndex ?? 0,
          canceled: true
        });
      }
      return;
    }
    if (event.event === 'systemPrintReady' && event.filePath) {
      SystemPrintAdapter.printPdf(context, event.filePath).catch((error: Error): void => {
        console.error('System print failed:', error);
      });
    } else if (event.event === 'shareRequested' && event.url) {
      HuaweiShareAdapter.sharePage(context, event.url, event.title ?? event.url)
        .catch((error: Error): void => {
          console.error('Huawei Share failed:', error);
        });
    } else if (event.event === 'castRequested') {
      this.currentTabCastSession.openPicker(context, this.browserState)
        .catch((error: Error): void => {
          console.error('HarmonyOS cast picker failed:', error);
        });
    } else if (event.event === 'filePickerRequested') {
      HarmonyFilePickerAdapter.show(context, event)
        .then((result: HarmonyFilePickerResult): void => {
          this.completeFilePicker(result);
        })
        .catch((error: Error): void => {
          console.error('HarmonyOS file picker failed:', error);
          this.completeFilePicker({
            requestId: event.requestId ?? 0,
            paths: [],
            fileTypeIndex: event.fileTypeIndex ?? 0,
            canceled: true
          });
        });
    } else if (event.event === 'permissionsRequested') {
      PermissionRequestAdapter.request(context, event)
        .then((result: PermissionRequestResultPayload): void => {
          this.completePermissionRequest(result);
        })
        .catch((error: Error): void => {
          console.error('HarmonyOS permission request failed:', error);
          this.completePermissionRequest({
            requestId: event.requestId ?? 0,
            granted: [],
            denied: event.permissions ?? []
          });
        });
    } else if (event.event === 'systemActionRequested' && event.action) {
      SystemIntegrationAdapter.handle(context, event.action as ChromiumSystemAction)
        .catch((error: Error): void => {
          console.error(`HarmonyOS system action failed: ${error.message}`);
        });
    } else if (event.event === 'systemPrintFailed') {
      console.error(`Chromium PDF generation failed: ${event.reason ?? 'unknown'}`);
    }
  }

  private completeFilePicker(result: HarmonyFilePickerResult): void {
    this.executeBrowserCommand({
      command: 'filePickerResult',
      requestId: result.requestId,
      paths: result.paths,
      fileTypeIndex: result.fileTypeIndex,
      canceled: result.canceled
    });
  }

  // Chromium resolves a site's geolocation request against the app's own
  // permission before prompting, so it needs to know that permission. Reported
  // once the native side is live, and again after any request that could have
  // changed it.
  private reportSystemPermissionState(): void {
    // Subscribing here rather than at startup keeps the two together: the
    // report is only useful once the native side can receive it, and the
    // subscription exists to send the same report again when the user changes
    // the permission somewhere this shell cannot see.
    PermissionRequestAdapter.observeLocation((): void => {
      this.reportSystemPermissionState();
    });
    const state = PermissionRequestAdapter.locationState();
    hilog.info(0x0000, 'OhosPerm', 'reporting systemPermissionState=%{public}s', state);
    this.executeBrowserCommand({
      command: 'systemPermissionState',
      location: state
    });
  }

  private systemPermissionReported: boolean = false;

  private completePermissionRequest(result: PermissionRequestResultPayload): void {
    this.reportSystemPermissionState();
    this.executeBrowserCommand({
      command: 'permissionResult',
      requestId: result.requestId,
      granted: result.granted,
      denied: result.denied
    });
  }

  private handleAuxiliaryWindowState(state: AuxiliaryWindowState): void {
    if (state.windowRole === 'pwa') {
      this.handlePwaWindowState(state);
      return;
    }
    if (!this.isFullscreenWindow()) {
      return;
    }

    const previous = this.findAuxiliaryWindow(state.widget);
    const remaining = this.auxiliaryWindows.filter(
      (view: AuxiliaryWindowView): boolean => view.state.widget !== state.widget
    );
    if (state.visible && !state.destroyed) {
      const modal = state.modal ?? previous?.modal ?? false;
      remaining.push(this.layoutAuxiliary(state, modal, previous));
      remaining.sort((left: AuxiliaryWindowView, right: AuxiliaryWindowView): number =>
        left.state.stackingOrder - right.state.stackingOrder);
    }

    this.auxiliaryWindows = remaining;
    const focusable = remaining.filter(
      (view: AuxiliaryWindowView): boolean => view.state.anchored !== true);
    this.activeAuxiliaryWidget =
      focusable.length > 0 ? focusable[focusable.length - 1].state.widget : 0;
  }

  private handlePwaWindowState(state: AuxiliaryWindowState): void {
    if (state.destroyed || !state.visible) {
      this.launchingPwaWidgets = this.launchingPwaWidgets.filter(
        (widget: number): boolean => widget !== state.widget);
      return;
    }
    if (this.launchingPwaWidgets.includes(state.widget)) {
      return;
    }

    const context = this.getUIContext().getHostContext() as common.UIAbilityContext | undefined;
    if (!context) {
      return;
    }
    this.launchingPwaWidgets.push(state.widget);
    PwaWindowLauncher.launch(context, state, this.startupConfigText, this.profileId)
      .catch((error: Error): void => {
        this.launchingPwaWidgets = this.launchingPwaWidgets.filter(
          (widget: number): boolean => widget !== state.widget);
        console.error('Failed to launch the PWA system window:', error);
        this.requestAuxiliaryWindowDismiss(state.widget);
      });
  }

  private requestAuxiliaryWindowDismiss(widget: number): void {
    if (widget <= 0) {
      return;
    }
    this.auxiliaryDismissWidget = widget;
    this.auxiliaryDismissRevision += 1;
  }

  private isMobilePhoneUi(): boolean {
    if (!this.startupConfigText || this.startupConfigText.trim().length === 0) {
      return this.browserState.uiFamily === 'mobile_phone';
    }

    try {
      const startupConfig = JSON.parse(this.startupConfigText) as Record<string, Object>;
      return String(startupConfig['uiFamily'] ?? '') === 'mobile_phone';
    } catch (error) {
      return this.browserState.uiFamily === 'mobile_phone';
    }
  }

  private isDesktopAuraUi(): boolean {
    if (!this.startupConfigText || this.startupConfigText.trim().length === 0) {
      return this.browserState.uiFamily === 'aura_pc' ||
        this.browserState.uiFamily === 'aura_tablet';
    }
    try {
      const startupConfig = JSON.parse(this.startupConfigText) as Record<string, Object>;
      const uiFamily = String(startupConfig['uiFamily'] ?? '');
      return uiFamily === 'aura_pc' || uiFamily === 'aura_tablet';
    } catch (error) {
      return this.browserState.uiFamily === 'aura_pc' ||
        this.browserState.uiFamily === 'aura_tablet';
    }
  }

  private isFullscreenWindow(): boolean {
    if (!this.startupConfigText || this.startupConfigText.trim().length === 0) {
      return this.isMobilePhoneUi();
    }
    try {
      const startupConfig = JSON.parse(this.startupConfigText) as Record<string, Object>;
      return String(startupConfig['windowMode'] ?? '') === 'fullscreen';
    } catch (error) {
      return this.isMobilePhoneUi();
    }
  }
}
