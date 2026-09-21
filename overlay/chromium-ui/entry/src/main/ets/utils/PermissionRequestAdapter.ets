/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { BrowserRuntimeEvent } from '../components/webwindow/NativeContext';

export interface PermissionRequestResultPayload {
  requestId: number;
  granted: string[];
  denied: string[];
}

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
}
