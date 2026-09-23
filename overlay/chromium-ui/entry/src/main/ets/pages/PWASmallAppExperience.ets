import { common } from '@kit.AbilityKit';
import { WebWindow } from 'engine';
import { PwaCapsuleOverlay } from '../components/pwa/PwaCapsuleOverlay';
import { CurrentTabCastSession } from '../components/mobile/CurrentTabCastSession';
import {
  BrowserCommandPayload,
  BrowserRuntimeEvent,
  BrowserStateSnapshot,
  PwaMenuButtonState,
  PwaMenuItemState
} from 'engine';
import { HuaweiKnockShareSession, HuaweiShareAdapter } from 'engine';
import { HarmonyFilePickerAdapter, HarmonyFilePickerResult } from 'engine';
import { PermissionRequestAdapter, PermissionRequestResultPayload } from 'engine';
import { BrowserCommandChannel } from 'engine';
import { SystemPrintAdapter } from 'engine';
import { ChromiumSystemAction, SystemIntegrationAdapter } from 'engine';

const EMPTY_PWA_STATE: BrowserStateSnapshot = {
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
  url: '',
  domain: '',
  title: 'Chromium',
  loading: false,
  canGoBack: false,
  canGoForward: false,
  isPwaWindow: true,
  pwaAppId: '',
  pwaStartUrl: ''
};

@Entry({ useSharedStorage: true })
@Component
struct PWASmallAppExperience {
  @LocalStorageLink('pwaWidget') pwaWidget: number = 0;
  @LocalStorageLink('pwaStartUrl') pwaStartUrl: string = 'chrome://newtab/';
  @LocalStorageLink('pwaProfileId') pwaProfileId: string = 'local';
  @LocalStorageLink('pwaStartupConfig') pwaStartupConfig: string = '';
  @LocalStorageLink('pwaAppVisible') pwaAppVisible: boolean = true;
  @LocalStorageLink('pwaThemeFontId') pwaThemeFontId: string = '';
  @LocalStorageLink('pwaThemeFontRevision') pwaThemeFontRevision: number = 0;
  @State private browserState: BrowserStateSnapshot = EMPTY_PWA_STATE;
  @State private browserCommandText: string = '';
  @State private browserCommandRevision: number = 0;
  @State private windowStatus: number = 0;
  @State private menuSheetVisible: boolean = false;
  @State private menuSessionId: number = 0;
  @State private menuTitle: string = '';
  @State private rootMenuItems: PwaMenuItemState[] = [];
  @State private menuItems: PwaMenuItemState[] = [];
  private readonly castSession: CurrentTabCastSession = new CurrentTabCastSession();
  private readonly knockShareSession: HuaweiKnockShareSession = new HuaweiKnockShareSession();

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
    this.dismissMenuSheet();
    this.castSession.stop().catch((error: Error): void => {
      console.error('Failed to stop the PWA cast session:', error);
    });
  }

  build() {
    Stack({ alignContent: Alignment.Top }) {
      WebWindow({
        xComponentId: `aura_pwa_${this.pwaWidget}`,
        surfaceReadyStorageKey: `auraPwaSurfaceReady_${this.pwaWidget}`,
        manageWindowDecor: false,
        url: this.pwaStartUrl,
        profileId: this.pwaProfileId,
        startupConfigText: this.pwaStartupConfig,
        appVisible: this.pwaAppVisible,
        themeFontId: this.pwaThemeFontId,
        themeFontRevision: this.pwaThemeFontRevision,
        browserCommandText: this.browserCommandText,
        browserCommandRevision: this.browserCommandRevision,
        onBrowserState: (state: BrowserStateSnapshot): void => {
          this.browserState = state;
          if (!this.systemPermissionReported) {
            this.systemPermissionReported = true;
            this.reportSystemPermissionState();
          }
          this.castSession.update(state);
          this.knockShareSession.update(state);
        },
        onRuntimeEvent: (event: BrowserRuntimeEvent): void => {
          this.handleRuntimeEvent(event);
        },
        onWindowStatus: (status: number): void => {
          this.windowStatus = status;
        }
      })
        .width('100%')
        .height('100%')

      PwaCapsuleOverlay({
        browserState: this.browserState,
        windowStatus: this.windowStatus,
        onCommand: (command: BrowserCommandPayload): void => {
          this.executeBrowserCommand(command);
        }
      })
        .align(Alignment.Top)
        .bindSheet($$this.menuSheetVisible, this.PwaMenuSheet, {
          height: SheetSize.FIT_CONTENT,
          showClose: false,
          enableOutsideInteractive: false,
          maskColor: 0x33000000,
          onWillDismiss: (action: DismissSheetAction): void => {
            this.dismissMenuSheet();
            action.dismiss();
          }
        })
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.browser_canvas'))
  }

  private startKnockShare(): void {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext | undefined;
    if (!context) {
      return;
    }
    this.knockShareSession.start(context).catch((error: Error): void => {
      console.error(`Failed to start PWA knock share: ${error.message}`);
    });
  }

  @Builder
  private PwaMenuSheet() {
    Column({ space: 4 }) {
      Row({ space: 8 }) {
        if (this.menuTitle.length > 0) {
          Button({ type: ButtonType.Circle, stateEffect: true }) {
            SymbolGlyph($r('sys.symbol.chevron_backward'))
              .fontSize(20)
              .fontColor([ $r('app.color.icon_primary') ])
          }
          .width(36)
          .height(36)
          .backgroundColor(Color.Transparent)
          .accessibilityText('\u8fd4\u56de\u4e0a\u4e00\u7ea7')
          .onClick((): void => this.restoreRootMenu())
        }
        Text(this.menuTitle || this.browserState.title || 'Chromium')
          .layoutWeight(1)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
        Button({ type: ButtonType.Circle, stateEffect: true }) {
          SymbolGlyph($r('sys.symbol.multiply'))
            .fontSize(18)
            .fontColor([ $r('app.color.icon_primary') ])
        }
        .width(36)
        .height(36)
        .backgroundColor(Color.Transparent)
        .accessibilityText('\u5173\u95ed\u83dc\u5355')
        .onClick((): void => this.dismissMenuSheet())
      }
      .width('100%')
      .margin({ bottom: 4 })

      Scroll() {
        Column({ space: 2 }) {
          ForEach(this.menuItems, (item: PwaMenuItemState, index: number): void => {
            if (item.type === 'separator') {
              Divider()
                .strokeWidth(1)
                .color($r('app.color.surface_secondary'))
                .margin({ top: 4, bottom: 4 })
            } else if (item.type === 'title') {
              Text(item.label)
                .width('100%')
                .fontSize(13)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.text_secondary'))
                .padding({ left: 12, right: 12, top: 8, bottom: 4 })
            } else if (item.type === 'buttonRow') {
              this.PwaButtonRow(item.buttons ?? [])
            } else {
              this.PwaMenuItem(item)
            }
          }, (item: PwaMenuItemState, index: number): string =>
            `${this.menuSessionId}:${index}:${item.type}:${item.commandId}`)
        }
        .width('100%')
      }
      .width('100%')
      .constraintSize({ maxHeight: 520 })
    }
    .width('100%')
    .padding({ left: 20, right: 20, top: 20, bottom: 24 })
    .backgroundColor($r('app.color.surface_primary'))
    .hitTestBehavior(HitTestMode.Block)
  }

  @Builder
  private PwaMenuItem(item: PwaMenuItemState) {
    Button({ type: ButtonType.Normal, stateEffect: true }) {
      Row({ space: 10 }) {
        if ((item.type === 'check' || item.type === 'radio') && item.checked) {
          SymbolGlyph($r('sys.symbol.checkmark'))
            .fontSize(18)
            .fontColor([ $r('app.color.icon_primary') ])
        } else {
          Blank()
            .width(18)
        }
        Column({ space: 1 }) {
          Text(item.label)
            .width('100%')
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .fontSize(16)
            .fontColor($r('app.color.text_primary'))
          if (item.secondaryLabel.length > 0) {
            Text(item.secondaryLabel)
              .width('100%')
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .fontSize(12)
              .fontColor($r('app.color.text_secondary'))
          }
        }
        .layoutWeight(1)
        if (item.minorText.length > 0) {
          Text(item.minorText)
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
        }
        if ((item.children?.length ?? 0) > 0) {
          SymbolGlyph($r('sys.symbol.chevron_forward'))
            .fontSize(16)
            .fontColor([ $r('app.color.text_secondary') ])
        }
      }
      .width('100%')
    }
    .width('100%')
    .height(48)
    .padding({ left: 12, right: 12 })
    .borderRadius(8)
    .backgroundColor(Color.Transparent)
    .enabled(item.enabled)
    .opacity(item.enabled ? 1 : 0.42)
    .onClick((): void => this.openOrActivateMenuItem(item))
  }

  @Builder
  private PwaButtonRow(buttons: PwaMenuButtonState[]) {
    Row({ space: 8 }) {
      ForEach(buttons, (button: PwaMenuButtonState, index: number): void => {
        if (button.type === 'space') {
          Blank().layoutWeight(1)
        } else if (button.type === 'label') {
          Text(button.label)
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
        } else {
          Button(button.label, { type: ButtonType.Normal, stateEffect: true })
            .height(40)
            .layoutWeight(1)
            .borderRadius(8)
            .enabled(button.enabled)
            .onClick((): void => this.activateMenuCommand(button.commandId))
        }
      }, (button: PwaMenuButtonState, index: number): string =>
        `${this.menuSessionId}:button:${index}:${button.commandId}`)
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 4, bottom: 4 })
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
    if (event.event === 'pwaMenuModel' && event.menuSessionId && event.items) {
      this.menuSessionId = event.menuSessionId;
      this.menuTitle = '';
      this.rootMenuItems = event.items;
      this.menuItems = event.items;
      this.menuSheetVisible = true;
      return;
    }
    if (event.event === 'pwaMenuClosed') {
      if (!event.menuSessionId || event.menuSessionId === this.menuSessionId) {
        this.clearMenuSheetState();
      }
      return;
    }
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext | undefined;
    if (!context) {
      return;
    }
    if (event.event === 'systemPrintReady' && event.filePath) {
      SystemPrintAdapter.printPdf(context, event.filePath).catch((error: Error): void => {
        console.error('PWA system print failed:', error);
      });
    } else if (event.event === 'shareRequested' && event.url) {
      HuaweiShareAdapter.sharePage(context, event.url, event.title ?? event.url)
        .catch((error: Error): void => {
          console.error('PWA Huawei Share failed:', error);
        });
    } else if (event.event === 'castRequested') {
      this.castSession.openPicker(context, this.browserState).catch((error: Error): void => {
        console.error('PWA cast picker failed:', error);
      });
    } else if (event.event === 'filePickerRequested') {
      HarmonyFilePickerAdapter.show(context, event)
        .then((result: HarmonyFilePickerResult): void => this.completeFilePicker(result))
        .catch((error: Error): void => {
          console.error('PWA file picker failed:', error);
          this.completeFilePicker({
            requestId: event.requestId ?? 0,
            paths: [],
            fileTypeIndex: event.fileTypeIndex ?? 0,
            canceled: true
          });
        });
    } else if (event.event === 'permissionsRequested') {
      PermissionRequestAdapter.request(context, event)
        .then((result: PermissionRequestResultPayload): void =>
          this.completePermissionRequest(result))
        .catch((error: Error): void => {
          console.error('PWA permission request failed:', error);
          this.completePermissionRequest({
            requestId: event.requestId ?? 0,
            granted: [],
            denied: event.permissions ?? []
          });
        });
    } else if (event.event === 'systemActionRequested' && event.action) {
      SystemIntegrationAdapter.handle(context, event.action as ChromiumSystemAction)
        .catch((error: Error): void => {
          console.error('PWA system action failed:', error);
        });
    }
  }

  private systemPermissionReported: boolean = false;

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
    this.executeBrowserCommand({
      command: 'systemPermissionState',
      location: PermissionRequestAdapter.locationState()
    });
  }

  private completePermissionRequest(result: PermissionRequestResultPayload): void {
    this.reportSystemPermissionState();
    this.executeBrowserCommand({
      command: 'permissionResult',
      requestId: result.requestId,
      granted: result.granted,
      denied: result.denied
    });
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

  private openOrActivateMenuItem(item: PwaMenuItemState): void {
    if ((item.children?.length ?? 0) > 0) {
      this.menuTitle = item.label;
      this.menuItems = item.children ?? [];
      return;
    }
    this.activateMenuCommand(item.commandId);
  }

  private activateMenuCommand(commandId: number): void {
    if (this.menuSessionId <= 0) {
      return;
    }
    this.executeBrowserCommand({
      command: 'pwaMenuAction',
      menuSessionId: this.menuSessionId,
      commandId
    });
    this.menuSheetVisible = false;
  }

  private restoreRootMenu(): void {
    this.menuTitle = '';
    this.menuItems = this.rootMenuItems;
  }

  private dismissMenuSheet(): void {
    if (this.menuSessionId > 0) {
      this.executeBrowserCommand({
        command: 'pwaMenuDismiss',
        menuSessionId: this.menuSessionId
      });
    }
    this.clearMenuSheetState();
  }

  private clearMenuSheetState(): void {
    this.menuSheetVisible = false;
    this.menuSessionId = 0;
    this.menuTitle = '';
    this.rootMenuItems = [];
    this.menuItems = [];
  }
}
