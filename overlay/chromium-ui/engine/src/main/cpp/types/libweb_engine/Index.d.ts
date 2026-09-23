export interface AuraStartupConfig {
  startUrl: string;
  uiProfile: 'phone' | 'tablet' | 'pc';
  uiFamily: 'aura_pc' | 'aura_tablet' | 'mobile_phone' | 'mobile_large';
  windowMode: 'fullscreen' | 'windowed';
  deviceClass: 'phone' | 'foldable' | 'tablet' | '2in1' | 'tv' | 'car';
  marketName: string;
  productModel: string;
  displayWidth?: number;
  displayHeight?: number;
  displayDensity?: number;
  jitless: boolean;
  fullscreenRequested: boolean;
}

export const Initialize: (
  xComponentId: string,
  configJson: string,
  onWindowAction: (action: string) => void
) => void;
export const OnSurfaceCreated: (xComponentId: string) => void;
export const OnSurfaceChanged: (
  xComponentId: string,
  x: number,
  y: number,
  width: number,
  height: number,
  density: number
) => void;
export const OnSurfaceDestroyed: (xComponentId: string) => void;
export const OnVisibilityChanged: (xComponentId: string, visible: boolean) => void;
export const OnFocusChanged: (xComponentId: string, focused: boolean) => void;
export const OnThemeFontChanged: (xComponentId: string, fontId: string) => void;
export const DispatchPointerEvent: (xComponentId: string, eventJson: string) => void;
export const DispatchKeyEvent: (xComponentId: string, eventJson: string) => boolean;
export const Navigate: (xComponentId: string, url: string) => void;
export const SetBluetoothEventCallback: (
  xComponentId: string,
  callback: (commandJson: string) => void
) => void;
export const CompleteBluetoothMessage: (xComponentId: string, messageJson: string) => void;
export const SetSystemServiceCallback: (
  xComponentId: string,
  callback: (requestJson: string) => void
) => void;
export const CompleteSystemServiceMessage: (xComponentId: string, messageJson: string) => void;
export const SetUsbEventCallback: (
  xComponentId: string,
  callback: (commandJson: string) => void
) => void;
export const CompleteUsbMessage: (xComponentId: string, messageJson: string) => void;
export const Shutdown: (xComponentId: string) => void;
