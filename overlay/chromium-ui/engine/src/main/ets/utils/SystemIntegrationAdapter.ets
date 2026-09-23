import { common, Want } from '@kit.AbilityKit';
import { updateManager } from '@kit.AppGalleryKit';
import bundleManager from '@ohos.bundle.bundleManager';

const HUAWEI_WALLET_APP_LINK = 'wallet://com.huawei.wallet/';

export type ChromiumSystemAction =
  'huaweiAccount' | 'huaweiWallet' | 'defaultBrowser' | 'checkForUpdates' |
  'appSettings';

export class SystemIntegrationAdapter {
  static async handle(
    context: common.UIAbilityContext,
    action: ChromiumSystemAction
  ): Promise<void> {
    switch (action) {
      case 'huaweiAccount':
        await SystemIntegrationAdapter.openHuaweiAccount(context);
        return;
      case 'huaweiWallet':
        await SystemIntegrationAdapter.openHuaweiWallet(context);
        return;
      case 'defaultBrowser':
        await SystemIntegrationAdapter.openDefaultBrowserSettings(context);
        return;
      case 'checkForUpdates':
        await SystemIntegrationAdapter.checkForUpdates(context);
        return;
      case 'appSettings':
        await SystemIntegrationAdapter.openAppSettings(context);
        return;
    }
  }

  /**
   * The application's own permission page. Chromium's content-settings bubble
   * offers this as its primary button once it knows a permission is blocked at
   * the OS level, and that button previously had nowhere to go.
   */
  private static async openAppSettings(context: common.UIAbilityContext): Promise<void> {
    const bundleName = context.abilityInfo.bundleName;
    await context.startAbility({
      bundleName: 'com.huawei.hmos.settings',
      abilityName: 'com.huawei.hmos.settings.MainAbility',
      uri: 'application_info_entry',
      parameters: { pushParams: bundleName }
    });
  }

  private static async openHuaweiAccount(context: common.UIAbilityContext): Promise<void> {
    await context.startAbility({ bundleName: 'com.huawei.hmos.settings' });
  }

  private static async openHuaweiWallet(context: common.UIAbilityContext): Promise<void> {
    let available = false;
    try {
      available = bundleManager.canOpenLink(HUAWEI_WALLET_APP_LINK);
    } catch (error) {
      console.info(`Unable to query Huawei Wallet availability: ${String(error)}`);
    }
    if (!available) {
      return;
    }

    const implicitWant: Want = {
      action: 'ohos.want.action.viewData',
      uri: HUAWEI_WALLET_APP_LINK
    };
    await context.startAbility(implicitWant);
  }

  private static async openDefaultBrowserSettings(context: common.UIAbilityContext): Promise<void> {
    // HarmonyOS does not expose a public API for third-party applications to
    // set the default browser directly. Force the system picker and include
    // the caller so Chromium is selectable even though it initiated the Want.
    await context.startAbility({
      action: 'ohos.want.action.viewData',
      entities: [ 'entity.system.browsable' ],
      uri: 'https://www.bing.com/',
      parameters: {
        'ohos.ability.params.showDefaultPicker': true,
        showCaller: true
      }
    });
  }

  private static async checkForUpdates(context: common.UIAbilityContext): Promise<void> {
    try {
      const result = await updateManager.checkAppUpdate(context);
      if (result.updateAvailable === updateManager.UpdateAvailableCode.LATER_VERSION_EXIST) {
        AlertDialog.show({
          title: '\u53d1\u73b0\u65b0\u7248\u672c',
          message: '\u6709\u65b0\u7248\u672c\u66f4\u65b0\u4e86\uff0c\u8bf7\u524d\u5f80 AppGallery \u66f4\u65b0\u3002' +
            'Chromium \u4e0d\u4f1a\u7ed5\u8fc7\u7cfb\u7edf\u6267\u884c\u6eda\u52a8\u66f4\u65b0\u3002',
          primaryButton: {
            value: '\u524d\u5f80\u66f4\u65b0',
            action: (): void => {
              updateManager.showUpdateDialog(context).catch((error: Error): void => {
                console.error(`Failed to show AppGallery update dialog: ${error.message}`);
              });
            }
          },
          secondaryButton: {
            value: '\u7a0d\u540e',
            action: (): void => {}
          }
        });
        return;
      }

      AlertDialog.show({
        title: '\u5df2\u662f\u6700\u65b0\u7248\u672c',
        message: 'AppGallery \u6682\u672a\u53d1\u73b0\u53ef\u7528\u7684\u65b0\u7248\u672c\u3002',
        confirm: {
          value: '\u5b8c\u6210',
          action: (): void => {}
        }
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      AlertDialog.show({
        title: '\u6682\u65f6\u65e0\u6cd5\u68c0\u67e5\u66f4\u65b0',
        message: `\u65e0\u6cd5\u8fde\u63a5 AppGallery\uff1a${message}`,
        confirm: {
          value: '\u77e5\u9053\u4e86',
          action: (): void => {}
        }
      });
    }
  }
}
