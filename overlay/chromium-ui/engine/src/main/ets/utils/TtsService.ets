/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// speechSynthesis for Chromium, on CoreSpeechKit's offline engine.
//
// Methods (see content/browser/speech/tts_ohos.cc):
//   voices                     -> { voices: [{ name, lang, person, gender }] }
//   speak { utteranceId, text, lang, person?, rate, pitch, volume } -> {}
//   stop                       -> {}
//   isSpeaking                 -> { speaking }
// Events: start / end / error { utteranceId, message? }.
//
// The engine takes its language and voice at creation, so one engine is kept
// per voice and replaced when a request asks for another. It has no pause.

import { textToSpeech } from '@kit.CoreSpeechKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { SystemServiceEmitter, SystemServiceHandler } from './SystemServiceBridge';

interface VoiceEntry {
  name: string;
  lang: string;
  person: number;
  gender: string;
}

// What CoreSpeechKit ships with, for when listVoices is unavailable. Only
// voices already on the device can speak; 13 is the preinstalled Chinese one.
const FALLBACK_VOICES: VoiceEntry[] = [
  { name: '聆小珊', lang: 'zh-CN', person: 13, gender: 'Female' }
];

function clamp(value: number, low: number, high: number, fallback: number): number {
  if (typeof value !== 'number' || isNaN(value)) {
    return fallback;
  }
  return Math.min(high, Math.max(low, value));
}

function normalizeLang(lang: string): string {
  const lower = (lang ?? '').toLowerCase();
  return lower.startsWith('en') ? 'en-US' : 'zh-CN';
}

export class TtsService implements SystemServiceHandler {
  private engine: textToSpeech.TextToSpeechEngine | null = null;
  private engineKey: string = '';
  private requestSerial: number = 0;
  // requestId -> utteranceId, so listener callbacks can name the utterance.
  private readonly utterances: Map<string, number> = new Map();

  private readonly emit: SystemServiceEmitter;

  constructor(emit: SystemServiceEmitter) {
    this.emit = emit;
  }

  handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    switch (method) {
      case 'voices':
        return this.voices();
      case 'speak':
        return this.speak(args);
      case 'stop':
        this.engine?.stop();
        return Promise.resolve(undefined);
      case 'isSpeaking': {
        const speaking: boolean = this.engine ? this.engine.isBusy() : false;
        const result: Record<string, Object> = { 'speaking': speaking };
        return Promise.resolve(result);
      }
      default:
        return Promise.reject(new Error(`tts has no method ${method}`));
    }
  }

  private async voices(): Promise<Object> {
    let found: VoiceEntry[] = [];
    try {
      const query: textToSpeech.VoiceQuery = { requestId: `voices-${Date.now()}`, online: 1 };
      const infos: textToSpeech.VoiceInfo[] = await textToSpeech.listVoices(query);
      found = infos
        .filter((info: textToSpeech.VoiceInfo): boolean =>
          info.status === undefined || info.status === 'INSTALLED')
        .map((info: textToSpeech.VoiceInfo): VoiceEntry => ({
          name: info.description && info.description.length > 0 ? info.description :
            `${info.language} ${info.person}`,
          lang: info.language.replace('_', '-'),
          person: info.person,
          gender: info.gender ?? ''
        }));
    } catch (error) {
      found = [];
    }
    const result: Record<string, Object> = { 'voices': found.length > 0 ? found : FALLBACK_VOICES };
    return result;
  }

  private async speak(args: Record<string, Object>): Promise<Object | undefined> {
    const utteranceId = Number(args['utteranceId'] ?? 0);
    const text = String(args['text'] ?? '');
    const lang = normalizeLang(String(args['lang'] ?? ''));
    const person = args['person'] !== undefined ? Number(args['person']) :
      (lang === 'en-US' ? 8 : 13);
    const engine = await this.engineFor(lang, person);

    const requestId = `chromium-${utteranceId}-${++this.requestSerial}`;
    this.utterances.set(requestId, utteranceId);
    const extraParams: Record<string, Object> = {
      // Chromium queues utterances itself and sends the next one after "end";
      // preempting keeps a stop-then-speak from waiting behind the old one.
      'queueMode': 1,
      'speed': clamp(Number(args['rate']), 0.5, 2, 1),
      'pitch': clamp(Number(args['pitch']), 0.5, 2, 1),
      'volume': clamp(Number(args['volume']), 0, 2, 1),
      'languageContext': lang
    };
    const params: textToSpeech.SpeakParams = { requestId: requestId, extraParams: extraParams };
    engine.speak(text, params);
    return undefined;
  }

  private engineFor(lang: string, person: number): Promise<textToSpeech.TextToSpeechEngine> {
    const key = `${lang}:${person}`;
    if (this.engine && this.engineKey === key) {
      return Promise.resolve(this.engine);
    }
    if (this.engine) {
      this.engine.shutdown();
      this.engine = null;
    }
    const params: textToSpeech.CreateEngineParams = {
      language: lang,
      person: person,
      online: 1,
      extraParams: { 'style': 'interaction-broadcast', 'locate': 'CN', 'name': 'ChromiumTts' }
    };
    return new Promise((resolve, reject) => {
      textToSpeech.createEngine(params, (err: BusinessError, engine: textToSpeech.TextToSpeechEngine) => {
        if (err) {
          reject(new Error(`createEngine failed: ${err.message} (code ${err.code})`));
          return;
        }
        this.engine = engine;
        this.engineKey = key;
        engine.setListener(this.listener());
        resolve(engine);
      });
    });
  }

  private listener(): textToSpeech.SpeakListener {
    return {
      onStart: (requestId: string, _response: textToSpeech.StartResponse): void => {
        this.emitFor('start', requestId);
      },
      onComplete: (requestId: string, response: textToSpeech.CompleteResponse): void => {
        // type 0 is "synthesis done", 1 is "playback done"; only the latter
        // is the end of the utterance for the page.
        if (response.type === 1) {
          this.emitFor('end', requestId);
          this.utterances.delete(requestId);
        }
      },
      onStop: (requestId: string, _response: textToSpeech.StopResponse): void => {
        // Chromium sent the stop and reports the interruption itself.
        this.utterances.delete(requestId);
      },
      onData: (_requestId: string, _audio: ArrayBuffer, _response: textToSpeech.SynthesisResponse): void => {
      },
      onError: (requestId: string, errorCode: number, errorMessage: string): void => {
        this.emitFor('error', requestId, `${errorMessage} (code ${errorCode})`);
        this.utterances.delete(requestId);
      }
    };
  }

  private emitFor(event: string, requestId: string, message?: string): void {
    const utteranceId = this.utterances.get(requestId);
    if (utteranceId === undefined) {
      return;
    }
    const data: Record<string, Object> = { 'utteranceId': utteranceId };
    if (message !== undefined) {
      data['message'] = message;
    }
    this.emit(event, data);
  }
}
