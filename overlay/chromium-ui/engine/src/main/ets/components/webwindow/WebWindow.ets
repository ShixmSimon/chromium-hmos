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

import display from '@ohos.display';
import { common } from '@kit.AbilityKit';
import { promptAction, window } from '@kit.ArkUI';
import { fileIo } from '@kit.CoreFileKit';
import { i18n } from '@kit.LocalizationKit';
import { AuraStartupConfig, CHROMIUM_HOME_URL } from '../../utils/DeviceType';
import { UIManager } from '../../utils/UIManager';
import {
  INativeContext,
  AuxiliaryWindowState,
  BrowserCommandPayload,
  BrowserRuntimeEvent,
  BrowserStateSnapshot,
  KeyEventPayload,
  NativeContext,
  PointerEventPayload,
  WindowRect
} from './NativeContext';

export interface WebWindowOptions {
  xComponentId?: string;
  surfaceReadyStorageKey?: string;
  manageWindowDecor?: boolean;
  url?: string;
  profileId?: string;
  startupConfigText?: string;
  routingRevision?: number;
  appVisible?: boolean;
  themeFontId?: string;
  themeFontRevision?: number;
  browserCommandText?: string;
  browserCommandRevision?: number;
  auxiliaryDismissWidget?: number;
  auxiliaryDismissRevision?: number;
  onBrowserState?: (state: BrowserStateSnapshot) => void;
  onRuntimeEvent?: (event: BrowserRuntimeEvent) => void;
  onWindowStatus?: (status: number) => void;
  onAuxiliaryWindowState?: (state: AuxiliaryWindowState) => void;
  onPageLoadStart?: (url: string) => void;
  onPageLoadFinish?: (url: string) => void;
  onTitleReceive?: (title: string) => void;
  onError?: (error: Error) => void;
}

const ICU_DATA_RAWFILE_PATH: string = 'chromium/icudtl.dat';
const RUNTIME_VERSION_RAWFILE_PATH: string = 'chromium/runtime.version';
const COMMON_RUNTIME_RAWFILES: string[] = [
  'chrome_100_percent.pak',
  'chrome_200_percent.pak',
  'headless_command_resources.pak',
  'icudtl.dat',
  'resources.pak',
  'snapshot_blob.bin'
];

@ComponentV2
export struct WebWindow {
  @Param xComponentId: string = 'aura_shell';
  @Param surfaceReadyStorageKey: string = 'auraSurfaceReady';
  @Param manageWindowDecor: boolean = true;
  @Param url: string = CHROMIUM_HOME_URL;
  @Param profileId: string = 'local';
  @Param startupConfigText: string = '';
  @Param routingRevision: number = 0;
  @Param appVisible: boolean = true;
  @Param themeFontId: string = '';
  @Param themeFontRevision: number = 0;
  @Param browserCommandText: string = '';
  @Param browserCommandRevision: number = 0;
  @Param auxiliaryDismissWidget: number = 0;
  @Param auxiliaryDismissRevision: number = 0;
  @Param onBrowserState?: (state: BrowserStateSnapshot) => void = undefined;
  @Param onRuntimeEvent?: (event: BrowserRuntimeEvent) => void = undefined;
  @Param onWindowStatus?: (status: number) => void = undefined;
  @Param onAuxiliaryWindowState?: (state: AuxiliaryWindowState) => void = undefined;
  @Param onPageLoadStart?: (url: string) => void = undefined;
  @Param onPageLoadFinish?: (url: string) => void = undefined;
  @Param onTitleReceive?: (title: string) => void = undefined;
  @Param onError?: (error: Error) => void = undefined;

  @Local currentUrl: string = CHROMIUM_HOME_URL;
  @Local useTextureXComponent: boolean = false;

  private readonly xComponentController: XComponentController = new XComponentController();
  private nativeContext: NativeContext | null = null;
  private activeWindow: window.Window | null = null;
  private surfaceReady: boolean = false;
  private icuRawFdOpen: boolean = false;
  private icuDataFd: number = -1;
  private icuDataOffset: number = 0;
  private icuDataLength: number = 0;
  private hasWindowOrigin: boolean = false;
  private windowOriginX: number = 0;
  private windowOriginY: number = 0;
  private focusRecoveryTimers: number[] = [];
  private rotationSyncTimer: number = -1;
  private lastExecutedBrowserCommandRevision: number = 0;
  private lastAuxiliaryDismissRevision: number = 0;
  private lastPwaWindowActive: number = -1;
  private lastActivationEpoch: number = 0;
  private lastFocusedTarget: 'webContents' | 'sidePanel' = 'webContents';
  private lastSidePanelEntryId: string = '';

  aboutToAppear() {
    this.currentUrl = this.url && this.url.trim().length > 0 ? this.url : CHROMIUM_HOME_URL;
    this.updateXComponentMode(this.readStartupConfig());
    this.setSurfaceReady(false);
    this.initWindowListeners();
  }

  aboutToDisappear() {
    this.destroyNativeContext();
    this.removeWindowListeners();
  }

  @Monitor('routingRevision')
  private onRoutingRevisionChange(_monitor: IMonitor): void {
    const startupConfig = this.readStartupConfig();
    this.updateXComponentMode(startupConfig);
    if (!this.nativeContext) {
      return;
    }

    this.configureSurfaceRotation();
    this.applyDisplayMetrics(startupConfig);
    this.nativeContext.UpdateConfiguration(startupConfig);
  }

  @Monitor('url')
  private onUrlChange(_monitor: IMonitor): void {
    const nextUrl = this.url && this.url.trim().length > 0 ? this.url : CHROMIUM_HOME_URL;
    if (nextUrl !== this.currentUrl) {
      this.loadUrl(nextUrl);
    }
  }

  @Monitor('appVisible')
  private onAppVisibilityChange(_monitor: IMonitor): void {
    if (!this.nativeContext) {
      return;
    }

    this.nativeContext.OnVisibilityChanged(this.appVisible);
    this.nativeContext.OnFocusChanged(this.appVisible);
    if (this.appVisible) {
      this.notifyCurrentBounds();
      // Let the activating pointer sequence finish before restoring focus.
      this.restoreXComponentFocus(80);
    }
  }

  @Monitor('themeFontRevision')
  private onThemeFontRevisionChange(_monitor: IMonitor): void {
    this.nativeContext?.OnThemeFontChanged(this.themeFontId);
  }

  @Monitor('browserCommandRevision')
  private onBrowserCommandRevisionChange(_monitor: IMonitor): void {
    this.executePendingBrowserCommand();
  }

  @Monitor('auxiliaryDismissRevision')
  private onAuxiliaryDismissRevisionChange(_monitor: IMonitor): void {
    this.executePendingAuxiliaryDismiss();
  }

  build() {
    Column() {
      XComponent({
        id: this.xComponentId,
        type: this.useTextureXComponent ? XComponentType.TEXTURE : XComponentType.SURFACE,
        libraryname: 'web_engine',
        controller: this.xComponentController
      })
        .id(this.xComponentId)
        .width('100%')
        .height('100%')
        .focusable(true)
        .defaultFocus(true)
        .onLoad(() => {
          this.configureSurfaceRotation();
          this.initNativeContext();
        })
        .onDestroy(() => {
          this.destroyNativeContext();
        })
        .onFocus(() => {
          this.nativeContext?.OnFocusChanged(this.appVisible);
        })
        .onBlur(() => {
          this.nativeContext?.OnFocusChanged(false);
        })
        .onAxisEvent((event: AxisEvent) => {
          this.dispatchAxisEvent(event);
        })
        .onKeyEventDispatch((event: KeyEvent): boolean => {
          return this.dispatchKeyEvent(event);
        })
    }
    .width('100%')
    .height('100%')
  }

  private initNativeContext(): void {
    try {
      const context = this.xComponentController.getXComponentContext();
      if (!context) {
        this.reportError(new Error('Failed to get XComponent context'));
        return;
      }

      const startupConfig = this.readStartupConfig();
      this.updateXComponentMode(startupConfig);
      this.applyDisplayMetrics(startupConfig);
      this.attachUserDataDirectory(startupConfig);
      this.prepareRuntimeAssets(startupConfig);
      this.attachIcuDataDescriptor(startupConfig);
      this.surfaceReady = false;
      this.setSurfaceReady(false);
      this.showDiagnostic('1/2 onLoad 已触发 (libweb_engine.so 已加载)');
      this.nativeContext = new NativeContext(this.xComponentId);
      const abilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      this.nativeContext.init(
        context as INativeContext,
        abilityContext,
        startupConfig,
        (action: string): void => this.handleWindowAction(action),
        (stateJson: string): void => this.handleBrowserState(stateJson),
        (stateJson: string): void => this.handleAuxiliaryWindowState(stateJson)
      );
      this.executePendingBrowserCommand();
      this.executePendingAuxiliaryDismiss();
      this.nativeContext.OnSurfaceCreated();
      this.notifyWindowId();
      this.nativeContext.OnThemeFontChanged(this.themeFontId);
      this.nativeContext.OnVisibilityChanged(this.appVisible);
      this.nativeContext.OnFocusChanged(this.appVisible);
      this.notifyCurrentBounds();
      this.requestXComponentFocus();
      const swallowed = this.nativeContext.takeLastError();
      if (swallowed) {
        this.showDiagnostic(`2/2 初始化失败: ${swallowed}`);
      } else {
        this.showDiagnostic('2/2 引擎初始化完成');
      }
    } catch (error) {
      this.closeIcuDataDescriptor();
      this.reportError(this.normalizeError(error));
    }
  }

  private destroyNativeContext(): void {
    if (this.nativeContext) {
      this.setSurfaceReady(false);
      this.surfaceReady = false;
      this.nativeContext.OnVisibilityChanged(false);
      this.nativeContext.OnSurfaceDestroyed();
      this.nativeContext.destroy();
      this.nativeContext = null;
    }

    this.closeIcuDataDescriptor();
  }

  private requestXComponentFocus(): void {
    try {
      this.getUIContext().getFocusController().requestFocus(this.xComponentId);
    } catch (error) {
    }
  }

  private restoreXComponentFocus(delayMs: number = 0): void {
    if (!this.nativeContext || !this.appVisible) {
      return;
    }

    this.clearFocusRecoveryTimers();
    this.focusRecoveryTimers.push(setTimeout((): void => {
      if (!this.nativeContext || !this.appVisible) {
        return;
      }
      this.requestXComponentFocus();
      this.nativeContext.OnFocusChanged(true);
      this.requestNativeInputRecovery();
    }, delayMs));
  }

  private requestNativeInputRecovery(): void {
    if (!this.nativeContext || !this.appVisible) {
      return;
    }
    const command: BrowserCommandPayload = {
      command: 'recoverInput',
      activationEpoch: this.lastActivationEpoch,
      focusedTarget: this.lastFocusedTarget,
      sidePanelEntryId: this.lastSidePanelEntryId
    };
    this.nativeContext.ExecuteBrowserCommand(JSON.stringify(command));
  }

  private clearFocusRecoveryTimers(): void {
    this.focusRecoveryTimers.forEach((timer: number): void => clearTimeout(timer));
    this.focusRecoveryTimers = [];
  }

  private attachIcuDataDescriptor(config: AuraStartupConfig): void {
    if (!this.icuRawFdOpen) {
      const hostContext = this.getUIContext().getHostContext() as common.Context | undefined;
      if (!hostContext) {
        throw new Error('Failed to get host context for Chromium ICU data');
      }

      const descriptor = hostContext.resourceManager.getRawFdSync(ICU_DATA_RAWFILE_PATH);
      this.icuDataFd = Number(descriptor.fd);
      this.icuDataOffset = Number(descriptor.offset);
      this.icuDataLength = Number(descriptor.length);
      if (this.icuDataFd < 0 || this.icuDataOffset < 0 || this.icuDataLength <= 0) {
        hostContext.resourceManager.closeRawFdSync(ICU_DATA_RAWFILE_PATH);
        throw new Error('Chromium ICU rawfile descriptor is invalid');
      }
      this.icuRawFdOpen = true;
    }

    config.icuDataFd = this.icuDataFd;
    config.icuDataOffset = this.icuDataOffset;
    config.icuDataLength = this.icuDataLength;
  }

  private attachUserDataDirectory(config: AuraStartupConfig): void {
    const hostContext = this.getUIContext().getHostContext() as common.Context | undefined;
    if (!hostContext || !hostContext.filesDir) {
      throw new Error('Failed to get Chromium user data directory');
    }
    const safeProfileId = this.profileId.trim().replace(/[^a-zA-Z0-9._-]/g, '_').substring(0, 96) || 'local';
    if (safeProfileId === 'local') {
      // Preserve the legacy path so existing users keep their current profile.
      config.userDataDir = `${hostContext.filesDir}/chromium-profile`;
    } else {
      const profilesDir = `${hostContext.filesDir}/chromium-profiles`;
      if (!fileIo.accessSync(profilesDir)) {
        fileIo.mkdirSync(profilesDir, true);
      }
      config.userDataDir = `${profilesDir}/${safeProfileId}`;
    }
    config.printOutputDir = `${hostContext.filesDir}/chromium-print`;
  }

  private prepareRuntimeAssets(config: AuraStartupConfig): void {
    const hostContext = this.getUIContext().getHostContext() as common.Context | undefined;
    if (!hostContext || !hostContext.filesDir) {
      throw new Error('Failed to get Chromium runtime directory');
    }

    const runtimeDir = `${hostContext.filesDir}/chromium-runtime`;
    const localesDir = `${runtimeDir}/locales`;
    const runtimeVersion = this.readRuntimeAssetVersion(hostContext);
    if (!fileIo.accessSync(runtimeDir)) {
      fileIo.mkdirSync(runtimeDir, true);
    }
    if (!fileIo.accessSync(localesDir)) {
      fileIo.mkdirSync(localesDir, true);
    }

    const markerPath = `${runtimeDir}/.complete-${runtimeVersion}`;
    if (!fileIo.accessSync(markerPath)) {
      for (const fileName of COMMON_RUNTIME_RAWFILES) {
        this.extractRawFile(
          hostContext,
          `chromium/${fileName}`,
          `${runtimeDir}/${fileName}`
        );
      }
      this.writeFile(markerPath, runtimeVersion);
    }

    const localeFiles = hostContext.resourceManager.getRawFileListSync('chromium/locales');
    const applicationLocale = this.resolveChromiumLocale(localeFiles);
    const localeDestination = `${localesDir}/${applicationLocale}.pak`;
    const localeMarkerPath = `${runtimeDir}/.locale-${applicationLocale}-${runtimeVersion}`;
    if (!fileIo.accessSync(localeMarkerPath)) {
      this.extractRawFile(
        hostContext,
        `chromium/locales/${applicationLocale}.pak`,
        localeDestination
      );
      this.writeFile(localeMarkerPath, runtimeVersion);
    }

    config.resourcesDir = runtimeDir;
    config.applicationLocale = applicationLocale;
  }

  private readRuntimeAssetVersion(hostContext: common.Context): string {
    const content = hostContext.resourceManager.getRawFileContentSync(RUNTIME_VERSION_RAWFILE_PATH);
    let version = '';
    for (let index = 0; index < content.byteLength; index++) {
      version += String.fromCharCode(content[index]);
    }
    version = version.trim();
    if (!/^[0-9a-f]{64}$/.test(version)) {
      throw new Error('Chromium runtime version manifest is invalid');
    }
    return version;
  }

  private resolveChromiumLocale(localeFiles: string[]): string {
    const availableLocales: string[] = localeFiles
      .map((fileName: string): string => {
        const leafName = fileName.substring(fileName.lastIndexOf('/') + 1);
        return leafName.endsWith('.pak') ? leafName.substring(0, leafName.length - 4) : leafName;
      })
      .filter((locale: string): boolean => !locale.includes('_'));
    const systemLocale = i18n.System.getSystemLanguage().replace('_', '-');
    const candidates: string[] = [ systemLocale ];
    if (systemLocale.startsWith('zh-Hans')) {
      candidates.push('zh-CN');
    } else if (systemLocale.startsWith('zh-Hant')) {
      candidates.push('zh-TW');
    }
    candidates.push(systemLocale.split('-')[0]);
    candidates.push('en-US');

    for (const candidate of candidates) {
      if (availableLocales.includes(candidate)) {
        return candidate;
      }
    }
    return 'en-US';
  }

  private extractRawFile(
    hostContext: common.Context,
    rawFilePath: string,
    destinationPath: string
  ): void {
    const content = hostContext.resourceManager.getRawFileContentSync(rawFilePath);
    const buffer = content.buffer.slice(
      content.byteOffset,
      content.byteOffset + content.byteLength
    ) as ArrayBuffer;
    const file = fileIo.openSync(
      destinationPath,
      fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY
    );
    try {
      const bytesWritten = fileIo.writeSync(file.fd, buffer);
      if (bytesWritten !== content.byteLength) {
        throw new Error(`Short write while extracting ${rawFilePath}`);
      }
    } finally {
      fileIo.closeSync(file);
    }
  }

  private writeFile(path: string, content: string): void {
    const file = fileIo.openSync(
      path,
      fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.WRITE_ONLY
    );
    try {
      fileIo.writeSync(file.fd, content);
    } finally {
      fileIo.closeSync(file);
    }
  }

  private closeIcuDataDescriptor(): void {
    if (!this.icuRawFdOpen) {
      return;
    }

    try {
      const hostContext = this.getUIContext().getHostContext() as common.Context | undefined;
      hostContext?.resourceManager.closeRawFdSync(ICU_DATA_RAWFILE_PATH);
    } catch (error) {
      console.error('Failed to close Chromium ICU rawfile descriptor:', error);
    }
    this.icuRawFdOpen = false;
    this.icuDataFd = -1;
    this.icuDataOffset = 0;
    this.icuDataLength = 0;
  }

  private initWindowListeners(): void {
    try {
      const hostContext = this.getUIContext().getHostContext() as common.Context | undefined;
      if (!hostContext) {
        return;
      }

      window.getLastWindow(hostContext).then((win: window.Window) => {
        this.activeWindow = win;
        this.lastPwaWindowActive = -1;
        this.applyPwaWindowDecor(Boolean(AppStorage.get('auraPwaWindowActive') ?? false));
        this.notifyWindowId();
        this.notifyCurrentBounds();
        this.notifyWindowStatus();
        win.on('windowSizeChange', (size: window.Size) => {
          this.handleWindowSizeChange(size);
        });
        win.on('windowEvent', (event: window.WindowEventType) => {
          this.handleWindowEvent(event);
        });
        try {
          win.on('windowStatusChange', (status: window.WindowStatusType): void => {
            this.onWindowStatus?.(Number(status));
            this.restoreXComponentFocus(80);
          });
        } catch (statusChangeError) {
          try {
            win.on('windowStatusDidChange', (status: window.WindowStatusType): void => {
              this.onWindowStatus?.(Number(status));
              this.restoreXComponentFocus(80);
            });
          } catch (statusDidChangeError) {
          }
        }
        try {
          win.on('windowRectChange', (options: window.RectChangeOptions) => {
            this.handleWindowRectChange(options);
          });
        } catch (error) {
          // Some device classes expose only windowSizeChange.
        }
        try {
          win.on('rotationChange', (info: window.RotationChangeInfo) => {
            if (info.type !== window.RotationChangeType.WINDOW_DID_ROTATE) {
              return;
            }
            if (this.rotationSyncTimer >= 0) {
              clearTimeout(this.rotationSyncTimer);
            }
            this.rotationSyncTimer = setTimeout(() => {
              this.rotationSyncTimer = -1;
              this.configureSurfaceRotation();
              this.notifyCurrentBounds();
            }, 0);
          });
        } catch (error) {
          // API 19+ rotation callbacks are unavailable on some device classes.
        }
      }).catch((error: Error) => {
        this.reportError(error);
      });
    } catch (error) {
      this.reportError(this.normalizeError(error));
    }
  }

  private notifyWindowId(): void {
    if (!this.nativeContext || !this.activeWindow) {
      return;
    }

    try {
      const windowId = Number(this.activeWindow.getWindowProperties().id);
      if (windowId > 0) {
        this.nativeContext.OnWindowIdChanged(windowId);
      }
    } catch (error) {
    }
  }

  private removeWindowListeners(): void {
    if (!this.activeWindow) {
      return;
    }

    try {
      this.activeWindow.off('windowSizeChange');
    } catch (error) {
    }
    try {
      this.activeWindow.off('windowRectChange');
    } catch (error) {
    }
    try {
      this.activeWindow.off('windowEvent');
    } catch (error) {
    }
    try {
      this.activeWindow.off('windowStatusChange');
    } catch (error) {
    }
    try {
      this.activeWindow.off('windowStatusDidChange');
    } catch (error) {
    }
    try {
      this.activeWindow.off('rotationChange');
    } catch (error) {
    }

    this.clearFocusRecoveryTimers();
    if (this.rotationSyncTimer >= 0) {
      clearTimeout(this.rotationSyncTimer);
      this.rotationSyncTimer = -1;
    }

    this.activeWindow = null;
    this.lastPwaWindowActive = -1;
  }

  private handleWindowAction(action: string): void {
    if (action === 'close') {
      const hostContext = this.getUIContext().getHostContext() as common.UIAbilityContext | undefined;
      hostContext?.terminateSelf().catch((error: Error) => {
        this.reportError(error);
      });
      return;
    }

    if (!this.activeWindow) {
      return;
    }

    let operation: Promise<void> | undefined;
    try {
      switch (action) {
        case 'enterFullscreen':
          operation = this.activeWindow.setWindowLayoutFullScreen(true);
          break;
        case 'exitFullscreen':
          operation = this.activeWindow.setWindowLayoutFullScreen(false);
          break;
        case 'maximize':
          operation = this.activeWindow.maximize();
          break;
        case 'minimize':
          operation = this.activeWindow.minimize();
          break;
        case 'restore':
          operation = this.activeWindow.recover();
          break;
        case 'startMoving':
          operation = this.activeWindow.startMoving();
          break;
        default:
          return;
      }
    } catch (error) {
      this.reportError(this.normalizeError(error));
      return;
    }

    operation.catch((error: Error) => {
      this.reportError(error);
    });
  }

  private handleWindowSizeChange(size: window.Size): void {
    if (!this.nativeContext) {
      return;
    }

    this.restoreXComponentFocus(80);
    this.notifyWindowStatus();

    if (this.activeWindow) {
      try {
        const properties = this.activeWindow.getWindowProperties();
        this.updateSurfaceRect({
          x: Number(properties.windowRect.left),
          y: Number(properties.windowRect.top),
          width: Number(size.width),
          height: Number(size.height)
        });
        return;
      } catch (error) {
      }
    }

    this.updateSurfaceRect({
      x: this.hasWindowOrigin ? this.windowOriginX : 0,
      y: this.hasWindowOrigin ? this.windowOriginY : 0,
      width: size.width,
      height: size.height
    });
  }

  private handleWindowRectChange(options: window.RectChangeOptions): void {
    if (!this.nativeContext) {
      return;
    }

    this.restoreXComponentFocus(80);
    this.notifyWindowStatus();

    this.updateSurfaceRect({
      x: Number(options.rect.left),
      y: Number(options.rect.top),
      width: Number(options.rect.width),
      height: Number(options.rect.height)
    });
  }

  private handleWindowEvent(event: window.WindowEventType): void {
    if (!this.nativeContext) {
      return;
    }

    if (event === window.WindowEventType.WINDOW_ACTIVE ||
      event === window.WindowEventType.WINDOW_SHOWN) {
      this.notifyCurrentBounds();
      this.restoreXComponentFocus(80);
      return;
    }
    if (event === window.WindowEventType.WINDOW_INACTIVE ||
      event === window.WindowEventType.WINDOW_HIDDEN) {
      this.nativeContext.OnFocusChanged(false);
    }
  }

  private notifyCurrentBounds(): void {
    if (!this.nativeContext || !this.activeWindow) {
      return;
    }

    try {
      const properties = this.activeWindow.getWindowProperties();
      this.updateSurfaceRect({
        x: Number(properties.windowRect.left),
        y: Number(properties.windowRect.top),
        width: Number(properties.windowRect.width),
        height: Number(properties.windowRect.height)
      });
    } catch (error) {
      this.reportError(this.normalizeError(error));
    }
  }

  private notifyWindowStatus(): void {
    if (!this.activeWindow || !this.onWindowStatus) {
      return;
    }
    try {
      this.onWindowStatus(Number(this.activeWindow.getWindowStatus()));
    } catch (error) {
    }
  }

  private applyPwaWindowDecor(active: boolean): void {
    if (!this.manageWindowDecor) {
      return;
    }
    const marker = active ? 1 : 0;
    AppStorage.setOrCreate('auraPwaWindowActive', active);
    if (!this.activeWindow || this.lastPwaWindowActive === marker) {
      return;
    }
    this.lastPwaWindowActive = marker;
    try {
      const config = this.readStartupConfig();
      this.activeWindow.setWindowDecorVisible(!active && config.windowMode === 'windowed');
    } catch (error) {
    }
  }

  private updateSurfaceRect(rect: WindowRect): void {
    if (!this.nativeContext) {
      return;
    }

    this.hasWindowOrigin = true;
    this.windowOriginX = rect.x;
    this.windowOriginY = rect.y;
    this.nativeContext.OnSurfaceChanged(rect, this.getDisplayDensity());
    this.markSurfaceReady();
  }

  private configureSurfaceRotation(): void {
    if (this.useTextureXComponent) {
      return;
    }
    try {
      // Keep the surface geometry stable while the container compensates for
      // the unfolded fold-display layer orientation.
      this.xComponentController.setXComponentSurfaceRotation({ lock: true });
    } catch (error) {
      console.warn('Failed to configure XComponent surface rotation:', error);
    }
  }

  private updateXComponentMode(config: AuraStartupConfig): void {
    // Touch-first displays can change their buffer orientation independently
    // of the app window. TEXTURE lets the system compositor keep Chromium
    // upright across phone, tablet, fold, TV, and car rotations.
    this.useTextureXComponent = config.deviceClass !== '2in1';
  }

  private markSurfaceReady(): void {
    if (this.surfaceReady) {
      return;
    }

    this.surfaceReady = true;
    this.setSurfaceReady(true);
  }

  private setSurfaceReady(ready: boolean): void {
    if (this.surfaceReadyStorageKey.trim().length > 0) {
      AppStorage.setOrCreate(this.surfaceReadyStorageKey, ready);
    }
  }

  private dispatchAxisEvent(event: AxisEvent): void {
    if (!this.nativeContext) {
      return;
    }

    const payload: PointerEventPayload = {
      action: Number(event.action),
      pointerType: 'wheel',
      dispatchSource: 'arkui',
      pointerId: 0,
      button: 0,
      sourceTool: 0,
      x: Number(event.x),
      y: Number(event.y),
      rootX: Number(event.globalDisplayX ?? event.displayX),
      rootY: Number(event.globalDisplayY ?? event.displayY),
      rootWindowRelative: false,
      deltaX: Number(event.getHorizontalAxisValue()),
      deltaY: -Number(event.getVerticalAxisValue()),
      timestamp: Number(event.timestamp)
    };
    this.nativeContext.DispatchPointerEvent(payload);
  }

  private dispatchKeyEvent(event: KeyEvent): boolean {
    if (!this.nativeContext) {
      return false;
    }

    const payload: KeyEventPayload = {
      action: Number(event.type),
      keyCode: Number(event.keyCode),
      keyText: String(event.keyText),
      dispatchSource: 'arkui',
      sourceTool: Number(event.keySource),
      timestamp: Number(event.timestamp)
    };
    return this.nativeContext.DispatchKeyEvent(payload);
  }

  loadUrl(url: string): void {
    this.currentUrl = url;
    if (!this.nativeContext) {
      this.showDiagnostic('导航失败: native context 未创建 (onLoad 未触发?)');
      return;
    }
    this.nativeContext.Navigate(url);
  }

  private executePendingBrowserCommand(): void {
    if (!this.nativeContext || this.browserCommandRevision <= 0 ||
      this.browserCommandRevision === this.lastExecutedBrowserCommandRevision ||
      !this.browserCommandText || this.browserCommandText.trim().length === 0) {
      return;
    }

    if (this.nativeContext.ExecuteBrowserCommand(this.browserCommandText)) {
      this.lastExecutedBrowserCommandRevision = this.browserCommandRevision;
    }
  }

  private executePendingAuxiliaryDismiss(): void {
    if (!this.nativeContext || this.auxiliaryDismissWidget <= 0 ||
      this.auxiliaryDismissRevision <= 0 ||
      this.auxiliaryDismissRevision === this.lastAuxiliaryDismissRevision) {
      return;
    }

    if (this.nativeContext.DismissAuxiliaryWindow(this.auxiliaryDismissWidget)) {
      this.lastAuxiliaryDismissRevision = this.auxiliaryDismissRevision;
    }
  }

  private handleBrowserState(stateJson: string): void {
    if (!stateJson || stateJson.trim().length === 0) {
      return;
    }

    try {
      const parsed = JSON.parse(stateJson) as BrowserRuntimeEvent | BrowserStateSnapshot;
      const runtimeEvent = parsed as BrowserRuntimeEvent;
      if (runtimeEvent.event) {
        this.onRuntimeEvent?.(runtimeEvent);
        if (runtimeEvent.event === 'pwaMenuClosed') {
          this.restoreXComponentFocus();
        }
        return;
      }
      const state = parsed as BrowserStateSnapshot;
      const nextActivationEpoch = Number(state.activationEpoch ?? 0);
      const nextFocusedTarget: 'webContents' | 'sidePanel' =
        state.focusedTarget === 'sidePanel' ? 'sidePanel' : 'webContents';
      const nextSidePanelEntryId = state.sidePanelEntryId ?? '';
      const browserTargetChanged = nextActivationEpoch > 0 &&
        nextActivationEpoch !== this.lastActivationEpoch;
      this.lastActivationEpoch = nextActivationEpoch;
      this.lastFocusedTarget = nextFocusedTarget;
      this.lastSidePanelEntryId = nextSidePanelEntryId;
      this.applyPwaWindowDecor(state.isPwaWindow);
      this.onBrowserState?.(state);
      if (browserTargetChanged) {
        this.restoreXComponentFocus();
      }
    } catch (error) {
      console.error('Failed to parse Chromium browser state:', error);
    }
  }

  private handleAuxiliaryWindowState(stateJson: string): void {
    if (!this.onAuxiliaryWindowState || !stateJson || stateJson.trim().length === 0) {
      return;
    }

    try {
      const state = JSON.parse(stateJson) as AuxiliaryWindowState;
      if (state.widget > 0 && state.componentId.startsWith('aura_aux_')) {
        this.onAuxiliaryWindowState(state);
      }
    } catch (error) {
      console.error('Failed to parse Chromium auxiliary window state:', error);
    }
  }

  private readStartupConfig(): AuraStartupConfig {
    if (this.startupConfigText && this.startupConfigText.trim().length > 0) {
      try {
        const parsedConfig = JSON.parse(this.startupConfigText) as AuraStartupConfig;
        return parsedConfig;
      } catch (error) {
      }
    }

    return UIManager.buildAuraStartupConfig(this.currentUrl);
  }

  private getDisplayDensity(): number {
    try {
      const defaultDisplay = display.getDefaultDisplaySync();
      const densityPixels = Number(defaultDisplay.densityPixels ?? 0);
      if (densityPixels > 0) {
        return densityPixels;
      }

      const densityDpi = Number(defaultDisplay.densityDPI ?? 0);
      return densityDpi > 0 ? densityDpi / 160.0 : 1.0;
    } catch (error) {
      return 1.0;
    }
  }

  private applyDisplayMetrics(config: AuraStartupConfig): void {
    try {
      const defaultDisplay = display.getDefaultDisplaySync();
      config.displayWidth = Number(defaultDisplay.width);
      config.displayHeight = Number(defaultDisplay.height);
      config.displayDensity = this.getDisplayDensity();
    } catch (error) {
      config.displayWidth = 1920;
      config.displayHeight = 1080;
      config.displayDensity = 1.0;
    }
  }

  private normalizeError(error: Object): Error {
    if (error instanceof Error) {
      return error;
    }

    return new Error(String(error));
  }

  private reportError(error: Error): void {
    console.error('WebWindow error:', error);
    this.showDiagnostic(`引擎错误: ${error.message}`);
    if (this.onError) {
      this.onError(error);
    }
  }

  // Every failure in this component used to reach console.error only, which on
  // a device is indistinguishable from the engine simply doing nothing. Until
  // the bring-up is verified these are surfaced.
  private showDiagnostic(message: string): void {
    try {
      promptAction.showToast({ message: message, duration: 5000 });
    } catch (e) {
      console.error('showToast failed:', e);
    }
  }
}
