/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

import { BrowserCommandPayload } from '../components/webwindow/NativeContext';

export type BrowserCommandFlush = (text: string) => void;

/**
 * Carries browser commands to the native side without losing any.
 *
 * A page hands commands to WebWindow through a pair of ArkUI state fields, and
 * ArkUI collapses everything written to them in one synchronous block into a
 * single notification. A handler that sent two commands back to back therefore
 * delivered only the later one -- silently, since the state write itself
 * succeeded. Reporting the system permission and then answering the request
 * that changed it lost the report that way, and Chromium spent the rest of the
 * run believing the app held no location permission.
 *
 * So commands accumulate until the block that produced them has finished, and
 * travel together as a JSON array. A lone command still travels as itself: the
 * engine and this shell are built separately and staged into one HAP by hand,
 * so an engine that predates the batch keeps working for everything except the
 * case that needed it.
 */
export class BrowserCommandChannel {
  private pending: BrowserCommandPayload[] = [];
  private flushScheduled: boolean = false;
  private readonly flush: BrowserCommandFlush;

  constructor(flush: BrowserCommandFlush) {
    this.flush = flush;
  }

  send(command: BrowserCommandPayload): void {
    this.pending = [...this.pending, command];
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
    if (this.pending.length === 0) {
      return;
    }
    const batch: BrowserCommandPayload[] = this.pending;
    this.pending = [];
    this.flush(JSON.stringify(batch.length === 1 ? batch[0] : batch));
  }
}
