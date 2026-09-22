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
import { AuraStartupConfig } from '../../utils/DeviceType';

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

export interface BrowserRuntimeEvent {
  version: number;
  event: 'systemPrintReady' | 'systemPrintFailed' | 'shareRequested' |
    'castRequested' | 'filePickerRequested' | 'pwaInstalled' | 'pwaMenuModel' |
    'pwaMenuClosed' | 'inputRecovered' | 'systemActionRequested' |
    'permissionsRequested';
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
  windowRole: 'auxiliary' | 'pwa';
  pwaAppId?: string;
  title?: string;
  url?: string;
  pwaStartUrl?: string;
}

export interface INativeContext extends BluetoothNativePort {
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
  ExecuteBrowserCommand(xComponentId: string, commandJson: string): boolean;
  SetAuxiliaryWindowEventCallback(xComponentId: string, callback: (stateJson: string) => void): void;
  DismissAuxiliaryWindow(xComponentId: string, widget: number): boolean;
  Shutdown(xComponentId: string): void;
}

export class NativeContext {
  private readonly xComponentId: string;
  private nativeContext: INativeContext | null = null;
  private bluetoothBridge: BluetoothBridge | null = null;
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
