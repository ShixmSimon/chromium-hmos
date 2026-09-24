/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// The engine's public surface. A shell depends on this package, places a
// WebWindow, and draws its own UI around it; everything below is what that
// takes. The bridges are here rather than in the shell because Chromium calls
// back into them -- Bluetooth, USB and the system permission prompts have no
// C interface an app can use -- and a shell that forgot one would lose the
// matching Web API without any error.

// The browser surface and the protocol spoken across it.
export { WebWindow } from './src/main/ets/components/webwindow/WebWindow';
export {
  AuxiliaryWindowState,
  BrowserCommandPayload,
  BrowserRuntimeEvent,
  ContextMenuAction,
  ContextMenuMediaFlags,
  BrowserStateSnapshot,
  BrowserTabState,
  PwaMenuButtonState,
  PwaMenuItemState
} from './src/main/ets/components/webwindow/NativeContext';
export { BrowserCommandChannel } from './src/main/ets/utils/BrowserCommandChannel';
export * from './src/main/ets/components/webwindow/ShellServiceTypes';

// Auxiliary Chromium windows: sizing only. Where and how they are drawn is the
// shell's business.
export {
  AUXILIARY_WINDOW_MARGIN_VP,
  AuxiliaryWindowView,
  ScreenSizeVp,
  auxiliaryWindowLeftVp,
  layoutAuxiliaryWindow
} from './src/main/ets/utils/AuxiliaryWindowLayout';

// Bridges Chromium calls back into.
export { BluetoothBridge, BluetoothNativePort } from './src/main/ets/utils/BluetoothBridge';
export { UsbBridge, UsbNativePort } from './src/main/ets/utils/UsbBridge';
export {
  PermissionRequestAdapter,
  PermissionRequestResultPayload
} from './src/main/ets/utils/PermissionRequestAdapter';
export {
  HarmonyFilePickerAdapter,
  HarmonyFilePickerResult
} from './src/main/ets/utils/HarmonyFilePickerAdapter';
export { SystemPrintAdapter } from './src/main/ets/utils/SystemPrintAdapter';
export { ChromiumSystemAction, SystemIntegrationAdapter } from './src/main/ets/utils/SystemIntegrationAdapter';
export { HuaweiKnockShareSession, HuaweiShareAdapter } from './src/main/ets/utils/HuaweiShareAdapter';

// A tap on a web notification relaunches the app with this want; the shell's
// EntryAbility passes every want here (onCreate and onNewWant).
export { handleNotificationWant } from './src/main/ets/utils/NotificationService';

// Device classification and startup configuration the engine is launched with.
export {
  AuraStartupConfig,
  CHROMIUM_HOME_URL,
  DeviceRouting,
  DeviceRoutingPolicy,
  WindowPolicy,
  WindowStateSnapshot
} from './src/main/ets/utils/DeviceType';
export { UIManager } from './src/main/ets/utils/UIManager';
