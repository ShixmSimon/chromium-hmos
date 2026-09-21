/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { Callback } from '@kit.BasicServicesKit';
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

  // Chromium is asking whether this app can locate at all, and that is the
  // coarse permission: LocationKit still returns network fixes when only
  // APPROXIMATELY_LOCATION is held, so gating on the precise one would BLOCK
  // sites that would have worked. The precise one is also ambiguous to read --
  // it reports INVALID for as long as the coarse one is NOT_DETERMINED.
  private static readonly kLocationGate: Permissions =
    'ohos.permission.APPROXIMATELY_LOCATION';

  private static locationWatcher:
    Callback<abilityAccessCtrl.PermissionStateChangeInfo> | undefined = undefined;

  /**
   * Follows the permission instead of sampling it. Chromium's copy is only as
   * good as the last report, and the user can revoke location in Settings
   * while a page is watching it -- nothing else in the shell would notice, and
   * the page would go on being told it may locate.
   *
   * Idempotent: a repeated subscription with the same arguments throws 12100004.
   */
  static observeLocation(onChange: () => void): void {
    if (PermissionRequestAdapter.locationWatcher) {
      return;
    }
    const watcher: Callback<abilityAccessCtrl.PermissionStateChangeInfo> =
      (info: abilityAccessCtrl.PermissionStateChangeInfo): void => {
        hilog.info(0x0000, 'OhosPerm', 'permission %{public}s changed: %{public}d',
          info.permissionName, info.change);
        onChange();
      };
    try {
      abilityAccessCtrl.createAtManager().on('selfPermissionStateChange',
        [PermissionRequestAdapter.kLocationGate], watcher);
      PermissionRequestAdapter.locationWatcher = watcher;
    } catch (error) {
      // Losing the subscription costs freshness, not correctness: the state is
      // still reported at startup and after each request.
      hilog.warn(0x0000, 'OhosPerm', 'observeLocation threw: %{public}s',
        String(error));
    }
  }

  static stopObservingLocation(): void {
    const watcher = PermissionRequestAdapter.locationWatcher;
    if (!watcher) {
      return;
    }
    PermissionRequestAdapter.locationWatcher = undefined;
    try {
      abilityAccessCtrl.createAtManager().off('selfPermissionStateChange',
        [PermissionRequestAdapter.kLocationGate], watcher);
    } catch (error) {
      hilog.warn(0x0000, 'OhosPerm', 'stopObservingLocation threw: %{public}s',
        String(error));
    }
  }

  /**
   * The app's own location permission, which Chromium resolves a site request
   * against before prompting: with the app permission denied there is nothing a
   * site grant could achieve, so the site prompt is skipped entirely.
   */
  static locationState(): SystemPermissionState {
    try {
      const status = abilityAccessCtrl.createAtManager()
        .getSelfPermissionStatus(PermissionRequestAdapter.kLocationGate);
      const state = PermissionRequestAdapter.toSystemState(status);
      hilog.info(0x0000, 'OhosPerm', 'locationState=%{public}s status=%{public}d',
        state, status);
      return state;
    } catch (error) {
      hilog.warn(0x0000, 'OhosPerm', 'locationState threw: %{public}s',
        String(error));
      return 'notDetermined';
    }
  }

  /**
   * checkAccessToken cannot tell "never asked" from "asked and refused" -- both
   * are PERMISSION_DENIED -- and calling both a denial deadlocks a site that is
   * already allowed: Chromium BLOCKs it, so no prompt appears, so nothing ever
   * asks the OS. getSelfPermissionStatus separates the two.
   */
  private static toSystemState(
    status: abilityAccessCtrl.PermissionStatus
  ): SystemPermissionState {
    switch (status) {
      case abilityAccessCtrl.PermissionStatus.GRANTED:
        return 'allowed';
      // Refused, or barred from being asked at all. Either way a site grant
      // could not lead anywhere, so Chromium should not offer one.
      case abilityAccessCtrl.PermissionStatus.DENIED:
      case abilityAccessCtrl.PermissionStatus.RESTRICTED:
        return 'denied';
      // NOT_DETERMINED (never requested, or set to ask every time) and INVALID
      // (undeclared, or unanswerable here) both leave the question open.
      // Reporting a denial we have not established would suppress the site
      // prompt that is what makes us ask the OS in the first place.
      default:
        return 'notDetermined';
    }
  }
}
