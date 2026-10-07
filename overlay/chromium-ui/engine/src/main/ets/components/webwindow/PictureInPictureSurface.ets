import { common } from '@kit.AbilityKit';
import { PiPWindow } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BrowserCommandPayload, BrowserRuntimeEvent, INativeContext } from './NativeContext';

@Component
export struct PictureInPictureSurface {
  @Prop componentId: string = '';
  @Prop widget: number = 0;
  private surfaceController: XComponentController = new XComponentController();
  private nativeContext: INativeContext | undefined = undefined;
  private pipController: PiPWindow.PiPController | undefined = undefined;
  private disposed: boolean = false;
  private attempted: boolean = false;
  private started: boolean = false;
  private restoring: boolean = false;
  private stopping: boolean = false;
  private stateTimer: number = -1;
  private missedStates: number = 0;

  aboutToDisappear(): void {
    this.disposed = true;
    this.clearStateTimer();
    if (this.started) {
      this.stop();
    }
  }

  private send(action: 'play' | 'pause' | 'close' | 'restore' | 'state'): void {
    const command: BrowserCommandPayload = {
      command: 'pictureInPictureAction', pipWidget: this.widget, pipAction: action
    };
    try {
      if (!this.nativeContext?.ExecuteBrowserCommand(this.componentId, JSON.stringify(command))) {
        hilog.warn(0x0000, 'ChromiumPiP', 'picture-in-picture command not accepted');
      }
    } catch {
      hilog.warn(0x0000, 'ChromiumPiP', 'picture-in-picture command failed');
    }
  }

  private clearStateTimer(): void {
    if (this.stateTimer !== -1) {
      clearInterval(this.stateTimer);
      this.stateTimer = -1;
    }
  }

  private pollState(): void {
    if (this.missedStates >= 3) {
      this.stop();
      return;
    }
    this.missedStates++;
    this.send('state');
  }

  private onNativeState(stateJson: string): void {
    if (this.disposed || !this.pipController || !this.started) {
      return;
    }
    try {
      const event = JSON.parse(stateJson) as BrowserRuntimeEvent;
      if (event.event === 'pictureInPictureState' && event.pipWidget === this.widget &&
        event.pipPlaying !== undefined) {
        this.missedStates = 0;
        this.pipController.updatePiPControlStatus(PiPWindow.PiPControlType.VIDEO_PLAY_PAUSE,
          event.pipPlaying ? PiPWindow.PiPControlStatus.PLAY : PiPWindow.PiPControlStatus.PAUSE);
      }
    } catch {
      hilog.warn(0x0000, 'ChromiumPiP', 'picture-in-picture status update failed');
    }
  }

  private async start(): Promise<void> {
    if (this.attempted || this.disposed) {
      return;
    }
    this.attempted = true;
    try {
      if (!canIUse('SystemCapability.Window.SessionManager') || !PiPWindow.isPiPEnabled()) {
        return;
      }
      this.nativeContext = this.surfaceController.getXComponentContext() as INativeContext;
      if (!this.nativeContext) {
        return;
      }
      this.nativeContext.SetBrowserEventCallback(this.componentId,
        (stateJson: string): void => this.onNativeState(stateJson));
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const controller = await PiPWindow.create({
        context: context,
        componentController: this.surfaceController,
        templateType: PiPWindow.PiPTemplateType.VIDEO_PLAY,
        controlGroups: []
      });
      if (this.disposed) {
        return;
      }
      this.pipController = controller;
      controller.on('stateChange', (state: PiPWindow.PiPState): void => {
        if (state === PiPWindow.PiPState.STARTED) {
          this.started = true;
        } else if (state === PiPWindow.PiPState.ABOUT_TO_RESTORE) {
          this.restoring = true;
        } else if (state === PiPWindow.PiPState.STOPPED) {
          const notify = this.started && !this.disposed && !this.stopping;
          this.started = false;
          this.clearStateTimer();
          this.release(controller);
          if (notify) {
            this.send(this.restoring ? 'restore' : 'close');
          }
        } else if (state === PiPWindow.PiPState.ERROR) {
          hilog.warn(0x0000, 'ChromiumPiP', 'system picture-in-picture lifecycle error');
        }
      });
      controller.on('controlEvent', (event: PiPWindow.ControlEventParam): void => {
        if (this.disposed || this.stopping ||
          event.controlType !== PiPWindow.PiPControlType.VIDEO_PLAY_PAUSE) {
          return;
        }
        if (event.status === PiPWindow.PiPControlStatus.PLAY) {
          this.send('play');
        } else if (event.status === PiPWindow.PiPControlStatus.PAUSE) {
          this.send('pause');
        }
      });
      await controller.startPiP();
      if (this.pipController !== controller) {
        return;
      }
      this.started = true;
      if (this.disposed) {
        this.stop();
      } else {
        this.pollState();
        this.stateTimer = setInterval((): void => { this.pollState(); }, 1000);
      }
    } catch {
      hilog.warn(0x0000, 'ChromiumPiP', 'system picture-in-picture unavailable; using in-app window');
      const controller = this.pipController;
      if (controller) {
        if (this.started) {
          await this.stop();
        } else {
          this.release(controller);
        }
      }
    }
  }

  private async stop(): Promise<void> {
    const controller = this.pipController;
    if (!controller || this.stopping) {
      return;
    }
    this.stopping = true;
    this.clearStateTimer();
    try {
      await controller.stopPiP();
      this.started = false;
      this.release(controller);
    } catch {
      hilog.warn(0x0000, 'ChromiumPiP', 'system picture-in-picture stop failed');
    } finally {
      this.stopping = false;
    }
  }

  private release(controller: PiPWindow.PiPController): void {
    try {
      controller.off('stateChange');
    } catch {
      hilog.warn(0x0000, 'ChromiumPiP', 'state listener cleanup failed');
    }
    try {
      controller.off('controlEvent');
    } catch {
      hilog.warn(0x0000, 'ChromiumPiP', 'control listener cleanup failed');
    }
    if (this.pipController === controller) {
      this.pipController = undefined;
    }
  }

  build() {
    XComponent({
      id: this.componentId,
      type: XComponentType.SURFACE,
      libraryname: 'web_engine',
      controller: this.surfaceController
    })
      .id(this.componentId)
      .width('100%')
      .height('100%')
      .backgroundColor(Color.Black)
      .onLoad((): void => { this.start(); })
  }
}
