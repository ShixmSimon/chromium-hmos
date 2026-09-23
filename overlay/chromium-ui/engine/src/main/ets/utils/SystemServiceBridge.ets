/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// The ArkTS end of components/ohos_system_service: HarmonyOS services that
// have no C interface -- text-to-speech, notifications, barcode and face
// detection, biometric authentication, window orientation -- answered here on
// Chromium's behalf.
//
// Every request gets exactly one reply, including for a service nobody
// registered and for a handler that throws: Chromium holds a callback per
// request and a missing reply is a Web API promise that never settles.

import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0x0000;
const TAG: string = 'SystemService';

export interface SystemServiceNativePort {
  SetSystemServiceCallback(componentId: string, callback: (requestJson: string) => void): void;
  CompleteSystemServiceMessage(componentId: string, messageJson: string): void;
}

export interface SystemServiceRequest {
  id: number;
  service: string;
  method: string;
  args: Record<string, Object>;
}

// Pushes an unsolicited event to Chromium, e.g. "the utterance finished".
export type SystemServiceEmitter = (event: string, data: Record<string, Object>) => void;

export interface SystemServiceHandler {
  // Resolves with the result Chromium receives, or rejects with its error.
  handle(method: string, args: Record<string, Object>): Promise<Object | undefined>;
}

export type SystemServiceHandlerFactory = (emit: SystemServiceEmitter) => SystemServiceHandler;

interface SystemServiceReply {
  id: number;
  ok: boolean;
  result?: Object;
  error?: string;
}

interface SystemServiceEvent {
  service: string;
  event: string;
  data: Record<string, Object>;
}

export class SystemServiceBridge {
  private readonly handlers: Map<string, SystemServiceHandler> = new Map();
  private readonly componentId: string;
  private readonly port: SystemServiceNativePort;

  constructor(componentId: string, port: SystemServiceNativePort) {
    this.componentId = componentId;
    this.port = port;
    try {
      this.port.SetSystemServiceCallback(componentId, (requestJson: string): void => {
        this.onRequest(requestJson);
      });
    } catch (error) {
      // An engine built before the bridge existed: every service simply
      // stays unavailable, which is how Chromium already behaves without it.
      hilog.warn(DOMAIN, TAG, 'engine has no system service bridge: %{public}s', String(error));
    }
  }

  register(service: string, factory: SystemServiceHandlerFactory): void {
    const emit: SystemServiceEmitter = (event: string, data: Record<string, Object>): void => {
      const message: SystemServiceEvent = { service: service, event: event, data: data };
      this.send(message);
    };
    this.handlers.set(service, factory(emit));
  }

  private onRequest(requestJson: string): void {
    let request: SystemServiceRequest;
    try {
      request = JSON.parse(requestJson) as SystemServiceRequest;
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'malformed request: %{public}s', String(error));
      return;
    }
    const handler = this.handlers.get(request.service);
    if (!handler) {
      this.reply({ id: request.id, ok: false, error: `No HarmonyOS handler for ${request.service}` });
      return;
    }
    let pending: Promise<Object | undefined>;
    try {
      pending = handler.handle(request.method, request.args ?? {});
    } catch (error) {
      this.reply({ id: request.id, ok: false, error: SystemServiceBridge.describe(error) });
      return;
    }
    pending.then((result: Object | undefined): void => {
      this.reply(result === undefined ? { id: request.id, ok: true } :
        { id: request.id, ok: true, result: result });
    }).catch((error: Object): void => {
      hilog.warn(DOMAIN, TAG, '%{public}s.%{public}s failed: %{public}s',
        request.service, request.method, SystemServiceBridge.describe(error));
      this.reply({ id: request.id, ok: false, error: SystemServiceBridge.describe(error) });
    });
  }

  private reply(reply: SystemServiceReply): void {
    this.send(reply);
  }

  private send(message: SystemServiceReply | SystemServiceEvent): void {
    try {
      this.port.CompleteSystemServiceMessage(this.componentId, JSON.stringify(message));
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'could not deliver a message: %{public}s', String(error));
    }
  }

  static describe(error: Object): string {
    const withMessage = error as Error;
    if (withMessage && typeof withMessage.message === 'string' && withMessage.message.length > 0) {
      const withCode = error as Record<string, Object>;
      return withCode['code'] !== undefined ? `${withMessage.message} (code ${withCode['code']})` :
        withMessage.message;
    }
    return String(error);
  }
}
