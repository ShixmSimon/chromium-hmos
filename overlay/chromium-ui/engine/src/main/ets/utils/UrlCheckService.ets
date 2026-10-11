/* Copyright 2026 The Chromium Authors. BSD-style license: LICENSE. */

// SDK-independent shell service. The shell supplies a Huawei executor and its
// own consent check; merely registering this service never authorizes a URL.
// One coordinator per app shares SDK concurrency across window factories.
export interface UrlThreatSdkEntry {
  url: string;
  threat: number;
}
export interface UrlThreatSdkResponse {
  results: UrlThreatSdkEntry[];
}
export interface UrlThreatExecutor {
  check(url: string): Promise<UrlThreatSdkResponse>;
}
export interface UrlThreatAuthorization {
  // Must read the shell's actual consent, not trust the engine's boolean.
  isAllowed(contextId: string, incognito: boolean): boolean;
}
export interface UrlCheckOptions {
  supported: boolean;
  // True only after the shell has verified origin_path with the official SDK.
  originPathVerified: boolean;
  maxConcurrent: number;
}
interface Policy {
  revision: number;
  enabled: boolean;
  provider: string;
  consent: boolean;
  incognito: boolean;
}
interface CheckResult {
  protocolVersion: number;
  requestId: string;
  contextId: string;
  policyRevision: number;
  verdict: string;
  reason: string;
  checkedUrl: string;
  elapsedMs: number;
  sdkCode: number;
}
interface ActiveCheck {
  contextId: string;
  finish: (verdict: string, reason: string, sdkCode: number) => void;
}
interface SdkError {
  code?: number;
}
interface CapabilitiesResult {
  protocolVersion: number;
  supported: boolean;
  configuration: string;
  queryModes: string[];
  reason: string;
}
interface ControlResult {
  accepted?: boolean;
  cancelled?: boolean;
  closed?: boolean;
  reason?: string;
}

function fieldString(args: Record<string, Object>, key: string): string {
  const value = args[key];
  return typeof value === 'string' ? value : '';
}
function fieldNumber(args: Record<string, Object>, key: string): number {
  const value = args[key];
  return typeof value === 'number' && Number.isFinite(value) ? value : -1;
}
function codeReason(code: number): string {
  switch (code) {
    case 201: return 'permission_denied';
    case 401: return 'invalid_request';
    case 801: return 'unsupported';
    case 1010800002: return 'network_unavailable';
    case 1010800003: return 'cloud_unavailable';
    case 1010800005: return 'concurrency_limited';
    case 1010800006: return 'rate_limited';
    case 1010800007: return 'timeout';
    case 1010800008: return 'cloud_quota_exceeded';
    default: return 'internal_error';
  }
}

export class UrlCheckCoordinator {
  private readonly policies: Map<string, Policy> = new Map();
  private readonly active: Map<string, ActiveCheck> = new Map();
  private sdkInFlight: number = 0;
  private coolingUntil: number = 0;
  private disposed: boolean = false;
  private readonly concurrencyLimit: number;
  private readonly executor: UrlThreatExecutor;
  private readonly authorization: UrlThreatAuthorization;
  private readonly options: UrlCheckOptions;

  constructor(executor: UrlThreatExecutor,
    authorization: UrlThreatAuthorization, options: UrlCheckOptions) {
    this.executor = executor;
    this.authorization = authorization;
    this.options = options;
    this.concurrencyLimit = Number.isSafeInteger(options.maxConcurrent) ?
      Math.max(1, Math.min(10, options.maxConcurrent)) : 1;
  }

  async handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    if (method === 'capabilities') {
      return { protocolVersion: 1, supported: this.options.supported,
        configuration: this.options.originPathVerified ? 'ready' : 'unverified',
        queryModes: this.options.originPathVerified ? ['origin_path'] : [],
        reason: this.options.originPathVerified ? '' : 'not_verified' } as CapabilitiesResult;
    }
    if (method === 'check') {
      return this.check(args);
    }
    const contextId = fieldString(args, 'contextId');
    const revision = fieldNumber(args, 'policyRevision');
    if (fieldNumber(args, 'protocolVersion') !== 1 || !contextId ||
      contextId.length > 128 || !Number.isSafeInteger(revision) || revision < 1 || this.disposed) {
      return { accepted: false, reason: 'invalid_request' } as ControlResult;
    }
    if (method === 'configure') {
      const old = this.policies.get(contextId);
      const provider = fieldString(args, 'provider');
      if ((old && revision <= old.revision) ||
        (provider !== 'local' && provider !== 'huawei' && provider !== 'both') ||
        typeof args['enabled'] !== 'boolean' || typeof args['cloudConsent'] !== 'boolean' ||
        typeof args['incognito'] !== 'boolean' || (!old && this.policies.size >= 128)) {
        return { accepted: false, reason: 'invalid_policy' } as ControlResult;
      }
      this.cancelContext(contextId, 'stale_policy');
      const policy: Policy = { revision: revision, enabled: args['enabled'] as boolean,
        provider: provider, consent: args['cloudConsent'] as boolean,
        incognito: args['incognito'] as boolean };
      this.policies.set(contextId, policy);
      return { accepted: true } as ControlResult;
    }
    if (method === 'cancel') {
      const requestId = fieldString(args, 'requestId');
      const check = this.active.get(requestId);
      if (check && check.contextId === contextId) {
        check.finish('unknown', 'cancelled', 0);
        return { cancelled: true } as ControlResult;
      }
      return { cancelled: false } as ControlResult;
    }
    if (method === 'closeContext') {
      this.cancelContext(contextId, 'context_closed');
      this.policies.delete(contextId);
      return { closed: true } as ControlResult;
    }
    return { accepted: false, reason: 'unsupported_method' } as ControlResult;
  }

  private cancelContext(contextId: string, reason: string): void {
    const ids: string[] = [];
    this.active.forEach((check: ActiveCheck, id: string): void => {
      if (check.contextId === contextId) ids.push(id);
    });
    ids.forEach((id: string): void => {
      this.active.get(id)?.finish('unknown', reason, 0);
    });
  }

  // Call immediately on consent withdrawal, before waiting for native ack.
  revokeContext(contextId: string): void {
    const policy = this.policies.get(contextId);
    if (policy) policy.consent = false;
    this.cancelContext(contextId, 'consent_required');
  }

  dispose(): void {
    this.disposed = true;
    const ids: string[] = [];
    this.policies.forEach((_policy: Policy, id: string): void => { ids.push(id); });
    ids.forEach((id: string): void => { this.cancelContext(id, 'context_closed'); });
    this.policies.clear();
  }

  private check(args: Record<string, Object>): Promise<CheckResult> {
    const started = Date.now();
    const id = fieldString(args, 'requestId');
    const contextId = fieldString(args, 'contextId');
    const revision = fieldNumber(args, 'policyRevision');
    const url = fieldString(args, 'url');
    const timeout = fieldNumber(args, 'timeoutMs');
    const result: CheckResult = { protocolVersion: 1, requestId: id,
      contextId: contextId, policyRevision: revision, verdict: 'unknown', reason: '',
      checkedUrl: '', elapsedMs: 0, sdkCode: 0 };
    let failure = '';
    const policy = this.policies.get(contextId);
    if (this.disposed) failure = 'context_closed';
    else if (fieldNumber(args, 'protocolVersion') !== 1) failure = 'unsupported_protocol';
    else if (!id || id.length > 128 || this.active.has(id) ||
      !Number.isSafeInteger(revision) || !Number.isSafeInteger(timeout) || timeout <= 0 ||
      timeout > 1500 || !this.validCloudUrl(url)) failure = 'invalid_request';
    else if (!policy || revision !== policy.revision) failure = 'stale_policy';
    else if (!policy.enabled || policy.provider === 'local') failure = 'disabled';
    else if (!policy.consent || !this.authorized(contextId, policy.incognito)) failure = 'consent_required';
    else if (!this.options.supported) failure = 'unsupported';
    else if (!this.options.originPathVerified || fieldString(args, 'queryMode') !== 'origin_path')
      failure = 'unsupported_query_mode';
    else if (this.sdkInFlight >= this.concurrencyLimit)
      failure = 'concurrency_limited';
    else if (Date.now() < this.coolingUntil) failure = 'rate_limited';
    if (failure) {
      result.reason = failure;
      return Promise.resolve(result);
    }
    result.checkedUrl = url;
    return new Promise<CheckResult>((resolve): void => {
      let completed = false;
      let timer = -1;
      const finish = (verdict: string, reason: string, sdkCode: number): void => {
        if (completed) return;
        completed = true;
        if (timer !== -1) clearTimeout(timer);
        this.active.delete(id);
        result.verdict = verdict;
        result.reason = reason;
        result.sdkCode = sdkCode;
        result.elapsedMs = Math.max(0, Date.now() - started);
        resolve(result);
      };
      this.active.set(id, { contextId: contextId, finish: finish });
      timer = setTimeout((): void => { finish('unknown', 'timeout', 0); }, timeout);
      this.sdkInFlight++;
      let pending: Promise<UrlThreatSdkResponse>;
      try {
        pending = this.executor.check(url);
      } catch (_error) {
        this.sdkInFlight--;
        finish('unknown', 'internal_error', 0);
        return;
      }
      pending.then((response: UrlThreatSdkResponse): void => {
        this.sdkInFlight--;
        if (completed) return;
        const current = this.policies.get(contextId);
        if (!current || current.revision !== revision || !current.consent ||
          !this.authorized(contextId, current.incognito)) {
          finish('unknown', 'stale_policy', 0);
          return;
        }
        if (!response || !Array.isArray(response.results) || response.results.length !== 1 ||
          !response.results[0] || response.results[0].url !== url) {
          finish('unknown', 'invalid_response', 0);
          return;
        }
        switch (response.results[0].threat) {
          case 0: finish('no_match', 'no_match', 0); break;
          case 1: finish('malware', 'matched', 0); break;
          case 2: finish('phishing', 'matched', 0); break;
          case 3: finish('other', 'other_category', 0); break;
          default: finish('unknown', 'invalid_response', 0);
        }
      }, (error: SdkError): void => {
        this.sdkInFlight--;
        const code = error && typeof error.code === 'number' ? error.code : 0;
        if (code === 1010800005 || code === 1010800006 || code === 1010800008)
          this.coolingUntil = Date.now() + 30000;
        finish('unknown', codeReason(code), code);
      });
    });
  }

  private authorized(contextId: string, incognito: boolean): boolean {
    try { return this.authorization.isAllowed(contextId, incognito); }
    catch (_error) { return false; }
  }

  private validCloudUrl(url: string): boolean {
    // Native GURL is authoritative. This boundary accepts only canonical
    // ASCII public DNS names and the advertised origin_path subset; IPv6,
    // Unicode and encoded/token-looking paths remain unchecked by this v1
    // shell adapter instead of being sent under ambiguous parsing semantics.
    if (url.length > 4096 || /[^\x21-\x7e]/.test(url) || /[?#@\\]/.test(url)) return false;
    const match = /^https?:\/\/([a-z0-9.-]+)(?::([0-9]+))?(\/.*)$/.exec(url);
    if (!match) return false;
    const host = match[1];
    const path = match[3];
    if (!host.includes('.') || host.endsWith('.') || /^[0-9.]+$/.test(host) ||
      host.endsWith('.localhost') || host.endsWith('.local') ||
      host.endsWith('.internal') || host.endsWith('.home.arpa')) return false;
    if (path.length > 256 || /%|token|oauth/i.test(path)) return false;
    return !path.split('/').some((segment: string): boolean => segment.length > 48);
  }
}
