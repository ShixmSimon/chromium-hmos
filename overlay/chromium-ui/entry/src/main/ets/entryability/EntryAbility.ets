import { AbilityConstant, Configuration, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { http } from '@kit.NetworkKit';
import display from '@ohos.display';
import defaultAppManager from '@ohos.bundle.defaultAppManager';
import bundleManager from '@ohos.bundle.bundleManager';
import { CHROMIUM_HOME_URL, DeviceRouting, DeviceRoutingPolicy, WindowPolicy, WindowStateSnapshot } from 'engine';
import { UIManager } from 'engine';
import {
  CrossDeviceEntryAdapter,
  CrossDeviceSessionSnapshot
} from '../utils/crossdevice/CrossDeviceEntryAdapter';
import {
  BrowserProfileService,
  BrowserProfileState
} from '../utils/profile/BrowserProfileService';

const DOMAIN = 0x0000;
const AURA_SURFACE_READY_KEY = 'auraSurfaceReady';
const AURA_APP_VISIBLE_KEY = 'auraAppVisible';
const AURA_THEME_FONT_ID_KEY = 'auraThemeFontId';
const AURA_THEME_FONT_REVISION_KEY = 'auraThemeFontRevision';
const AURA_COLOR_SCHEME_KEY = 'auraColorScheme';
const AURA_ONBOARDING_READY_KEY = 'auraOnboardingReady';
const AURA_ONBOARDING_COMPLETE_KEY = 'auraOnboardingComplete';
const AURA_PROFILE_ID_KEY = 'auraProfileId';
const AURA_PROFILE_KIND_KEY = 'auraProfileKind';
const AURA_SYNC_MODE_KEY = 'auraSyncMode';
const AURA_BROWSER_COMMAND_TEXT_KEY = 'auraBrowserCommandText';
const AURA_BROWSER_COMMAND_REVISION_KEY = 'auraBrowserCommandRevision';
const HUAWEI_WALLET_APP_LINK = 'wallet://com.huawei.wallet/';
const CDP_PAGE_LIST_URL = 'http://127.0.0.1:9222/json/list';

interface CdpPageTarget {
  type?: string;
  url?: string;
  title?: string;
}

interface BrowserSystemStateCommand {
  command: string;
  huaweiWalletAvailable: boolean;
  known: boolean;
  isDefault?: boolean;
}

export default class EntryAbility extends UIAbility {
  private startupWant: Want | null = null;
  private mainWindow: window.Window | null = null;
  private lastRoutingSignature: string = '';
  private lastThemeFontId: string = '';
  private lastColorScheme: 'light' | 'dark' = 'light';
  private lastFoldStatus: number = -1;
  private routingRefreshTimer: number = -1;

  private readonly foldStatusChanged = (foldStatus: display.FoldStatus): void => {
    this.lastFoldStatus = Number(foldStatus);
    this.refreshRouting('foldStatusChange', this.lastFoldStatus);
    this.scheduleRoutingRefresh('foldStatusSettled', 180);
  };

  private readonly windowRotationChanged = (info: window.RotationChangeInfo): void => {
    if (info.type === window.RotationChangeType.WINDOW_DID_ROTATE) {
      this.scheduleRoutingRefresh('rotationDidChange', 80);
    }
  };

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.startupWant = want;
    const initialThemeFontId = this.context.config.fontId ?? '';
    this.lastThemeFontId = initialThemeFontId;
    this.lastColorScheme = this.resolveColorScheme(this.context.config.colorMode);
    AppStorage.setOrCreate(AURA_APP_VISIBLE_KEY, true);
    AppStorage.setOrCreate(AURA_THEME_FONT_ID_KEY, initialThemeFontId);
    AppStorage.setOrCreate(AURA_THEME_FONT_REVISION_KEY, 0);
    AppStorage.setOrCreate(AURA_COLOR_SCHEME_KEY, this.lastColorScheme);
    AppStorage.setOrCreate(AURA_ONBOARDING_READY_KEY, false);
    AppStorage.setOrCreate(AURA_ONBOARDING_COMPLETE_KEY, false);
    AppStorage.setOrCreate(AURA_PROFILE_ID_KEY, 'local');
    AppStorage.setOrCreate(AURA_PROFILE_KIND_KEY, 'local');
    AppStorage.setOrCreate(AURA_SYNC_MODE_KEY, 'off');
    AppStorage.setOrCreate('auraPwaWindowActive', false);
    AppStorage.setOrCreate(AURA_BROWSER_COMMAND_TEXT_KEY, '');
    AppStorage.setOrCreate(AURA_BROWSER_COMMAND_REVISION_KEY, 0);
    hilog.info(DOMAIN, 'AuraShell', 'Initial theme font configuration: %{public}s', initialThemeFontId);
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    } catch (err) {
      hilog.error(DOMAIN, 'AuraShell', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    this.restoreBrowserProfile();
  }

  onDestroy(): void {
    AppStorage.setOrCreate(AURA_APP_VISIBLE_KEY, false);
    this.unregisterRuntimeObservers();
  }

  onForeground(): void {
    AppStorage.setOrCreate(AURA_APP_VISIBLE_KEY, true);
    this.refreshSystemState();
  }

  onBackground(): void {
    AppStorage.setOrCreate(AURA_APP_VISIBLE_KEY, false);
  }

  onConfigurationUpdate(newConfig: Configuration): void {
    const fontId = newConfig.fontId ?? '';
    if (fontId !== this.lastThemeFontId) {
      this.lastThemeFontId = fontId;
      AppStorage.setOrCreate(AURA_THEME_FONT_ID_KEY, fontId);
      const revision = Number(AppStorage.get(AURA_THEME_FONT_REVISION_KEY) ?? 0);
      AppStorage.setOrCreate(AURA_THEME_FONT_REVISION_KEY, revision + 1);
      hilog.info(DOMAIN, 'AuraShell', 'Theme font configuration changed: %{public}s', fontId);
    }

    const colorScheme = this.resolveColorScheme(newConfig.colorMode);
    if (colorScheme !== this.lastColorScheme) {
      this.lastColorScheme = colorScheme;
      AppStorage.setOrCreate(AURA_COLOR_SCHEME_KEY, colorScheme);
      this.refreshRouting('colorModeChange', this.lastFoldStatus);
      hilog.info(DOMAIN, 'AuraShell', 'Color scheme changed: %{public}s', colorScheme);
    }
  }

  async onContinue(wantParam: Record<string, Object>): Promise<AbilityConstant.OnContinueResult> {
    const snapshot = await this.captureContinuationSnapshot();
    CrossDeviceEntryAdapter.writeContinuation(wantParam, snapshot);
    hilog.info(DOMAIN, 'AuraShell', 'Prepared cross-device continuation for %{public}s', snapshot.url);
    return AbilityConstant.OnContinueResult.AGREE;
  }

  onNewWant(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    this.startupWant = want;
    const startUrl = CrossDeviceEntryAdapter.resolveLaunchUrl(want);
    AppStorage.setOrCreate('auraStartUrl', startUrl);
    if (this.mainWindow) {
      this.refreshRouting('newWant');
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    const uiPath = UIManager.loadUI();
    const startUrl = CrossDeviceEntryAdapter.resolveLaunchUrl(this.startupWant ?? undefined);

    AppStorage.setOrCreate('auraStartUrl', startUrl);
    AppStorage.setOrCreate('auraRoutingRevision', 0);
    AppStorage.setOrCreate('auraRouting', '');
    AppStorage.setOrCreate('auraStartupConfig',
      UIManager.serializeAuraStartupConfig(startUrl, undefined, undefined, this.lastColorScheme));
    AppStorage.setOrCreate(AURA_SURFACE_READY_KEY, false);

    windowStage.loadContent(uiPath, (err) => {
      if (err && err.code) {
        hilog.error(DOMAIN, 'AuraShell', 'Failed to load UI content: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, 'AuraShell', 'Loaded UI content: %{public}s', uiPath);
    });

    windowStage.getMainWindow().then((win: window.Window) => {
      this.mainWindow = win;
      this.lastFoldStatus = DeviceRoutingPolicy.getFoldStatus();
      this.configureChromiumOwnedFrame(win);
      this.refreshRouting('initial', this.lastFoldStatus);
      this.registerRuntimeObservers(win);
    }).catch((error: Error) => {
      hilog.error(DOMAIN, 'AuraShell', 'Failed to get main window: %{public}s', JSON.stringify(error));
    });
  }

  onWindowStageDestroy(): void {
    AppStorage.setOrCreate(AURA_SURFACE_READY_KEY, false);
    this.unregisterRuntimeObservers();
  }

  private registerRuntimeObservers(win: window.Window): void {
    try {
      win.on('windowSizeChange', (_size: window.Size) => {
        this.refreshRouting('windowSizeChange', this.lastFoldStatus);
        this.scheduleRoutingRefresh('windowSizeSettled', 120);
      });
    } catch (error) {
      hilog.warn(DOMAIN, 'AuraShell', 'Window size observer is unavailable: %{public}s', JSON.stringify(error));
    }

    try {
      win.on('windowStatusChange', (_status: window.WindowStatusType) => {
        this.refreshRouting('windowStatusChange');
      });
    } catch (statusChangeError) {
      try {
        win.on('windowStatusDidChange', (_status: window.WindowStatusType) => {
          this.refreshRouting('windowStatusDidChange');
        });
      } catch (statusDidChangeError) {
        hilog.warn(DOMAIN, 'AuraShell', 'Window status observers are unavailable: %{public}s / %{public}s',
          JSON.stringify(statusChangeError), JSON.stringify(statusDidChangeError));
      }
    }

    try {
      win.on('freeWindowModeChange', (_enabled: boolean) => {
        this.refreshRouting('freeWindowModeChange');
      });
    } catch (error) {
      hilog.info(DOMAIN, 'AuraShell', 'Free-window observer is unavailable: %{public}s', JSON.stringify(error));
    }

    try {
      win.on('rotationChange', this.windowRotationChanged);
    } catch (error) {
      hilog.info(DOMAIN, 'AuraShell', 'Rotation observer is unavailable: %{public}s', JSON.stringify(error));
    }

    if (DeviceRoutingPolicy.isFoldable()) {
      try {
        display.on('foldStatusChange', this.foldStatusChanged);
      } catch (error) {
        hilog.error(DOMAIN, 'AuraShell', 'Failed to register fold listener: %{public}s', JSON.stringify(error));
      }
    }
  }

  private configureChromiumOwnedFrame(win: window.Window): void {
    try {
      win.setWindowDecorVisible(false);
    } catch (error) {
      hilog.warn(DOMAIN, 'AuraShell', 'Window decor control is unavailable: %{public}s', JSON.stringify(error));
    }

    win.setPreferredOrientation(window.Orientation.AUTO_ROTATION_UNSPECIFIED).catch((error: Error) => {
      hilog.warn(DOMAIN, 'AuraShell', 'Preferred orientation control is unavailable: %{public}s',
        JSON.stringify(error));
    });
  }

  private unregisterRuntimeObservers(): void {
    if (this.mainWindow) {
      try {
        this.mainWindow.off('windowSizeChange');
      } catch (error) {
      }
      try {
        this.mainWindow.off('windowStatusChange');
      } catch (error) {
      }
      try {
        this.mainWindow.off('windowStatusDidChange');
      } catch (error) {
      }
      try {
        this.mainWindow.off('freeWindowModeChange');
      } catch (error) {
      }
      try {
        this.mainWindow.off('rotationChange', this.windowRotationChanged);
      } catch (error) {
      }
    }

    try {
      display.off('foldStatusChange', this.foldStatusChanged);
    } catch (error) {
    }

    if (this.routingRefreshTimer >= 0) {
      clearTimeout(this.routingRefreshTimer);
      this.routingRefreshTimer = -1;
    }

    this.mainWindow = null;
  }

  private scheduleRoutingRefresh(reason: string, delayMs: number): void {
    if (this.routingRefreshTimer >= 0) {
      clearTimeout(this.routingRefreshTimer);
    }
    this.routingRefreshTimer = setTimeout(() => {
      this.routingRefreshTimer = -1;
      const foldStatus = DeviceRoutingPolicy.getFoldStatus();
      this.lastFoldStatus = foldStatus;
      this.refreshRouting(reason, foldStatus);
    }, delayMs);
  }

  private refreshRouting(reason: string, foldStatusOverride?: number): void {
    if (!this.mainWindow) {
      return;
    }

    if (reason !== 'initial' && reason !== 'colorModeChange' && !this.isSurfaceReady()) {
      hilog.info(DOMAIN, 'AuraShell', 'Skip routing refresh before first surface frame: %{public}s', reason);
      return;
    }

    const windowState = this.captureWindowState(this.mainWindow);
    const foldStatus = foldStatusOverride ?? this.lastFoldStatus;
    const routing = UIManager.getRouting(windowState, foldStatus);
    const startUrl = (AppStorage.get('auraStartUrl') as string) || CHROMIUM_HOME_URL;
    const routingSignature = `${DeviceRoutingPolicy.buildRoutingSignature(routing)}|${this.lastColorScheme}`;

    this.applyWindowPolicy(this.mainWindow, routing);
    AppStorage.setOrCreate('auraRouting', JSON.stringify(routing));
    AppStorage.setOrCreate('auraStartupConfig',
      UIManager.serializeAuraStartupConfig(startUrl, windowState, foldStatus, this.lastColorScheme));

    if (routingSignature !== this.lastRoutingSignature) {
      this.lastRoutingSignature = routingSignature;
      const currentRevision = Number(AppStorage.get('auraRoutingRevision') ?? 0);
      AppStorage.setOrCreate('auraRoutingRevision', currentRevision + 1);
    }

    hilog.info(DOMAIN, 'AuraShell', 'Routing refreshed from %{public}s: %{public}s', reason, routingSignature);
  }

  private isSurfaceReady(): boolean {
    return Boolean(AppStorage.get(AURA_SURFACE_READY_KEY) ?? false);
  }

  private resolveColorScheme(colorMode?: ConfigurationConstant.ColorMode): 'light' | 'dark' {
    return colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK ? 'dark' : 'light';
  }

  private restoreBrowserProfile(): void {
    BrowserProfileService.load(this.context)
      .then((state: BrowserProfileState): void => {
        AppStorage.setOrCreate(AURA_ONBOARDING_COMPLETE_KEY, state.onboardingComplete);
        AppStorage.setOrCreate(AURA_PROFILE_ID_KEY, state.profileId);
        AppStorage.setOrCreate(AURA_PROFILE_KIND_KEY, state.profileKind);
        AppStorage.setOrCreate(AURA_SYNC_MODE_KEY, state.syncMode);
        AppStorage.setOrCreate(AURA_ONBOARDING_READY_KEY, true);
      })
      .catch((error: Error): void => {
        hilog.error(DOMAIN, 'AuraShell', 'Failed to restore browser profile: %{public}s',
          JSON.stringify(error));
        AppStorage.setOrCreate(AURA_ONBOARDING_READY_KEY, true);
      });
  }

  private refreshSystemState(): void {
    let huaweiWalletAvailable = false;
    try {
      huaweiWalletAvailable = bundleManager.canOpenLink(HUAWEI_WALLET_APP_LINK);
    } catch (error) {
      hilog.info(DOMAIN, 'AuraShell', 'Huawei Wallet link is unavailable: %{public}s',
        JSON.stringify(error));
    }
    defaultAppManager.isDefaultApplication(defaultAppManager.ApplicationType.BROWSER)
      .then((isDefault: boolean): void => {
        this.dispatchBrowserCommand({
          command: 'systemCapabilities',
          huaweiWalletAvailable,
          known: true,
          isDefault
        });
      })
      .catch((error: Error): void => {
        hilog.warn(DOMAIN, 'AuraShell', 'Default browser state is unavailable: %{public}s',
          JSON.stringify(error));
        this.dispatchBrowserCommand({
          command: 'systemCapabilities',
          huaweiWalletAvailable,
          known: false
        });
      });
  }

  private dispatchBrowserCommand(command: BrowserSystemStateCommand): void {
    AppStorage.setOrCreate(AURA_BROWSER_COMMAND_TEXT_KEY, JSON.stringify(command));
    const revision = Number(AppStorage.get(AURA_BROWSER_COMMAND_REVISION_KEY) ?? 0);
    AppStorage.setOrCreate(AURA_BROWSER_COMMAND_REVISION_KEY, revision + 1);
  }

  private captureWindowState(win: window.Window): WindowStateSnapshot {
    let inFreeWindowMode = false;
    let windowStatus = 0;

    try {
      inFreeWindowMode = !!win.isInFreeWindowMode();
    } catch (error) {
    }

    try {
      windowStatus = Number(win.getWindowStatus());
    } catch (error) {
    }

    return {
      inFreeWindowMode,
      windowStatus
    };
  }

  private applyWindowPolicy(win: window.Window, routing: DeviceRouting): void {
    try {
      // Chromium owns browser chrome, but HarmonyOS remains the sole owner of
      // desktop caption controls. Fullscreen/mobile states have no caption.
      const pwaWindowActive = Boolean(AppStorage.get('auraPwaWindowActive') ?? false);
      win.setWindowDecorVisible(
        routing.windowPolicy === WindowPolicy.WINDOWED && !pwaWindowActive);
    } catch (error) {
      hilog.warn(DOMAIN, 'AuraShell', 'Window decor update is unavailable: %{public}s', JSON.stringify(error));
    }

    if (routing.windowPolicy === WindowPolicy.FULLSCREEN) {
      win.setWindowLayoutFullScreen(true).catch(() => {
      });
      try {
        win.setSpecificSystemBarEnabled('status', false);
        win.setSpecificSystemBarEnabled('navigationIndicator', false);
      } catch (error) {
      }
    } else {
      win.setWindowLayoutFullScreen(false).catch(() => {
      });
      try {
        win.setSpecificSystemBarEnabled('status', true);
        win.setSpecificSystemBarEnabled('navigationIndicator', true);
      } catch (error) {
      }
    }
  }

  private async captureContinuationSnapshot(): Promise<CrossDeviceSessionSnapshot> {
    const fallbackUrl = (AppStorage.get('auraStartUrl') as string) || CHROMIUM_HOME_URL;
    const request = http.createHttp();
    try {
      const response = await request.request(CDP_PAGE_LIST_URL, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.STRING,
        usingCache: false,
        connectTimeout: 1500,
        readTimeout: 1500
      });
      if (Number(response.responseCode) === 200 && typeof response.result === 'string') {
        const targets = JSON.parse(response.result) as CdpPageTarget[];
        for (const target of targets) {
          if (target.type === 'page' && target.url) {
            return {
              url: target.url,
              title: target.title ?? '',
              capturedAt: Date.now()
            };
          }
        }
      }
    } catch (error) {
      hilog.warn(DOMAIN, 'AuraShell', 'CDP snapshot unavailable during continuation: %{public}s',
        JSON.stringify(error));
    } finally {
      request.destroy();
    }
    return {
      url: fallbackUrl,
      title: '',
      capturedAt: Date.now()
    };
  }
}
