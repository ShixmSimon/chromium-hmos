// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
import { ShellServiceEvent } from './ShellServiceTypes';

export type TranslationStyle = 'standard' | 'quote' | 'gray';
export type TranslationStatus = 'idle' | 'waiting' | 'translating' | 'paused' | 'error' | 'stopped';
export type TranslationReleaseReason = 'providerError' | 'rateLimited' | 'authentication' | 'quota' | 'cancelled' | 'timeout';
export interface TranslationIdentity {
  contextId: string;
  tabId: string;
  navigationId: string;
  sessionId: string;
}
export interface TranslationSegmentIdentity {
  frameId: string;
  frameDocumentId: string;
  segmentId: string;
  segmentRevision: number;
}
export interface TranslationSegment extends TranslationSegmentIdentity {
  text: string;
  priority: 'visible' | 'prefetch';
}
export interface TranslationResult extends TranslationSegmentIdentity {
  translatedText: string;
}
export interface TranslationItemResult {
  segmentId: string;
  segmentRevision: number;
  ok: boolean;
  reason: string;
}
export interface TranslationLimits {
  maxSegments: number;
  maxTextUnits: number;
  maxSegmentUnits: number;
  maxResultSegmentUnits: number;
  maxResultBatchUnits: number;
  maxRetainedTextUnits: number;
  maxSessionSegments: number;
  maxTrackedNodes: number;
  maxInFlightBatches: number;
  leaseMs: number;
  maxRetries: number;
  scrollSettleMs: number;
  taskBudgetMs: number;
  maxPrefetchViewportRatio: number;
}
export interface TranslationCapabilitiesEvent extends ShellServiceEvent {
  requestId: number;
  protocolVersion: number;
  supported: boolean;
  frameScope: 'mainFrame';
  styles: TranslationStyle[];
  limits: TranslationLimits;
}
export interface TranslationEvent extends ShellServiceEvent, TranslationIdentity {
  protocolVersion: number;
  requestId?: number;
  sequence: number;
  ok?: boolean;
  reason?: string;
  supported?: boolean;
  frameScope?: 'mainFrame';
  state?: TranslationStatus;
  style?: TranslationStyle;
  showTranslation?: boolean;
  discovered?: number;
  inFlight?: number;
  translated?: number;
  failed?: number;
  hasMore?: boolean;
  limitReached?: boolean;
  retainedTextUnits?: number;
  batchId?: string;
  leaseMs?: number;
  segments?: TranslationSegment[];
  applied?: number;
  rejected?: number;
  results?: TranslationItemResult[];
}
