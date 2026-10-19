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
  private warned: boolean = false;
  private queryReported: boolean = false;
  private eventReported: boolean = false;
  private lastReported: string = "";
  private onBrightness = (id: number, info: display.BrightnessInfo): void => {
    if (id !== this.displayId) {
      return;
    }
    if (!this.eventReported) {
      this.report('first-event', info);
      this.eventReported = true;
    }
    this.publish(info);
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
    try {
      this.displayId = display.getDefaultDisplaySync().id;
      const info = display.getBrightnessInfo(this.displayId);
      if (!this.queryReported) {
        this.report('first-query', info);
        this.queryReported = true;
      }
      this.publish(info);
    } catch (error) {
      // Older systems retain the existing GL defaults / Vulkan SDR10 path.
      this.port.UpdateDisplayBrightness(0, 0, 0, 0);
      this.warn(error);
    }
  }

  destroy(): void {
    this.displayId = -1;
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
    const signature = `${this.displayId}:${info.sdrNits}:${info.currentHeadroom}:${info.maxHeadroom}`;
    if (signature !== this.lastReported) {
      this.report('changed', info);
    }
    this.port.UpdateDisplayBrightness(this.displayId, info.sdrNits,
      info.currentHeadroom, info.maxHeadroom);
  }

  private report(source: string, info: display.BrightnessInfo): void {
    this.lastReported = `${this.displayId}:${info.sdrNits}:${info.currentHeadroom}:${info.maxHeadroom}`;
    // Chromium's hilog handler is not installed yet during the initial query.
    console.info(`OHOS brightness bridge: source=${source} display=${this.displayId}` +
      ` sdr_nits=${info.sdrNits} current_headroom=${info.currentHeadroom}` +
      ` max_headroom=${info.maxHeadroom}`);
  }

  private warn(error: Object): void {
    if (!this.warned) {
      console.warn('OHOS brightness: system query/listener unavailable:', error);
      this.warned = true;
    }
  }
}
