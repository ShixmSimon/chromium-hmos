// Synthetic-page-only shell test helper. Not enabled or shipped as a provider.
// Copy into an external shell debug module; imports use the new engine HAR.
import {
  BrowserCommandPayload, BrowserRuntimeEvent, TranslationEvent,
  TranslationSegment, TranslationResult, TranslationStyle, shellEvent
} from 'engine';

type SendTranslationCommand = (command: BrowserCommandPayload) => void;
type TranslationReply = (event: TranslationEvent) => void;
export type TranslationProbeFault = 'none' | 'delay' | 'reverse' | 'oversize' |
  'authentication' | 'quota' | 'drop' | 'wrongIdentity';

// Route the shell's runtime events into onEvent(). Explicitly start/pump from
// a debug UI after opening the synthetic fixture. There are no network calls.
export class ViewportTranslationProbe {
  private nextRequest: number = 730000;
  private readonly pending: Map<number, TranslationReply> = new Map();
  private session?: TranslationEvent;
  private readonly send: SendTranslationCommand;
  fakeProviderCalls: number = 0;
  lastReply?: TranslationEvent;

  constructor(send: SendTranslationCommand) { this.send = send; }

  onEvent(raw: BrowserRuntimeEvent): void {
    if (!raw.event.startsWith('translation')) { return; }
    const e: TranslationEvent = shellEvent<TranslationEvent>(raw);
    if (e.event === 'translationSessionEnded' && e.sessionId === this.session?.sessionId) {
      this.session = undefined;
    }
    if (e.requestId !== undefined) {
      const resolve: TranslationReply | undefined = this.pending.get(e.requestId);
      if (resolve !== undefined) {
        this.pending.delete(e.requestId);
        this.lastReply = e;
        resolve(e);
      }
    }
  }

  private request(command: BrowserCommandPayload): Promise<TranslationEvent> {
    const id: number = ++this.nextRequest;
    command.requestId = id;
    return new Promise<TranslationEvent>((resolve, reject): void => {
      const timer: number = setTimeout((): void => {
        this.pending.delete(id);
        reject(new Error('translationReplyTimeout'));
      }, 7000);
      this.pending.set(id, (reply: TranslationEvent): void => {
        clearTimeout(timer);
        resolve(reply);
      });
      this.send(command);
    });
  }

  private command(name: string, session: TranslationEvent): BrowserCommandPayload {
    return { command: name, contextId: session.contextId, tabId: session.tabId,
      navigationId: session.navigationId, sessionId: session.sessionId };
  }

  async start(tabId: string): Promise<TranslationEvent> {
    const caps: TranslationEvent = await this.request({ command: 'getTranslationCapabilities' });
    if (caps.supported !== true) { throw new Error('translationUnsupported'); }
    const state: TranslationEvent = await this.request({ command: 'getTranslationState', tabId: tabId });
    const start: BrowserCommandPayload = this.command('startTranslation', state);
    start.sourceLanguage = 'en'; start.targetLanguage = 'zh-CN';
    start.style = 'standard'; start.prefetchViewportRatio = 0;
    const reply: TranslationEvent = await this.request(start);
    if (reply.ok === true) { this.session = reply; }
    return reply;
  }

  // Call twice without awaiting to exercise two provider responses in flight.
  // A native 'busy' response means retry this take after 300–500 ms. It is not
  // a provider call and must not trigger network work in a real provider.
  async pump(fault: TranslationProbeFault = 'none', delayMs: number = 2000): Promise<TranslationEvent> {
    const session: TranslationEvent | undefined = this.session;
    if (session === undefined) { throw new Error('translationNotStarted'); }
    const take: BrowserCommandPayload = this.command('takeTranslationBatch', session);
    take.maxSegments = 4; take.maxTextUnits = 8000;
    const batch: TranslationEvent = await this.request(take);
    if (batch.ok !== true || batch.segments === undefined || batch.segments.length === 0) { return batch; }
    this.fakeProviderCalls += 1;
    if (fault === 'drop') { return batch; } // User waits >45s, checks lease event/state.
    if (fault === 'delay') {
      await new Promise<void>((resolve): void => { setTimeout(resolve, delayMs); });
    }
    if (fault === 'authentication' || fault === 'quota') {
      const release: BrowserCommandPayload = this.command('releaseTranslationBatch', session);
      release.batchId = batch.batchId; release.reason = fault; release.retryable = false;
      return this.request(release);
    }
    const results: TranslationResult[] = batch.segments.map((segment: TranslationSegment): TranslationResult => {
      return { frameId: segment.frameId, frameDocumentId: segment.frameDocumentId,
        segmentId: segment.segmentId, segmentRevision: segment.segmentRevision,
        translatedText: fault === 'oversize' ? 'x'.repeat(8001) : `【测试译文】${segment.text}` };
    });
    if (fault === 'reverse') { results.reverse(); }
    if (fault === 'wrongIdentity' && results.length > 0) { results[0].frameDocumentId = 'invalid'; }
    const apply: BrowserCommandPayload = this.command('applyTranslationBatch', session);
    apply.batchId = batch.batchId; apply.segments = results;
    // Deliberately retain the old session for delayed-result rejection testing.
    return this.request(apply);
  }

  async presentation(style: TranslationStyle, show: boolean = true): Promise<TranslationEvent> {
    if (this.session === undefined) { throw new Error('translationNotStarted'); }
    const command: BrowserCommandPayload = this.command('setTranslationPresentation', this.session);
    command.style = style; command.showTranslation = show;
    return this.request(command);
  }

  async stop(): Promise<TranslationEvent> {
    if (this.session === undefined) { throw new Error('translationNotStarted'); }
    return this.request(this.command('stopTranslation', this.session));
  }
}
