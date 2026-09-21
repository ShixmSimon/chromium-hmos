/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

import { abilityAccessCtrl, bundleManager, common, Permissions } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BrowserRuntimeEvent } from '../components/webwindow/NativeContext';

export interface PermissionRequestResultPayload {
  requestId: number;
  granted: string[];
  denied: string[];
}

export type SystemPermissionState = 'allowed' | 'denied' | 'notDetermined';

/**
 * Chromium asks for a HarmonyOS permission at the moment a site is granted the
 * web permission behind it, rather than the shell asking for everything at
 * startup. Declaring a permission in module.json5 only makes it requestable;
 * the process holds none of them until something asks the user.
 */
export class PermissionRequestAdapter {
  static async request(
    context: common.UIAbilityContext,
    event: BrowserRuntimeEvent
  ): Promise<PermissionRequestResultPayload> {
    const requestId = event.requestId ?? 0;
    const wanted = (event.permissions ?? []) as Permissions[];
    if (wanted.length === 0) {
      return { requestId, granted: [], denied: [] };
    }

    const result = await abilityAccessCtrl.createAtManager()
      .requestPermissionsFromUser(context, wanted);

    // authResults is positional: index 0 there describes permissions[0] here,
    // and 0 means granted.
    const names = result.permissions ?? wanted;
    const outcomes = result.authResults ?? [];
    const granted: string[] = [];
    const denied: string[] = [];
    names.forEach((name: string, index: number): void => {
      if (outcomes[index] === 0) {
        granted.push(name);
      } else {
        denied.push(name);
      }
    });
    return { requestId, granted, denied };
  }

  /**
   * The app's own location permission, which Chromium resolves a site request
   * against before prompting: with the app permission denied there is nothing a
   * site grant could achieve, so the site prompt is skipped entirely.
   *
   * 'notDetermined' covers the case where the token cannot be read at all --
   * reporting a denial we have not established would suppress prompts the user
   * should still see.
   */
  static locationState(): SystemPermissionState {
    try {
      const info = bundleManager.getBundleInfoForSelfSync(
        bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
      const tokenId = info.appInfo.accessTokenId;
      const granted = abilityAccessCtrl.createAtManager()
        .checkAccessTokenSync(tokenId, 'ohos.permission.LOCATION');
      const state: SystemPermissionState =
        granted === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED
          ? 'allowed' : 'denied';
      hilog.info(0x0000, 'OhosPerm', 'locationState=%{public}s token=%{public}d',
        state, tokenId);
      return state;
    } catch (error) {
      hilog.warn(0x0000, 'OhosPerm', 'locationState threw: %{public}s',
        String(error));
      return 'notDetermined';
    }
  }
}
