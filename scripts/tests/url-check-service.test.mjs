import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { stripTypeScriptTypes } from 'node:module';
import test from 'node:test';

// Execute the actual SDK-independent ArkTS service, with an injected SDK.
// Native GURL, ArkTS compilation and real Huawei availability remain CI/device
// checks; these tests do not pretend that Node implements the Harmony SDK.
const source = await readFile(new URL('../../overlay/chromium-ui/engine/src/main/ets/utils/UrlCheckService.ets', import.meta.url), 'utf8');
const javascript = stripTypeScriptTypes(source, { mode: 'transform' });
const { UrlCheckCoordinator } = await import(`data:text/javascript;base64,${Buffer.from(javascript).toString('base64')}`);
const url = 'https://example.com/login';
const request = (id = 'r1', overrides = {}) => ({ protocolVersion: 1,
  requestId: id, contextId: 'ctx', policyRevision: 1, url,
  queryMode: 'origin_path', timeoutMs: 200, ...overrides });
const policy = (overrides = {}) => ({ protocolVersion: 1, contextId: 'ctx',
  policyRevision: 1, enabled: true, provider: 'huawei', cloudConsent: true,
  incognito: false, ...overrides });
async function fixture(check, options = {}, allowed = () => true) {
  const calls = [];
  const service = new UrlCheckCoordinator({ check: (value) => { calls.push(value); return check(value); } },
    { isAllowed: allowed }, { supported: true, originPathVerified: true, maxConcurrent: 2, ...options });
  await service.handle('configure', policy());
  return { service, calls };
}
const response = (threat, checkedUrl = url) => Promise.resolve({ results: [{ url: checkedUrl, threat }] });

for (const [code, verdict] of [[0, 'no_match'], [1, 'malware'], [2, 'phishing'], [3, 'other']]) {
  test(`SDK threat ${code} becomes ${verdict}`, async () => {
    const { service, calls } = await fixture(() => response(code));
    const result = await service.handle('check', request());
    assert.equal(result.verdict, verdict);
    assert.equal(result.checkedUrl, url);
    assert.deepEqual(calls, [url]);
  });
}
test('only-local, disabled and absent consent never call SDK', async () => {
  for (const override of [{ provider: 'local' }, { enabled: false }, { cloudConsent: false }]) {
    const { service, calls } = await fixture(() => response(2));
    await service.handle('configure', policy({ policyRevision: 2, ...override }));
    const result = await service.handle('check', request('r1', { policyRevision: 2 }));
    assert.equal(result.verdict, 'unknown');
    assert.equal(calls.length, 0);
  }
});
test('engine consent cannot replace shell consent; incognito is checked independently', async () => {
  const { service, calls } = await fixture(() => response(2), {}, (_ctx, incognito) => !incognito);
  await service.handle('configure', policy({ contextId: 'otr', incognito: true }));
  assert.equal((await service.handle('check', request('private', { contextId: 'otr' }))).reason, 'consent_required');
  assert.equal(calls.length, 0);
  assert.equal((await service.handle('check', request())).verdict, 'phishing');
});
test('unsafe/unsanitized URLs, wrong protocol and unknown context never reach SDK', async () => {
  const { service, calls } = await fixture(() => response(0));
  for (const badUrl of ['file:///secret', 'http://localhost/', 'http://127.0.0.1/',
    'http://foo.local/', 'http://[::1]/', 'https://example.com/?token=secret',
    'https://example.com/#secret', 'https://user@example.com/', 'https://example.com/oauth/callback',
    'https://example.com/%61ccess', 'https://example.com/' + 'a'.repeat(49),
    'https://example.com/\nhttps://evil.example/']) {
    assert.equal((await service.handle('check', request('bad', { url: badUrl }))).verdict, 'unknown');
  }
  assert.equal((await service.handle('check', request('x', { protocolVersion: 2 }))).reason, 'unsupported_protocol');
  assert.equal((await service.handle('check', request('x', { contextId: 'absent' }))).reason, 'stale_policy');
  assert.equal(calls.length, 0);
});
test('unknown threats, mismatched and malformed SDK responses are never safe', async () => {
  for (const data of [null, {}, { results: [] }, { results: [null] },
    { results: [{ url, threat: 9 }] }, { results: [{ url: 'https://other.example/', threat: 0 }] }]) {
    const { service } = await fixture(() => Promise.resolve(data));
    const result = await service.handle('check', request());
    assert.equal(result.verdict, 'unknown');
    assert.equal(result.reason, 'invalid_response');
  }
});
test('SDK failures return fixed codes without exception messages', async () => {
  for (const [code, reason] of [[201, 'permission_denied'], [801, 'unsupported'],
    [1010800002, 'network_unavailable'], [1010800006, 'rate_limited'],
    [1010800008, 'cloud_quota_exceeded'], [1010800007, 'timeout']]) {
    const { service } = await fixture(() => Promise.reject({ code, message: 'SECRET_URL_AND_TOKEN' }));
    const result = await service.handle('check', request());
    assert.equal(result.verdict, 'unknown');
    assert.equal(result.reason, reason);
    assert.ok(!JSON.stringify(result).includes('SECRET'));
  }
});
test('timeout does not free real SDK concurrency or accept late result', async () => {
  let settle;
  const { service, calls } = await fixture(() => new Promise(resolve => { settle = resolve; }), { maxConcurrent: 1 });
  const first = await service.handle('check', request('r1', { timeoutMs: 10 }));
  assert.equal(first.reason, 'timeout');
  assert.equal((await service.handle('check', request('r2'))).reason, 'concurrency_limited');
  assert.equal(calls.length, 1);
  settle({ results: [{ url, threat: 2 }] });
  await Promise.resolve();
  assert.equal(first.verdict, 'unknown');
});
test('mode change cancels pending checks and rejects stale policies', async () => {
  let settle;
  const { service } = await fixture(() => new Promise(resolve => { settle = resolve; }));
  const pending = service.handle('check', request());
  await service.handle('configure', policy({ policyRevision: 2, provider: 'local' }));
  assert.equal((await pending).reason, 'stale_policy');
  assert.equal((await service.handle('configure', policy())).accepted, false);
  settle({ results: [{ url, threat: 2 }] });
  assert.equal((await pending).verdict, 'unknown');
});
test('cancel is context-scoped and idempotent', async () => {
  let settle;
  const { service } = await fixture(() => new Promise(resolve => { settle = resolve; }));
  const pending = service.handle('check', request());
  assert.equal((await service.handle('cancel', request('r1', { contextId: 'other' }))).cancelled, false);
  assert.equal((await service.handle('cancel', request())).cancelled, true);
  assert.equal((await pending).reason, 'cancelled');
  assert.equal((await service.handle('cancel', request())).cancelled, false);
  settle({ results: [{ url, threat: 0 }] });
});
test('consent withdrawal and disposal settle pending work', async () => {
  for (const dispose of [false, true]) {
    let settle;
    const { service } = await fixture(() => new Promise(resolve => { settle = resolve; }));
    const pending = service.handle('check', request());
    if (dispose) service.dispose(); else service.revokeContext('ctx');
    assert.equal((await pending).reason, dispose ? 'context_closed' : 'consent_required');
    assert.equal((await service.handle('check', request('r2'))).verdict, 'unknown');
    settle({ results: [{ url, threat: 2 }] });
  }
});
test('unverified SDK mode and capability probes never send URL', async () => {
  const { service, calls } = await fixture(() => response(0), { originPathVerified: false });
  assert.equal((await service.handle('capabilities', {})).configuration, 'unverified');
  assert.equal((await service.handle('check', request())).reason, 'unsupported_query_mode');
  assert.equal(calls.length, 0);
});
