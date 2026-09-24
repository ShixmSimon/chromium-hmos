import { common } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import { BrowserRuntimeEvent } from '../components/webwindow/NativeContext';

export interface HarmonyFilePickerResult {
  requestId: number;
  paths: string[];
  fileTypeIndex: number;
  canceled: boolean;
}

export class HarmonyFilePickerAdapter {
  static async show(
    context: common.UIAbilityContext,
    request: BrowserRuntimeEvent
  ): Promise<HarmonyFilePickerResult> {
    const requestId = request.requestId ?? 0;
    const fileTypeIndex = request.fileTypeIndex ?? 0;
    if (!canIUse('SystemCapability.FileManagement.UserFileService')) {
      return { requestId, paths: [], fileTypeIndex, canceled: true };
    }

    const documentPicker = new picker.DocumentViewPicker(context);
    let paths: string[] = [];
    if (request.pickerType === 'save') {
      const options = new picker.DocumentSaveOptions();
      // No empty placeholder file: if Chromium cannot write the chosen file
      // (phones may not write outside the app's own Download/<bundle>), a 0 B
      // file would be left behind. The URI then also carries create and
      // delete permission.
      options.autoCreateEmptyFile = false;
      if (request.defaultName && request.defaultName.length > 0) {
        options.newFileNames = [request.defaultName];
      }
      if (request.suffixFilters && request.suffixFilters.length > 0) {
        options.fileSuffixChoices = request.suffixFilters;
      }
      paths = await documentPicker.save(options);
      let selectedTypeIndex: number = fileTypeIndex;
      if (canIUse('SystemCapability.FileManagement.UserFileService.FolderSelection')) {
        const pickerIndex = documentPicker.getSelectedIndex();
        selectedTypeIndex = pickerIndex >= 0 ? pickerIndex + 1 : fileTypeIndex;
      }
      return {
        requestId,
        paths,
        fileTypeIndex: selectedTypeIndex,
        canceled: paths.length === 0
      };
    }

    const options = new picker.DocumentSelectOptions();
    options.maxSelectNumber = request.pickerType === 'openMultiple' ? 500 : 1;
    if (request.suffixFilters && request.suffixFilters.length > 0) {
      options.fileSuffixFilters = request.suffixFilters;
    }
    if (request.pickerType === 'folder') {
      if (!canIUse('SystemCapability.FileManagement.UserFileService.FolderSelection')) {
        return { requestId, paths: [], fileTypeIndex, canceled: true };
      }
      options.selectMode = picker.DocumentSelectMode.FOLDER;
    }
    paths = await documentPicker.select(options);
    return { requestId, paths, fileTypeIndex, canceled: paths.length === 0 };
  }
}
