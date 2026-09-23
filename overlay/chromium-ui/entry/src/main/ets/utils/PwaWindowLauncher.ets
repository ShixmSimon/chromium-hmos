import { AbilityConstant, common, StartOptions, Want } from '@kit.AbilityKit';
import { AuraStartupConfig } from 'engine';
import { AuxiliaryWindowState } from 'engine';

const FLOATING_WINDOW_MODE: number = 102;

export class PwaWindowLauncher {
  static async launch(
    context: common.UIAbilityContext,
    state: AuxiliaryWindowState,
    startupConfigText: string,
    profileId: string
  ): Promise<void> {
    const startUrl = state.pwaStartUrl || state.url || 'chrome://newtab/';
    const pwaAppId = state.pwaAppId || `widget-${state.widget}`;
    const config = PwaWindowLauncher.buildStartupConfig(
      startupConfigText, state.widget, pwaAppId, startUrl);
    const want: Want = {
      bundleName: context.abilityInfo.bundleName,
      abilityName: 'PWASmallAppExperience',
      parameters: {
        pwaWidget: state.widget,
        pwaAppId,
        pwaTitle: state.title || 'Chromium',
        pwaStartUrl: startUrl,
        pwaProfileId: profileId,
        pwaStartupConfig: JSON.stringify(config)
      }
    };
    const options: StartOptions = {
      hideStartWindow: true,
      windowMode: config.windowMode === 'windowed' ?
        FLOATING_WINDOW_MODE : AbilityConstant.WindowMode.WINDOW_MODE_FULLSCREEN
    };

    try {
      await context.startAbility(want, options);
    } catch (error) {
      // Some phones reject explicit window modes. A distinct UIAbility still
      // creates a real system window when launched without StartOptions.
      console.warn('PWA window mode was rejected, retrying with system default:', error);
      await context.startAbility(want);
    }
  }

  private static buildStartupConfig(
    startupConfigText: string,
    widget: number,
    pwaAppId: string,
    startUrl: string
  ): AuraStartupConfig {
    const config = JSON.parse(startupConfigText) as AuraStartupConfig;
    config.startUrl = startUrl;
    config.windowRole = 'pwa';
    config.targetWidget = widget;
    config.pwaAppId = pwaAppId;
    return config;
  }
}
