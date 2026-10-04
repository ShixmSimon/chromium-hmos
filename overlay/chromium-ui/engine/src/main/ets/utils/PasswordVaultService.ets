/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// HarmonyOS's Password Vault for pages Chromium draws.
//
// The vault fills ArkUI TextInputs and ArkWeb pages by itself. Chromium's
// pages are neither, so they are described to it by hand through
// autoFillManager: a username node and a password node, and the page's URL,
// which is what the vault matches saved accounts by.
//
// Methods (see chrome/browser/ui/ohos/password_vault_ohos.cc):
//   fill { url, kind: 'login' | 'signup', focus: 'username' | 'password' }
//        -> { username, password }   the account the user picked
//        -> {}                       nothing saved, or the user backed out
//   save { url, username, password }
//        -> {}                       the vault asks the user by itself
//
// Passwords pass through here and nowhere else on the ArkTS side: never
// logged, never kept.

import { autoFillManager, common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { SystemServiceHandler } from './SystemServiceBridge';

const DOMAIN = 0x0000;
const TAG = 'PasswordVault';

// Where the nodes say they are. The vault shows a sheet at the bottom of the
// window rather than a popup at the field, so these only need to be inside
// the window; Chromium does not report where the fields are.
const NODE_LEFT = 16;
const NODE_WIDTH = 328;
const NODE_HEIGHT = 44;
const USERNAME_TOP = 120;
const PASSWORD_TOP = 180;
const FALLBACK_WIDTH = 360;
const FALLBACK_HEIGHT = 780;

function rect(left: number, top: number, width: number, height: number): autoFillManager.AutoFillRect {
  const r: autoFillManager.AutoFillRect = { left: left, top: top, width: width, height: height };
  return r;
}

function emptyReply(): Record<string, Object> {
  const reply: Record<string, Object> = {};
  return reply;
}

export class PasswordVaultService implements SystemServiceHandler {
  private readonly context: common.UIAbilityContext;
  // The vault takes one fill at a time; a second one fails while the sheet
  // is up.
  private filling: boolean = false;

  constructor(context: common.UIAbilityContext) {
    this.context = context;
  }

  async handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    switch (method) {
      case 'fill':
        return this.fill(args);
      case 'save':
        return this.save(args);
      default:
        throw new Error(`passwordvault has no method ${method}`);
    }
  }

  private async fill(args: Record<string, Object>): Promise<Object> {
    if (this.filling) {
      return emptyReply();
    }
    const url = String(args['url'] ?? '');
    const signup = String(args['kind'] ?? '') === 'signup';
    const focusPassword = String(args['focus'] ?? '') === 'password';
    let ui: UIContext;
    let viewData: autoFillManager.ViewData;
    try {
      const win = await window.getLastWindow(this.context);
      ui = win.getUIContext();
      viewData = this.viewData(win, ui, url, '', '', signup, focusPassword);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'no window to show the vault in: %{public}s', String(error));
      return emptyReply();
    }
    const request: autoFillManager.FillRequest = {
      type: signup ? autoFillManager.AutoFillType.NEW_PASSWORD :
        (focusPassword ? autoFillManager.AutoFillType.PASSWORD : autoFillManager.AutoFillType.USER_NAME),
      viewData: viewData,
      // AUTO_REQUEST lists the accounts saved for this site; MANUAL_REQUEST
      // would list every account in the vault.
      triggerType: autoFillManager.AutoFillTriggerType.AUTO_REQUEST
    };
    this.filling = true;
    return new Promise((resolve) => {
      // Declared before it is passed: an inline object literal is reported
      // as an invalid callback and its result never arrives.
      const callback: autoFillManager.AutoFillCallback = {
        onSuccess: (data: autoFillManager.ViewData): void => {
          this.filling = false;
          resolve(this.account(data));
        },
        onFailure: (result: autoFillManager.FillFailureResult): void => {
          // Nothing saved for the site, the user closed the sheet, or Lumie
          // went to the background (which always ends the request).
          this.filling = false;
          hilog.info(DOMAIN, TAG, 'fill ended without an account: %{public}d', result.errCode);
          resolve(emptyReply());
        }
      };
      try {
        autoFillManager.requestAutoFill(ui, request, callback);
      } catch (error) {
        this.filling = false;
        hilog.warn(DOMAIN, TAG, 'fill request refused: %{public}d', (error as BusinessError).code);
        resolve(emptyReply());
      }
    });
  }

  private async save(args: Record<string, Object>): Promise<Object> {
    const url = String(args['url'] ?? '');
    const username = String(args['username'] ?? '');
    const password = String(args['password'] ?? '');
    if (password.length === 0) {
      return emptyReply();
    }
    try {
      const win = await window.getLastWindow(this.context);
      const ui = win.getUIContext();
      const request: autoFillManager.SaveRequest = {
        viewData: this.viewData(win, ui, url, username, password, false, true)
      };
      const callback: autoFillManager.AutoSaveCallback = {
        onSuccess: (): void => {
          hilog.info(DOMAIN, TAG, 'save handled by the vault');
        },
        onFailure: (): void => {
          hilog.info(DOMAIN, TAG, 'save not taken by the vault');
        }
      };
      autoFillManager.requestAutoSave(ui, request, callback);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'save request refused: %{public}s', String((error as BusinessError).code ?? error));
    }
    return emptyReply();
  }

  // The username and password the vault handed back, by node type. A
  // sign-up form gets its strong password from the NEW_PASSWORD node.
  private account(data: autoFillManager.ViewData): Record<string, Object> {
    let username = '';
    let password = '';
    for (const node of data.pageNodeInfos) {
      if (node.autoFillType === autoFillManager.AutoFillType.USER_NAME) {
        username = node.value;
      } else if (node.autoFillType === autoFillManager.AutoFillType.PASSWORD ||
        node.autoFillType === autoFillManager.AutoFillType.NEW_PASSWORD) {
        password = node.value;
      }
    }
    if (password.length === 0) {
      return emptyReply();
    }
    const reply: Record<string, Object> = { 'username': username, 'password': password };
    return reply;
  }

  private viewData(win: window.Window, ui: UIContext, url: string, username: string, password: string,
    signup: boolean, focusPassword: boolean): autoFillManager.ViewData {
    let width = FALLBACK_WIDTH;
    let height = FALLBACK_HEIGHT;
    try {
      const r = win.getWindowProperties().windowRect;
      width = ui.px2vp(r.width);
      height = ui.px2vp(r.height);
    } catch (error) {
      // Keep the fallback size.
    }
    const userNode: autoFillManager.PageNodeInfo = {
      id: 1,
      autoFillType: autoFillManager.AutoFillType.USER_NAME,
      value: username,
      placeholder: '',
      rect: rect(NODE_LEFT, USERNAME_TOP, NODE_WIDTH, NODE_HEIGHT),
      isFocus: !focusPassword
    };
    const passwordNode: autoFillManager.PageNodeInfo = {
      id: 2,
      autoFillType: signup ? autoFillManager.AutoFillType.NEW_PASSWORD : autoFillManager.AutoFillType.PASSWORD,
      value: password,
      placeholder: '',
      rect: rect(NODE_LEFT, PASSWORD_TOP, NODE_WIDTH, NODE_HEIGHT),
      isFocus: focusPassword
    };
    const data: autoFillManager.ViewData = {
      bundleName: this.context.abilityInfo.bundleName,
      pageUrl: url,
      pageNodeInfos: [userNode, passwordNode],
      pageRect: rect(0, 0, width, height)
    };
    return data;
  }
}
