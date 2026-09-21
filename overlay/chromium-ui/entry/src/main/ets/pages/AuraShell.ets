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
import { WebWindow } from '../components/webwindow/WebWindow';
import { MobileBrowserOverlay } from '../components/mobile/MobileBrowserOverlay';
import { CurrentTabCastSession } from '../components/mobile/CurrentTabCastSession';
import { BrowserOnboarding } from '../components/onboarding/BrowserOnboarding';
import {
  AuxiliaryWindowState,
  BrowserCommandPayload,
  BrowserRuntimeEvent,
  BrowserStateSnapshot
} from '../components/webwindow/NativeContext';
import { CHROMIUM_HOME_URL } from '../utils/DeviceType';
import { HuaweiKnockShareSession, HuaweiShareAdapter } from '../utils/HuaweiShareAdapter';
import { HarmonyFilePickerAdapter, HarmonyFilePickerResult } from '../utils/HarmonyFilePickerAdapter';
import { PermissionRequestAdapter, PermissionRequestResultPayload } from '../utils/PermissionRequestAdapter';
import { BrowserCommandChannel } from '../utils/BrowserCommandChannel';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { SystemPrintAdapter } from '../utils/SystemPrintAdapter';
import { ChromiumSystemAction, SystemIntegrationAdapter } from '../utils/SystemIntegrationAdapter';
import { PwaWindowLauncher } from '../utils/PwaWindowLauncher';
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
  @State private auxiliaryWindows: AuxiliaryWindowState[] = [];
  @State private activeAuxiliaryWidget: number = 0;
  @State private auxiliarySheetVisible: boolean = false;
  @State private auxiliaryDismissWidget: number = 0;
  @State private auxiliaryDismissRevision: number = 0;
  private readonly currentTabCastSession: CurrentTabCastSession = new CurrentTabCastSession();
  private readonly knockShareSession: HuaweiKnockShareSession = new HuaweiKnockShareSession();
  private launchingPwaWidgets: number[] = [];

  aboutToAppear(): void {
    this.startKnockShare();
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
              this.executeBrowserCommand(command);
            }
          })
        }
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.browser_canvas'))
    .bindSheet($$this.auxiliarySheetVisible, this.AuxiliaryWindowSheet, {
      height: SheetSize.LARGE,
      showClose: false,
      enableOutsideInteractive: false,
      maskColor: Color.Transparent,
      onWillDismiss: (action: DismissSheetAction): void => {
        this.requestAuxiliaryWindowDismiss(this.activeAuxiliaryWidget);
        if (this.auxiliaryWindows.length <= 1) {
          action.dismiss();
        }
      }
    })
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

  @Builder
  private AuxiliaryWindowSheet() {
    Column() {
      Row() {
        Text('Chromium')
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
        Blank()
        Button({ type: ButtonType.Circle }) {
          Image($r('app.media.chromium_close'))
            .width(20)
            .height(20)
            .fillColor($r('app.color.icon_primary'))
        }
        .width(40)
        .height(40)
        .backgroundColor(Color.Transparent)
        .onClick(() => {
          this.requestAuxiliaryWindowDismiss(this.activeAuxiliaryWidget);
        })
      }
      .width('100%')
      .height(48)

      if (this.auxiliaryWindows.length > 1) {
        Scroll() {
          Row({ space: 8 }) {
            ForEach(this.auxiliaryWindows, (state: AuxiliaryWindowState, index: number) => {
              Button(`窗口 ${index + 1}`)
                .height(36)
                .fontSize(14)
                .fontColor(state.widget === this.activeAuxiliaryWidget ?
                  $r('app.color.on_accent') : $r('app.color.text_secondary'))
                .backgroundColor(state.widget === this.activeAuxiliaryWidget ?
                  $r('app.color.accent_primary') : $r('app.color.surface_secondary'))
                .borderRadius(8)
                .onClick(() => {
                  this.activeAuxiliaryWidget = state.widget;
                })
            }, (state: AuxiliaryWindowState): string => state.widget.toString())
          }
        }
        .scrollable(ScrollDirection.Horizontal)
        .scrollBar(BarState.Off)
        .width('100%')
        .height(44)
      }

      Stack() {
        ForEach(this.auxiliaryWindows, (state: AuxiliaryWindowState) => {
          XComponent({
            id: state.componentId,
            type: XComponentType.SURFACE,
            libraryname: 'web_engine'
          })
            .id(state.componentId)
            .width('100%')
            .height('100%')
            .focusable(state.widget === this.activeAuxiliaryWidget)
            .defaultFocus(state.widget === this.activeAuxiliaryWidget)
            .visibility(state.widget === this.activeAuxiliaryWidget ?
              Visibility.Visible : Visibility.Hidden)
            .zIndex(state.widget === this.activeAuxiliaryWidget ? 1 : 0)
        }, (state: AuxiliaryWindowState): string => state.componentId)
      }
      .width('100%')
      .layoutWeight(1)
      .backgroundColor($r('app.color.surface_primary'))
    }
    .width('100%')
    .height('100%')
    .padding({ left: 12, right: 12, bottom: 12 })
    .backgroundColor($r('app.color.surface_primary'))
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

  private handleRuntimeEvent(event: BrowserRuntimeEvent): void {
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

    const remaining = this.auxiliaryWindows.filter(
      (item: AuxiliaryWindowState): boolean => item.widget !== state.widget
    );
    if (state.visible && !state.destroyed) {
      remaining.push(state);
      remaining.sort((left: AuxiliaryWindowState, right: AuxiliaryWindowState): number =>
        left.stackingOrder - right.stackingOrder);
      this.activeAuxiliaryWidget = state.widget;
    } else if (this.activeAuxiliaryWidget === state.widget) {
      this.activeAuxiliaryWidget = remaining.length > 0 ? remaining[remaining.length - 1].widget : 0;
    }

    this.auxiliaryWindows = remaining;
    this.auxiliarySheetVisible = remaining.length > 0;
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
