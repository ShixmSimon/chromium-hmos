/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Web notifications as HarmonyOS notifications.
//
// Methods (see chrome/browser/notifications/notification_platform_bridge_ohos.cc):
//   publish { id, title, body, origin, silent, icon?: { width, height, rgba } }
//                         -> {}     rgba is base64 RGBA_8888, at most 128x128
//   close { id }          -> {}
//   displayed             -> { ids: string[] }
//   enabled               -> { enabled }
// Events: click { id }   when the user taps a notification.
//
// A tap launches the app through a WantAgent carrying the Chromium id. Only
// the shell's EntryAbility sees that launch, so it must pass every want to
// handleNotificationWant() (docs/SHELL_CONTRACT.md).

import { common, Want, wantAgent, WantAgent } from '@kit.AbilityKit';
import { notificationManager } from '@kit.NotificationKit';
import { image } from '@kit.ImageKit';
import { util } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { SystemServiceEmitter, SystemServiceHandler } from './SystemServiceBridge';

const WANT_KEY: string = 'chromiumNotificationId';
const DOMAIN: number = 0x0000;
const TAG: string = 'WebNotification';

// Clicks that arrive before the engine is up, e.g. a tap that launched the
// app from cold. Delivered as soon as a service is registered.
let activeService: NotificationService | null = null;
const pendingClicks: string[] = [];

// The shell calls this from EntryAbility.onCreate and onNewWant. Returns true
// when the want was a notification tap, which the shell can then treat as
// "bring the browser to the front" and nothing more.
export function handleNotificationWant(want: Want): boolean {
  const id = want.parameters?.[WANT_KEY];
  if (typeof id !== 'string' || id.length === 0) {
    return false;
  }
  if (activeService) {
    activeService.reportClick(id);
  } else {
    pendingClicks.push(id);
  }
  return true;
}

// HarmonyOS ids are 32-bit numbers; Chromium's are strings. The string rides
// along as the label, which is what identifies the notification for cancel.
export function numericId(id: string): number {
  let hash = 0;
  for (let i = 0; i < id.length; i++) {
    hash = (hash * 31 + id.charCodeAt(i)) | 0;
  }
  return hash & 0x7fffffff;
}

export class NotificationService implements SystemServiceHandler {
  private permissionAsked: boolean = false;
  private readonly context: common.UIAbilityContext;
  private readonly emit: SystemServiceEmitter;

  constructor(context: common.UIAbilityContext, emit: SystemServiceEmitter) {
    this.context = context;
    this.emit = emit;
    activeService = this;
    const queued = pendingClicks.splice(0, pendingClicks.length);
    queued.forEach((id: string): void => this.reportClick(id));
  }

  reportClick(id: string): void {
    const data: Record<string, Object> = { 'id': id };
    this.emit('click', data);
  }

  async handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    switch (method) {
      case 'publish':
        await this.publish(args);
        return undefined;
      case 'close': {
        const id = String(args['id'] ?? '');
        await notificationManager.cancel(numericId(id), id);
        return undefined;
      }
      case 'displayed': {
        const active = await notificationManager.getActiveNotifications();
        const ids: string[] = active
          .map((request: notificationManager.NotificationRequest): string => request.label ?? '')
          .filter((label: string): boolean => label.length > 0);
        const result: Record<string, Object> = { 'ids': ids };
        return result;
      }
      case 'enabled': {
        const enabled = await notificationManager.isNotificationEnabled();
        const result: Record<string, Object> = { 'enabled': enabled };
        return result;
      }
      default:
        throw new Error(`notification has no method ${method}`);
    }
  }

  private async publish(args: Record<string, Object>): Promise<void> {
    await this.ensureEnabled();
    const id = String(args['id'] ?? '');
    const origin = String(args['origin'] ?? '');
    const request: notificationManager.NotificationRequest = {
      id: numericId(id),
      label: id,
      notificationSlotType: notificationManager.SlotType.SOCIAL_COMMUNICATION,
      content: {
        notificationContentType: notificationManager.ContentType.NOTIFICATION_CONTENT_BASIC_TEXT,
        normal: {
          title: String(args['title'] ?? ''),
          text: String(args['body'] ?? ''),
          additionalText: origin
        }
      },
      wantAgent: await this.clickAgent(id)
    };
    if (args['silent'] === true) {
      request.isAlertOnce = true;
    }
    const icon = await this.iconFrom(args['icon'] as Record<string, Object> | undefined);
    if (icon) {
      request.largeIcon = icon;
    }
    await notificationManager.publish(request);
  }

  // The system asks the user once; after a refusal publish() fails, and the
  // failure goes back to Chromium as this call's error.
  private async ensureEnabled(): Promise<void> {
    if (await notificationManager.isNotificationEnabled()) {
      return;
    }
    if (this.permissionAsked) {
      throw new Error('Notifications are turned off for this app');
    }
    this.permissionAsked = true;
    await notificationManager.requestEnableNotification(this.context);
  }

  private clickAgent(id: string): Promise<WantAgent> {
    const parameters: Record<string, Object> = {};
    parameters[WANT_KEY] = id;
    const want: Want = {
      bundleName: this.context.abilityInfo.bundleName,
      abilityName: this.context.abilityInfo.name,
      parameters: parameters
    };
    const info: wantAgent.WantAgentInfo = {
      wants: [want],
      actionType: wantAgent.OperationType.START_ABILITY,
      requestCode: numericId(id),
      actionFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
    };
    return wantAgent.getWantAgent(info);
  }

  private async iconFrom(icon: Record<string, Object> | undefined): Promise<image.PixelMap | undefined> {
    if (!icon) {
      return undefined;
    }
    const width = Number(icon['width'] ?? 0);
    const height = Number(icon['height'] ?? 0);
    const rgba = String(icon['rgba'] ?? '');
    if (width <= 0 || height <= 0 || rgba.length === 0) {
      return undefined;
    }
    try {
      const bytes: Uint8Array = new util.Base64Helper().decodeSync(rgba);
      const buffer: ArrayBuffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
      return await image.createPixelMap(buffer, {
        size: { width: width, height: height },
        srcPixelFormat: image.PixelMapFormat.RGBA_8888,
        pixelFormat: image.PixelMapFormat.RGBA_8888,
        editable: false
      });
    } catch (error) {
      // A notification without its icon beats no notification.
      hilog.warn(DOMAIN, TAG, 'dropped an icon: %{public}s', String(error));
      return undefined;
    }
  }
}
