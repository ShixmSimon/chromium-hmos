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
//   authenticate { requestId, challenge, title }
//                             -> { token }       challenge and token are base64
// cancelAuthentication { requestId } closes only the matching WebAuthn prompt.
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

interface UserAuthPendingOperation {
  owner: UserAuthService;
  requestId: string;
  instance: userAuth.UserAuthInstance;
  finish: (result: userAuth.UserAuthResult | null) => void;
}

export class UserAuthService implements SystemServiceHandler {
  // System prompts are exclusive. Share ownership across per-window service
  // instances so a cancel routed through another window can find its request.
  private static pending: UserAuthPendingOperation | null = null;

  async handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    switch (method) {
      case 'available':
        const result: Record<string, Object> = { 'available': this.enrolledTypes().length > 0 };
        return result;
      case 'authenticate':
        return this.authenticate(args);
      case 'verify':
        return this.verify(args);
      case 'cancelAuthentication':
        const requestId: string = String(args['requestId'] ?? '');
        const authentication: UserAuthPendingOperation | null = UserAuthService.pending;
        if (requestId.length > 0 && authentication !== null && authentication.requestId === requestId) {
          this.cancelOperation(authentication);
        }
        const cancelledAuthentication: Record<string, Object> = {};
        return cancelledAuthentication;
      case 'cancel':
        // Legacy password verification must never cancel a WebAuthn prompt.
        const verification: UserAuthPendingOperation | null = UserAuthService.pending;
        if (verification !== null && verification.owner === this && verification.requestId.length === 0) {
          this.cancelOperation(verification);
        }
        const cancelledVerification: Record<string, Object> = {};
        return cancelledVerification;
      default:
        throw new Error(`userauth has no method ${method}`);
    }
  }

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

  // A failed or cancelled SDK operation settles once, even if cancel() emits
  // no result or a success callback arrives after cancellation.
  private run(challenge: Uint8Array, title: string, requestId: string): Promise<userAuth.UserAuthResult | null> {
    const types: userAuth.UserAuthType[] = this.enrolledTypes();
    if (UserAuthService.pending !== null || types.length === 0) {
      return Promise.resolve<userAuth.UserAuthResult | null>(null);
    }
    const authParam: userAuth.AuthParam = {
      challenge: challenge, authType: types, authTrustLevel: userAuth.AuthTrustLevel.ATL3
    };
    const widgetParam: userAuth.WidgetParam = { title: title };
    return new Promise<userAuth.UserAuthResult | null>((resolve) => {
      let instance: userAuth.UserAuthInstance;
      try {
        instance = userAuth.getUserAuthInstance(authParam, widgetParam);
      } catch (error) {
        resolve(null);
        return;
      }
      let finished: boolean = false;
      let operation: UserAuthPendingOperation | null = null;
      const finish = (result: userAuth.UserAuthResult | null): void => {
        if (finished) {
          return;
        }
        finished = true;
        if (UserAuthService.pending === operation) {
          UserAuthService.pending = null;
        }
        try {
          instance.off('result');
        } catch (error) {
          // Registration may have failed or the listener was already detached.
        }
        resolve(result);
      };
      operation = { owner: this, requestId: requestId, instance: instance, finish: finish };
      UserAuthService.pending = operation;
      try {
        instance.on('result', {
          onResult: (result: userAuth.UserAuthResult): void => { finish(result); }
        });
        instance.start();
      } catch (error) {
        this.cancelOperation(operation);
      }
    });
  }

  private cancelOperation(operation: UserAuthPendingOperation): void {
    // Retire ownership before SDK callbacks; a late callback cannot clear or
    // authenticate a newer request. Always settle the bridge's original call.
    operation.finish(null);
    try {
      operation.instance.cancel();
    } catch (error) {
      // Already finished or failed to start.
    }
  }

  private async verify(args: Record<string, Object>): Promise<Object> {
    const challenge: Uint8Array = cryptoFramework.createRandom().generateRandomSync(32).data;
    const title: string = String(args['title'] ?? '') || '验证身份';
    const result: userAuth.UserAuthResult | null = await this.run(challenge, title, '');
    const reply: Record<string, Object> = {
      'ok': result !== null && result.result === userAuth.UserAuthResultCode.SUCCESS
    };
    return reply;
  }

  private async authenticate(args: Record<string, Object>): Promise<Object> {
    const requestId: string = String(args['requestId'] ?? '');
    if (requestId.length === 0) {
      throw new Error('user authentication requires a requestId');
    }
    const helper = new util.Base64Helper();
    const challenge: Uint8Array = helper.decodeSync(String(args['challenge'] ?? ''));
    const title: string = String(args['title'] ?? '') || '验证身份';
    const result: userAuth.UserAuthResult | null = await this.run(challenge, title, requestId);
    if (result !== null && result.result === userAuth.UserAuthResultCode.SUCCESS && result.token) {
      const reply: Record<string, Object> = { 'token': helper.encodeToStringSync(result.token) };
      return reply;
    }
    throw new Error('user authentication cancelled, unavailable or failed');
  }
}
