/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Downloads in the notification shade: a progress notification while a file
// downloads, "download complete" when it is done, and a tap that opens the
// file in the app the system picks for it.
//
// The shell feeds it every downloadUpdated event (see docs/SHELL_CONTRACT.md,
// "Downloads") and passes every want to handleDownloadNotificationWant() from
// EntryAbility.onCreate and onNewWant, since only the shell's ability sees the
// launch a notification tap causes.

import { common, Want, wantAgent, WantAgent, wantConstant } from '@kit.AbilityKit';
import { notificationManager } from '@kit.NotificationKit';
import { fileUri } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { DownloadItem } from '../components/webwindow/ShellServiceTypes';
import { numericId } from './NotificationService';

const DOMAIN: number = 0x0000;
const TAG: string = 'DownloadNotifier';
// Separates these from web notifications, which share the numeric id space.
const LABEL: string = 'chromium-download';
const PATH_KEY: string = 'chromiumDownloadPath';
const MIME_KEY: string = 'chromiumDownloadMime';
// The shade redraws on every publish; once a second is enough for progress.
const PROGRESS_INTERVAL_MS: number = 1000;

// Opens the file a "download complete" notification points at. Returns true
// when the want was such a tap, which the shell then treats as handled.
export function handleDownloadNotificationWant(context: common.UIAbilityContext,
  want: Want): boolean {
  const path = want.parameters?.[PATH_KEY];
  if (typeof path !== 'string' || path.length === 0) {
    return false;
  }
  const mime = want.parameters?.[MIME_KEY];
  openDownloadedFile(context, path, typeof mime === 'string' ? mime : '');
  return true;
}

// Hands the file to whichever app views its type; the system shows a chooser
// when there are several.
export function openDownloadedFile(context: common.UIAbilityContext, path: string,
  mimeType: string): void {
  const openWant: Want = {
    action: 'ohos.want.action.viewData',
    uri: fileUri.getUriFromPath(path),
    type: mimeType.length > 0 ? mimeType : undefined,
    flags: wantConstant.Flags.FLAG_AUTH_READ_URI_PERMISSION
  };
  context.startAbility(openWant).catch((error: Error): void => {
    hilog.warn(DOMAIN, TAG, 'Cannot open %{public}s: %{public}s', path, JSON.stringify(error));
  });
}

function formatBytes(bytes: number): string {
  if (bytes < 0) {
    return '';
  }
  const units: string[] = ['B', 'KB', 'MB', 'GB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}

export class DownloadNotifier {
  private readonly context: common.UIAbilityContext;
  // Keyed by download id: when progress was last published, and in which state.
  private readonly lastPublish: Map<string, number> = new Map();
  private readonly lastState: Map<string, string> = new Map();
  private templateSupported: boolean | undefined = undefined;
  // Publishing is asynchronous (permission and template checks first), so
  // publishes are chained: otherwise a slow "downloading" notification could
  // land after, and cover, the "download complete" one sent later.
  private queue: Promise<void> = Promise.resolve();
  private permissionAsked: boolean = false;

  constructor(context: common.UIAbilityContext) {
    this.context = context;
  }

  update(item: DownloadItem): void {
    const id = numericId(item.id);
    if (item.removed || item.state === 'cancelled') {
      this.forget(item.id);
      this.enqueue(() => notificationManager.cancel(id, LABEL));
      return;
    }
    const stateChanged = this.lastState.get(item.id) !== item.state;
    const now = Date.now();
    if (!stateChanged && item.state === 'inProgress' &&
      now - (this.lastPublish.get(item.id) ?? 0) < PROGRESS_INTERVAL_MS) {
      return;
    }
    this.lastState.set(item.id, item.state);
    this.lastPublish.set(item.id, now);
    this.enqueue(() => this.publish(id, item));
  }

  private enqueue(task: () => Promise<void>): void {
    this.queue = this.queue.then(task).catch((error: Error): void => {
      hilog.warn(DOMAIN, TAG, 'Download notification failed: %{public}s', JSON.stringify(error));
    });
  }

  private forget(downloadId: string): void {
    this.lastPublish.delete(downloadId);
    this.lastState.delete(downloadId);
  }

  private async publish(id: number, item: DownloadItem): Promise<void> {
    await this.ensureEnabled();
    const name = item.fileName.length > 0 ? item.fileName : item.url;
    let title = '正在下载';
    let text = name;
    let template: notificationManager.NotificationTemplate | undefined = undefined;
    if (item.state === 'completed') {
      title = '下载完成';
      text = `${name}  ${formatBytes(item.totalBytes > 0 ? item.totalBytes : item.receivedBytes)}`;
    } else if (item.state === 'failed') {
      title = '下载失败';
      text = item.failReason.length > 0 ? `${name}（${item.failReason}）` : name;
    } else if (item.dangerous) {
      title = '下载需要确认';
      text = `${name} 可能有害，请在下载页中选择保留或丢弃`;
    } else {
      title = item.state === 'paused' ? '已暂停' : '正在下载';
      text = item.totalBytes > 0 ?
        `${name}  ${formatBytes(item.receivedBytes)} / ${formatBytes(item.totalBytes)}` :
        `${name}  ${formatBytes(item.receivedBytes)}`;
      template = await this.progressTemplate(name, item);
    }
    const request: notificationManager.NotificationRequest = {
      id: id,
      label: LABEL,
      content: {
        notificationContentType: notificationManager.ContentType.NOTIFICATION_CONTENT_BASIC_TEXT,
        normal: { title: title, text: text }
      },
      tapDismissed: item.state === 'completed',
      wantAgent: await this.tapAgent(id, item)
    };
    // Set only when present: publish() rejects an explicit undefined (401).
    if (template) {
      request.template = template;
    }
    await notificationManager.publish(request);
  }

  private async progressTemplate(name: string,
    item: DownloadItem): Promise<notificationManager.NotificationTemplate | undefined> {
    if (item.totalBytes <= 0) {
      return undefined;
    }
    if (this.templateSupported === undefined) {
      try {
        this.templateSupported = await notificationManager.isSupportTemplate('downloadTemplate');
      } catch (error) {
        this.templateSupported = false;
      }
    }
    if (!this.templateSupported) {
      return undefined;
    }
    const percent = Math.min(100, Math.floor(item.receivedBytes * 100 / item.totalBytes));
    const data: Record<string, Object> = {
      'title': '正在下载',
      'fileName': name,
      'progressValue': percent
    };
    return { name: 'downloadTemplate', data: data };
  }

  // A finished download opens its file; anything else brings the browser back.
  private tapAgent(id: number, item: DownloadItem): Promise<WantAgent> {
    const parameters: Record<string, Object> = {};
    if (item.state === 'completed' && item.filePath.length > 0) {
      parameters[PATH_KEY] = item.filePath;
      parameters[MIME_KEY] = item.mimeType;
    }
    const want: Want = {
      bundleName: this.context.abilityInfo.bundleName,
      abilityName: this.context.abilityInfo.name,
      parameters: parameters
    };
    const info: wantAgent.WantAgentInfo = {
      wants: [want],
      actionType: wantAgent.OperationType.START_ABILITY,
      requestCode: id,
      actionFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG]
    };
    return wantAgent.getWantAgent(info);
  }

  private async ensureEnabled(): Promise<void> {
    if (this.permissionAsked) {
      return;
    }
    this.permissionAsked = true;
    try {
      if (!await notificationManager.isNotificationEnabled()) {
        await notificationManager.requestEnableNotification(this.context);
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'Notifications not enabled: %{public}s', JSON.stringify(error));
    }
  }
}
