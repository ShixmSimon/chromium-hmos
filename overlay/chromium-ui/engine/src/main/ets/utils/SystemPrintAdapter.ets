import { common } from '@kit.AbilityKit';
import { print } from '@kit.BasicServicesKit';

export class SystemPrintAdapter {
  static async printPdf(context: common.UIAbilityContext, filePath: string): Promise<void> {
    if (!filePath || filePath.trim().length === 0) {
      return;
    }
    if (!canIUse('SystemCapability.Print.PrintFramework')) {
      throw new Error('System printing is not supported on this device');
    }
    await print.print([filePath], context);
  }
}
