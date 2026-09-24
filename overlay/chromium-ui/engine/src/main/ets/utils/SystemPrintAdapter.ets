import { common } from '@kit.AbilityKit';
import { print } from '@kit.BasicServicesKit';
import { fileUri } from '@kit.CoreFileKit';

export class SystemPrintAdapter {
  static async printPdf(context: common.UIAbilityContext, filePath: string): Promise<void> {
    if (!filePath || filePath.trim().length === 0) {
      return;
    }
    if (!canIUse('SystemCapability.Print.PrintFramework')) {
      throw new Error('System printing is not supported on this device');
    }
    // The print service reads the file from its own process and only accepts a
    // file:// URI; a sandbox path makes its preview say the format is unsupported.
    const uri = filePath.startsWith('/') ? fileUri.getUriFromPath(filePath) : filePath;
    await print.print([uri], context);
  }
}
