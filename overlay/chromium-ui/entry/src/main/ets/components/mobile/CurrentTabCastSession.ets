import { common } from '@kit.AbilityKit';
import { avSession, AVCastPickerStyle } from '@kit.AVSessionKit';
import { BrowserStateSnapshot } from 'engine';

export class CurrentTabCastSession {
  private session: avSession.AVSession | null = null;
  private latestState: BrowserStateSnapshot | null = null;
  private starting: boolean = false;
  private pickerOpening: boolean = false;
  private pickerHelper: avSession.AVCastPickerHelper | null = null;
  private lifecycleRevision: number = 0;

  async start(context: common.UIAbilityContext, state: BrowserStateSnapshot): Promise<void> {
    this.latestState = state;
    if (this.session) {
      await this.publishCurrentTab();
      return;
    }
    if (this.starting || !canIUse('SystemCapability.Multimedia.AVSession.Core')) {
      return;
    }

    this.starting = true;
    const revision = ++this.lifecycleRevision;
    try {
      const session = await avSession.createAVSession(context, 'ChromiumCurrentTab', 'video');
      if (revision !== this.lifecycleRevision) {
        await session.destroy();
        return;
      }
      this.session = session;
      await session.activate();
      await this.publishCurrentTab();
    } catch (error) {
      console.error('Failed to initialize Chromium AVSession:', error);
    } finally {
      this.starting = false;
    }
  }

  update(state: BrowserStateSnapshot): void {
    this.latestState = state;
    this.publishCurrentTab().catch((error: Error) => {
      console.error('Failed to update Chromium AVSession:', error);
    });
  }

  async openPicker(
    context: common.UIAbilityContext,
    state: BrowserStateSnapshot
  ): Promise<void> {
    if (this.pickerOpening) {
      return;
    }
    if (!canIUse('SystemCapability.Multimedia.AVSession.AVCast')) {
      throw new Error('HarmonyOS AVCast is not supported on this device');
    }

    this.pickerOpening = true;
    try {
      await this.start(context, state);
      if (!this.pickerHelper) {
        this.pickerHelper = new avSession.AVCastPickerHelper(context);
      }
      await this.pickerHelper.select({
        sessionType: 'video',
        pickerStyle: AVCastPickerStyle.STYLE_PANEL
      });
    } finally {
      this.pickerOpening = false;
    }
  }

  async stop(): Promise<void> {
    this.lifecycleRevision += 1;
    const session = this.session;
    this.session = null;
    this.pickerHelper = null;
    this.pickerOpening = false;
    this.latestState = null;
    if (!session) {
      return;
    }

    try {
      await session.deactivate();
    } catch (error) {
    }
    try {
      await session.destroy();
    } catch (error) {
      console.error('Failed to destroy Chromium AVSession:', error);
    }
  }

  private async publishCurrentTab(): Promise<void> {
    const session = this.session;
    const state = this.latestState;
    if (!session || !state) {
      return;
    }

    const sourceUrl = state.url && state.url.length > 0 ? state.url : 'chrome://newtab/';
    const title = state.title && state.title.length > 0 ? state.title : 'Chromium';
    const subtitle = state.domain && state.domain.length > 0 ? state.domain : sourceUrl;
    const metadata: avSession.AVMetadata = {
      assetId: sourceUrl,
      title,
      subtitle,
      description: sourceUrl,
      filter: avSession.ProtocolType.TYPE_CAST_PLUS_STREAM
    };
    const queueItem: avSession.AVQueueItem = {
      itemId: 0,
      description: {
        assetId: sourceUrl,
        title,
        subtitle,
        description: sourceUrl,
        mediaType: 'text/html',
        extras: {
          sourceUrl,
          sourceKind: 'chromium-current-tab'
        }
      }
    };

    await session.setAVMetadata(metadata);
    await session.setAVQueueTitle('Chromium');
    await session.setAVQueueItems([queueItem]);
    await session.setExtras({
      sourceUrl,
      title,
      activeTabIndex: state.activeTabIndex,
      sourceKind: 'chromium-current-tab'
    });
  }
}
