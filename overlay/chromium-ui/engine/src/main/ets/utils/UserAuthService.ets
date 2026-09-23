/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// User verification for Chromium's WebAuthn platform authenticator.
//
// The authenticator's keys live in HUKS and are bound to user authentication:
// HUKS hands out a challenge, the user proves who they are here, and the
// resulting AuthToken unlocks one signing operation. Keys never pass through
// ArkTS; only the challenge in and the token out do.
//
// Methods (see device/fido/ohos/):
//   available                 -> { available }   biometric or PIN at ATL3
//   authenticate { challenge, title }
//                             -> { token }       challenge and token are base64
// A cancelled or failed authentication rejects; Chromium maps that to
// NotAllowedError.

import { userAuth } from '@kit.UserAuthenticationKit';
import { util } from '@kit.ArkTS';
import { SystemServiceHandler } from './SystemServiceBridge';

const AUTH_TYPES: userAuth.UserAuthType[] = [
  userAuth.UserAuthType.FACE,
  userAuth.UserAuthType.FINGERPRINT,
  userAuth.UserAuthType.PIN
];

export class UserAuthService implements SystemServiceHandler {
  async handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    switch (method) {
      case 'available':
        const result: Record<string, Object> = { 'available': this.available() };
        return result;
      case 'authenticate':
        return this.authenticate(args);
      default:
        throw new Error(`userauth has no method ${method}`);
    }
  }

  // PIN counts: a platform authenticator only needs *some* user verification,
  // and every HarmonyOS phone with a lock screen has one.
  private available(): boolean {
    for (const type of AUTH_TYPES) {
      try {
        userAuth.getAvailableStatus(type, userAuth.AuthTrustLevel.ATL3);
        return true;
      } catch (error) {
        // Not enrolled or not supported; try the next.
      }
    }
    return false;
  }

  private authenticate(args: Record<string, Object>): Promise<Object> {
    const helper = new util.Base64Helper();
    const challenge: Uint8Array = helper.decodeSync(String(args['challenge'] ?? ''));
    const title = String(args['title'] ?? '').length > 0 ? String(args['title']) : '验证身份';
    const authParam: userAuth.AuthParam = {
      challenge: challenge,
      authType: AUTH_TYPES,
      authTrustLevel: userAuth.AuthTrustLevel.ATL3
    };
    const widgetParam: userAuth.WidgetParam = { title: title };
    return new Promise((resolve, reject) => {
      const instance = userAuth.getUserAuthInstance(authParam, widgetParam);
      instance.on('result', {
        onResult: (result: userAuth.UserAuthResult): void => {
          try {
            instance.off('result');
          } catch (error) {
            // Already detached.
          }
          if (result.result === userAuth.UserAuthResultCode.SUCCESS && result.token) {
            const reply: Record<string, Object> = { 'token': helper.encodeToStringSync(result.token) };
            resolve(reply);
          } else {
            reject(new Error(`user authentication ended with ${result.result}`));
          }
        }
      });
      instance.start();
    });
  }
}
