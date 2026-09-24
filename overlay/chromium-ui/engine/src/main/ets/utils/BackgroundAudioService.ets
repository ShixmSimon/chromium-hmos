/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// The audio-playback continuous task, held while a page's media session is
// active so playback survives the app leaving the foreground. Without it
// HarmonyOS mutes the app and then freezes it.
//
// Methods (see components/system_media_controls/ohos/system_media_controls_ohos.cc):
//   start -> {}   idempotent
//   stop  -> {}   idempotent
//
// The shell's ability must declare "backgroundModes": ["audioPlayback"]
// (docs/SHELL_CONTRACT.md); KEEP_BACKGROUND_RUNNING comes from this HAR.

import { common, wantAgent, WantAgent } from '@kit.AbilityKit';
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { SystemServiceHandler } from './SystemServiceBridge';

export class BackgroundAudioService implements SystemServiceHandler {
  private readonly context: common.UIAbilityContext;
  private running: boolean = false;

  constructor(context: common.UIAbilityContext) {
    this.context = context;
  }

  async handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    if (method === 'start') {
      if (!this.running) {
        await backgroundTaskManager.startBackgroundRunning(this.context,
          backgroundTaskManager.BackgroundMode.AUDIO_PLAYBACK, await this.returnAgent());
        this.running = true;
      }
      return undefined;
    }
    if (method === 'stop') {
      if (this.running) {
        this.running = false;
        await backgroundTaskManager.stopBackgroundRunning(this.context);
      }
      return undefined;
    }
    throw new Error(`backgroundaudio has no method ${method}`);
  }

  // Tapping the task's notification brings the browser back.
  private returnAgent(): Promise<WantAgent> {
    const info: wantAgent.WantAgentInfo = {
      wants: [{
        bundleName: this.context.abilityInfo.bundleName,
        abilityName: this.context.abilityInfo.name
      }],
      actionType: wantAgent.OperationType.START_ABILITY,
      requestCode: 0,
      actionFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
    };
    return wantAgent.getWantAgent(info);
  }
}
