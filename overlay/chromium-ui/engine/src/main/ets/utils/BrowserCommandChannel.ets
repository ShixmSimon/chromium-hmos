/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

import { BrowserCommandPayload } from '../components/webwindow/NativeContext';

export type BrowserCommandFlush = (text: string) => void;

// One command as the channel writes it: numbered, so WebWindow can tell the
// ones it has already run from the ones it has not.
export interface SequencedBrowserCommand {
  seq: number;
  command: BrowserCommandPayload;
}

// What the channel writes to browserCommandText. `channel` names the channel
// instance, so a WebWindow handed commands by a new one starts counting again.
export interface SequencedBrowserCommandBatch {
  channel: string;
  sequencedCommands: SequencedBrowserCommand[];
}

interface RecentCommand {
  sentAt: number;
  entry: SequencedBrowserCommand;
}

// How long a command stays in what the channel writes. Two writes coalesce
// only within one frame, so this is far more than they need; it is short so
// that a WebWindow rebuilt later is not handed a long run of old commands.
const RECENT_COMMAND_MS: number = 1000;

let nextChannelId: number = 1;

/**
 * Carries browser commands to the native side without losing any.
 *
 * A page hands commands to WebWindow through a pair of ArkUI state fields, and
 * ArkUI collapses everything written to them before WebWindow's monitor runs
 * into a single notification. A handler that sent two commands back to back
 * therefore delivered only the later one -- silently, since the state write
 * itself succeeded. Reporting the system permission and then answering the
 * request that changed it lost the report that way, and Chromium spent the
 * rest of the run believing the app held no location permission.
 *
 * Batching the commands of one synchronous block was not enough: two blocks
 * can still write before the monitor runs, and the second batch replaced the
 * first. On a phone, getExtensionActions sent while an extension was being
 * uninstalled vanished that way, and the shell waited for replies that were
 * never asked for.
 *
 * So every command is numbered, and each write carries every command sent in
 * the last second, not just the newest batch. WebWindow runs only the ones
 * numbered past the last it ran, so a write that replaced another loses
 * nothing. A shell that writes browserCommandText itself, without this
 * channel, is unaffected.
 */
export class BrowserCommandChannel {
  private readonly channelId: string =
    `${Date.now()}-${nextChannelId++}`;
  private recent: RecentCommand[] = [];
  private nextSeq: number = 1;
  private flushScheduled: boolean = false;
  private readonly flush: BrowserCommandFlush;

  constructor(flush: BrowserCommandFlush) {
    this.flush = flush;
  }

  send(command: BrowserCommandPayload): void {
    const entry: SequencedBrowserCommand = { seq: this.nextSeq, command: command };
    this.nextSeq += 1;
    const recent: RecentCommand = { sentAt: Date.now(), entry: entry };
    this.recent = [...this.recent, recent];
    if (this.flushScheduled) {
      return;
    }
    this.flushScheduled = true;
    // A microtask, so the batch closes at the end of the current block rather
    // than at the end of the frame: a command is delayed by as little as
    // correctness allows.
    Promise.resolve().then((): void => {
      this.drain();
    });
  }

  private drain(): void {
    this.flushScheduled = false;
    const oldest: number = Date.now() - RECENT_COMMAND_MS;
    // The newest command stays however old it is: it may be the one this
    // write exists to deliver.
    this.recent = this.recent.filter((recent: RecentCommand, index: number): boolean =>
      recent.sentAt >= oldest || index === this.recent.length - 1);
    if (this.recent.length === 0) {
      return;
    }
    const batch: SequencedBrowserCommandBatch = {
      channel: this.channelId,
      sequencedCommands: this.recent.map((recent: RecentCommand): SequencedBrowserCommand => recent.entry)
    };
    this.flush(JSON.stringify(batch));
  }
}
