/* Copyright 2026. Licensed under the Apache License, Version 2.0. */
import display from '@ohos.display';

export interface DisplayBrightnessNativePort {
  UpdateDisplayBrightness(displayId: number, sdrNits: number,
    currentHeadroom: number, maxHeadroom: number): void;
}

// Public API 22. Query before Chromium startup; keep all backends up to date
// using system events, with no per-frame calls into the display service.
export class DisplayBrightnessBridge {
  private displayId: number = -1;
  private brightnessListening: boolean = false;
  private displayListening: boolean = false;
  private pending: display.BrightnessInfo | null = null;
  private timer: number = -1;
  private warned: boolean = false;
  private onBrightness = (id: number, info: display.BrightnessInfo): void => {
    if (id !== this.displayId) {
      return;
    }
    this.pending = info;
    if (this.timer < 0) {
      this.timer = setTimeout(() => {
        this.timer = -1;
        const value = this.pending;
        this.pending = null;
        if (value) {
          this.publish(value);
        }
      }, 100);
    }
  };
  private onDisplay = (_id: number): void => { this.refresh(); };

  private port: DisplayBrightnessNativePort;

  constructor(port: DisplayBrightnessNativePort) {
    this.port = port;
  }

  start(): void {
    // Subscribe first so a change during the initial query is not missed.
    try {
      display.on('brightnessInfoChange', this.onBrightness);
      this.brightnessListening = true;
    } catch (error) {
      this.warn(error);
    }
    try {
      display.on('change', this.onDisplay);
      this.displayListening = true;
    } catch (error) {
      this.warn(error);
    }
    this.refresh();
  }

  refresh(): void {
    this.cancelPending();
    try {
      this.displayId = display.getDefaultDisplaySync().id;
      this.publish(display.getBrightnessInfo(this.displayId));
    } catch (error) {
      // Older systems retain the existing GL defaults / Vulkan SDR10 path.
      this.port.UpdateDisplayBrightness(0, 0, 0, 0);
      this.warn(error);
    }
  }

  destroy(): void {
    this.cancelPending();
    if (this.brightnessListening) {
      try {
        display.off('brightnessInfoChange', this.onBrightness);
      } catch (error) {
        this.warn(error);
      }
      this.brightnessListening = false;
    }
    if (this.displayListening) {
      try {
        display.off('change', this.onDisplay);
      } catch (error) {
        this.warn(error);
      }
      this.displayListening = false;
    }
  }

  private publish(info: display.BrightnessInfo): void {
    this.port.UpdateDisplayBrightness(this.displayId, info.sdrNits,
      info.currentHeadroom, info.maxHeadroom);
  }

  private cancelPending(): void {
    if (this.timer >= 0) {
      clearTimeout(this.timer);
      this.timer = -1;
    }
    this.pending = null;
  }

  private warn(error: Object): void {
    if (!this.warned) {
      console.warn('OHOS brightness: system query/listener unavailable:', error);
      this.warned = true;
    }
  }
}
