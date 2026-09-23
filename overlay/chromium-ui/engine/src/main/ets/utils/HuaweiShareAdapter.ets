import { common } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import deviceInfo from '@ohos.deviceInfo';
import { harmonyShare, systemShare } from '@kit.ShareKit';
import { BrowserStateSnapshot } from '../components/webwindow/NativeContext';

export class HuaweiShareAdapter {
  static async sharePage(
    context: common.UIAbilityContext,
    url: string,
    title: string
  ): Promise<void> {
    if (!url || url.trim().length === 0) {
      return;
    }
    if (!canIUse('SystemCapability.Collaboration.SystemShare')) {
      throw new Error('System Share is not supported on this device');
    }

    const data = HuaweiShareAdapter.createPageData(url, title);
    const controller = new systemShare.ShareController(data);
    await controller.show(context, {
      selectionMode: systemShare.SelectionMode.SINGLE,
      previewMode: systemShare.SharePreviewMode.DETAIL
    });
  }

  static createPageData(url: string, title: string): systemShare.SharedData {
    const displayTitle = title && title.trim().length > 0 ? title : url;
    const record: systemShare.SharedRecord = {
      utd: 'general.hyperlink',
      content: url,
      uri: url,
      title: displayTitle,
      label: displayTitle,
      description: url
    };
    return new systemShare.SharedData(record);
  }
}

export class HuaweiKnockShareSession {
  private state: BrowserStateSnapshot | null = null;
  private listening: boolean = false;
  private starting: boolean = false;
  private lifecycleRevision: number = 0;
  private windowCapability: harmonyShare.SendCapabilityRegistry | null = null;

  private readonly shareCurrentPage = (target: harmonyShare.SharableTarget): void => {
    const state = this.state;
    if (!state || !state.ready || !HuaweiKnockShareSession.isShareableUrl(state.url)) {
      return;
    }
    target.share(HuaweiShareAdapter.createPageData(state.url, state.title))
      .catch((error: Error): void => {
        console.error(`HarmonyOS knock share failed: ${error.message}`);
      });
  };

  update(state: BrowserStateSnapshot): void {
    this.state = state;
  }

  async start(context: common.UIAbilityContext): Promise<void> {
    if (this.listening || this.starting ||
      !canIUse('SystemCapability.Collaboration.HarmonyShare')) {
      return;
    }

    const revision = ++this.lifecycleRevision;
    this.starting = true;
    try {
      const windowScoped = deviceInfo.deviceType === '2in1' ||
        deviceInfo.deviceType === 'tablet';
      if (windowScoped) {
        const hostWindow = await window.getLastWindow(context);
        if (revision !== this.lifecycleRevision) {
          return;
        }
        const capability: harmonyShare.SendCapabilityRegistry = {
          windowId: hostWindow.getWindowProperties().id,
          sendOnly: true
        };
        harmonyShare.on('knockShare', capability, this.shareCurrentPage);
        this.windowCapability = capability;
      } else {
        if (revision !== this.lifecycleRevision) {
          return;
        }
        harmonyShare.on('knockShare', this.shareCurrentPage);
      }
      this.listening = true;
    } catch (error) {
      console.error(`Failed to start HarmonyOS knock share: ${String(error)}`);
    } finally {
      if (revision === this.lifecycleRevision) {
        this.starting = false;
      }
    }
  }

  stop(): void {
    ++this.lifecycleRevision;
    this.starting = false;
    if (!this.listening ||
      !canIUse('SystemCapability.Collaboration.HarmonyShare')) {
      return;
    }
    try {
      if (this.windowCapability) {
        harmonyShare.off('knockShare', this.windowCapability, this.shareCurrentPage);
      } else {
        harmonyShare.off('knockShare', this.shareCurrentPage);
      }
    } catch (error) {
      console.error(`Failed to stop HarmonyOS knock share: ${String(error)}`);
    }
    this.windowCapability = null;
    this.listening = false;
  }

  private static isShareableUrl(url: string): boolean {
    return url.startsWith('http://') || url.startsWith('https://');
  }
}
