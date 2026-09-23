import { Want } from '@kit.AbilityKit';
import { CHROMIUM_HOME_URL } from 'engine';

export interface CrossDeviceSessionSnapshot {
  url: string;
  title: string;
  capturedAt: number;
}

const CONTINUATION_URL_KEY: string = 'chromium.continuation.url';
const CONTINUATION_TITLE_KEY: string = 'chromium.continuation.title';
const CONTINUATION_TIME_KEY: string = 'chromium.continuation.capturedAt';

export class CrossDeviceEntryAdapter {
  static resolveLaunchUrl(want?: Want): string {
    if (!want) {
      return CHROMIUM_HOME_URL;
    }

    const uri = CrossDeviceEntryAdapter.stringifyValue(want.uri);
    if (uri.startsWith('http://') || uri.startsWith('https://')) {
      return uri;
    }

    const parameters = want.parameters;
    const url = CrossDeviceEntryAdapter.firstHttpUrl([
      CrossDeviceEntryAdapter.readParameter(parameters, CONTINUATION_URL_KEY),
      CrossDeviceEntryAdapter.readParameter(parameters, 'url'),
      CrossDeviceEntryAdapter.readParameter(parameters, 'uri'),
      CrossDeviceEntryAdapter.readParameter(parameters, 'targetUrl')
    ]);
    if (url.startsWith('http://') || url.startsWith('https://')) {
      return url;
    }

    return CHROMIUM_HOME_URL;
  }

  static writeContinuation(
    wantParam: Record<string, Object>,
    snapshot: CrossDeviceSessionSnapshot
  ): void {
    const url = CrossDeviceEntryAdapter.isRestorableUrl(snapshot.url) ? snapshot.url : CHROMIUM_HOME_URL;
    wantParam[CONTINUATION_URL_KEY] = url;
    wantParam[CONTINUATION_TITLE_KEY] = snapshot.title;
    wantParam[CONTINUATION_TIME_KEY] = snapshot.capturedAt;
    wantParam['url'] = url;
  }

  private static readParameter(parameters: Record<string, Object> | undefined, key: string): string {
    if (!parameters) {
      return '';
    }

    return CrossDeviceEntryAdapter.stringifyValue(parameters[key]);
  }

  private static firstHttpUrl(values: string[]): string {
    for (let i = 0; i < values.length; i++) {
      const value = values[i];
      if (CrossDeviceEntryAdapter.isRestorableUrl(value)) {
        return value;
      }
    }
    return '';
  }

  private static isRestorableUrl(value: string): boolean {
    return value.startsWith('http://') || value.startsWith('https://') || value === CHROMIUM_HOME_URL;
  }

  private static stringifyValue(value: string | Object | undefined): string {
    if (typeof value === 'string') {
      return value;
    }
    if (value === undefined) {
      return '';
    }
    return String(value);
  }
}
