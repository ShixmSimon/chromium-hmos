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
//
// The password manager uses the same service for a plainer question -- is this
// the person who saved the password -- and wants an answer, not a token:
//   verify { title }          -> { ok }
//   cancel                    -> {}               the caller went away
// verify resolves either way rather than rejecting, because "the user said no"
// is an answer and the engine logs a rejection as a transport failure.

import { userAuth } from '@kit.UserAuthenticationKit';
import { util } from '@kit.ArkTS';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { SystemServiceHandler } from './SystemServiceBridge';

const AUTH_TYPES: userAuth.UserAuthType[] = [
  userAuth.UserAuthType.FACE,
  userAuth.UserAuthType.FINGERPRINT,
  userAuth.UserAuthType.PIN
];

export class UserAuthService implements SystemServiceHandler {
  // The verification currently on screen, so cancel has something to cancel.
  private pending: userAuth.UserAuthInstance | null = null;

  async handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    switch (method) {
      case 'available':
        const result: Record<string, Object> = { 'available': this.available() };
        return result;
      case 'authenticate':
        return this.authenticate(args);
      case 'verify':
        return this.verify(args);
      case 'cancel':
        this.cancel();
        const empty: Record<string, Object> = {};
        return empty;
      default:
        throw new Error(`userauth has no method ${method}`);
    }
  }

  // PIN counts: a platform authenticator only needs *some* user verification,
  // and every HarmonyOS phone with a lock screen has one.
  private available(): boolean {
    return this.enrolledTypes().length > 0;
  }

  // Asking for a type the user never enrolled (no face registered, say) can
  // make the whole prompt fail to open, so only enrolled ones are offered.
  private enrolledTypes(): userAuth.UserAuthType[] {
    return AUTH_TYPES.filter((type: userAuth.UserAuthType): boolean => {
      try {
        userAuth.getAvailableStatus(type, userAuth.AuthTrustLevel.ATL3);
        return true;
      } catch (error) {
        return false;
      }
    });
  }

  // Whoever is holding the phone, are they the person who saved this? The
  // challenge is thrown away: nothing is being signed, so it only has to be
  // unpredictable, and HUKS is not involved.
  private async verify(args: Record<string, Object>): Promise<Object> {
    if (!this.available()) {
      const none: Record<string, Object> = { 'ok': false };
      return none;
    }
    const challenge: Uint8Array = cryptoFramework.createRandom().generateRandomSync(32).data;
    const title = String(args['title'] ?? '').length > 0 ? String(args['title']) : '验证身份';
    const authParam: userAuth.AuthParam = {
      challenge: challenge,
      authType: this.enrolledTypes(),
      authTrustLevel: userAuth.AuthTrustLevel.ATL3
    };
    const widgetParam: userAuth.WidgetParam = { title: title };
    return new Promise((resolve) => {
      let instance: userAuth.UserAuthInstance;
      try {
        instance = userAuth.getUserAuthInstance(authParam, widgetParam);
      } catch (error) {
        const failed: Record<string, Object> = { 'ok': false };
        resolve(failed);
        return;
      }
      this.pending = instance;
      instance.on('result', {
        onResult: (result: userAuth.UserAuthResult): void => {
          try {
            instance.off('result');
          } catch (error) {
            // Already detached.
          }
          if (this.pending === instance) {
            this.pending = null;
          }
          const reply: Record<string, Object> = {
            'ok': result.result === userAuth.UserAuthResultCode.SUCCESS
          };
          resolve(reply);
        }
      });
      instance.start();
    });
  }

  // The caller stopped waiting -- the tab closed, the window went away. Take
  // the prompt down rather than leave it over whatever is there now.
  private cancel(): void {
    const instance = this.pending;
    this.pending = null;
    if (instance === null) {
      return;
    }
    try {
      instance.cancel();
    } catch (error) {
      // Already finished.
    }
  }

  private authenticate(args: Record<string, Object>): Promise<Object> {
    const helper = new util.Base64Helper();
    const challenge: Uint8Array = helper.decodeSync(String(args['challenge'] ?? ''));
    const title = String(args['title'] ?? '').length > 0 ? String(args['title']) : '验证身份';
    const authParam: userAuth.AuthParam = {
      challenge: challenge,
      authType: this.enrolledTypes(),
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
