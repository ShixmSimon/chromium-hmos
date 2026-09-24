/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Where Chromium saves downloads: the app's own Download/<bundle> directory,
// which the user sees in the Files app. DocumentViewPicker in DOWNLOAD mode
// creates it, shows no UI and returns a URI the app keeps access to.
//
// A shell calls prepare() once its page is up -- earlier, the picker refuses
// with 13900042 -- and sends the URI in a setDownloadDirectory command. The
// engine makes it Chromium's download directory and stops asking where to save.

import { common } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const LOG_DOMAIN = 0x0000;
const LOG_TAG = 'ShellDownloadDirectory';

export class ShellDownloadDirectory {
  private static directoryUri: string = '';

  // The URI prepare() found, or '' before it has (or when it failed).
  static uri(): string {
    return ShellDownloadDirectory.directoryUri;
  }

  // Resolves to the directory URI; '' when the picker is unavailable, in which
  // case Chromium asks where to save each download instead.
  static async prepare(context: common.UIAbilityContext): Promise<string> {
    if (ShellDownloadDirectory.directoryUri.length > 0) {
      return ShellDownloadDirectory.directoryUri;
    }
    try {
      const options = new picker.DocumentSaveOptions();
      options.pickerMode = picker.DocumentPickerMode.DOWNLOAD;
      const result: string[] = await new picker.DocumentViewPicker(context).save(options);
      ShellDownloadDirectory.directoryUri = result.length > 0 ? result[0] : '';
      hilog.info(LOG_DOMAIN, LOG_TAG, 'Download directory: %{public}s',
        ShellDownloadDirectory.directoryUri);
    } catch (error) {
      hilog.warn(LOG_DOMAIN, LOG_TAG, 'Download directory unavailable: %{public}s',
        JSON.stringify(error));
    }
    return ShellDownloadDirectory.directoryUri;
  }
}
