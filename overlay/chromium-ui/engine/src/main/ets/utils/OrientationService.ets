/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// screen.orientation.lock() / unlock() for Chromium, on the main window's
// preferred orientation.
//
// Methods (see chrome/browser/ui/ohos/screen_orientation_delegate_ohos.cc):
//   lock { type }  -> {}      type is a Web ScreenOrientationLockType name
//   unlock         -> {}      restores what the window had before the lock
//
// The window's and the display's landscape are mirror images on HarmonyOS
// (window LANDSCAPE reports as display LANDSCAPE_INVERTED), so which side
// "landscape-primary" lands on is decided here and nowhere else.

import { common } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { SystemServiceHandler } from './SystemServiceBridge';

const LOCK_TYPES: Map<string, window.Orientation> = new Map([
  ['portrait-primary', window.Orientation.PORTRAIT],
  ['portrait-secondary', window.Orientation.PORTRAIT_INVERTED],
  // Chromium derives the type from the display angle: 90 is
  // landscape-primary, 270 landscape-secondary. Window LANDSCAPE turns the
  // display to 270 and LANDSCAPE_INVERTED to 90 (measured on device), so the
  // names cross here; mapped straight, a landscape-primary lock never saw its
  // own orientation and never resolved.
  ['landscape-primary', window.Orientation.LANDSCAPE_INVERTED],
  ['landscape-secondary', window.Orientation.LANDSCAPE],
  ['portrait', window.Orientation.AUTO_ROTATION_PORTRAIT],
  ['landscape', window.Orientation.AUTO_ROTATION_LANDSCAPE],
  ['natural', window.Orientation.PORTRAIT],
  ['any', window.Orientation.AUTO_ROTATION]
]);

export class OrientationService implements SystemServiceHandler {
  // What the shell had set before the first lock; restored by unlock.
  private beforeLock: window.Orientation | undefined = undefined;
  private readonly context: common.UIAbilityContext;

  constructor(context: common.UIAbilityContext) {
    this.context = context;
  }

  async handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    const win = await window.getLastWindow(this.context);
    if (method === 'lock') {
      const type = String(args['type'] ?? '');
      const orientation = LOCK_TYPES.get(type);
      if (orientation === undefined) {
        throw new Error(`Unsupported orientation lock ${type}`);
      }
      if (this.beforeLock === undefined) {
        this.beforeLock = this.currentPreference(win);
      }
      await win.setPreferredOrientation(orientation);
      return undefined;
    }
    if (method === 'unlock') {
      const restore = this.beforeLock ?? window.Orientation.AUTO_ROTATION_UNSPECIFIED;
      this.beforeLock = undefined;
      await win.setPreferredOrientation(restore);
      return undefined;
    }
    throw new Error(`orientation has no method ${method}`);
  }

  private currentPreference(win: window.Window): window.Orientation {
    try {
      return win.getPreferredOrientation();
    } catch (error) {
      // The shell's own default (EntryAbility sets it at startup).
      return window.Orientation.AUTO_ROTATION_UNSPECIFIED;
    }
  }
}
