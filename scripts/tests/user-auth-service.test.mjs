import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {stripTypeScriptTypes} from 'node:module';
import test from 'node:test';
import vm from 'node:vm';

// Run the actual ArkTS logic with a controlled UserAuthenticationKit boundary.
// Native biometrics, token validity and ArkTS compilation are separate checks.
const source = readFileSync(new URL('../../overlay/chromium-ui/engine/src/main/ets/utils/UserAuthService.ets', import.meta.url), 'utf8')
  .replace(/^import .*;\r?\n/gm, '').replace('export class', 'class');
const executable = stripTypeScriptTypes(source) + '\nglobalThis.Service = UserAuthService;';
function fixture(options = {}) {
  const instances = [];
  const context = vm.createContext({
    userAuth: {
      UserAuthType: {FACE: 1, FINGERPRINT: 2, PIN: 4}, AuthTrustLevel: {ATL3: 3},
      UserAuthResultCode: {SUCCESS: 0},
      getAvailableStatus() {if (options.unavailable) throw Error('not enrolled');},
      getUserAuthInstance(params) {
        if (options.constructThrows) throw Error('construct failed');
        const item = {params, cancelled: 0, detached: 0,
          on(_, callback) {this.callback = callback; if (options.onThrows) throw Error('on failed');},
          off() {this.detached++;},
          start() {if (options.startThrows) throw Error('start failed');},
          cancel() {this.cancelled++; if (options.cancelThrows) throw Error('cancel failed');},
          // Deliberately allow an already-dispatched callback after off().
          success() {this.callback.onResult({result: 0, token: new Uint8Array([1, 2, 3])});},
          fail() {this.callback.onResult({result: 1});}
        };
        instances.push(item); return item;
      }
    },
    util: {Base64Helper: class {
      decodeSync(value) {return new Uint8Array(Buffer.from(value, 'base64'));}
      encodeToStringSync(value) {return Buffer.from(value).toString('base64');}
    }},
    cryptoFramework: {createRandom: () => ({generateRandomSync: n => ({data: new Uint8Array(n)})})}
  });
  vm.runInContext(executable, context);
  return {service: new context.Service(), other: new context.Service(), instances};
}
const request = requestId => ({requestId, challenge: 'AQIDBA==', title: 'localhost'});
const denied = promise => assert.rejects(promise, /cancelled, unavailable or failed/);

test('successful authentication passes the exact challenge and token', async () => {
  const f = fixture(); const promise = f.service.handle('authenticate', request('a'));
  assert.deepEqual([...f.instances[0].params.challenge], [1, 2, 3, 4]);
  f.instances[0].success(); assert.equal((await promise).token, 'AQID');
  const next = f.service.handle('authenticate', request('b')); f.instances[1].success(); await next;
});
test('matching cancellation dismisses and settles even without an SDK callback', async () => {
  const f = fixture(); const settled = denied(f.service.handle('authenticate', request('a')));
  await f.other.handle('cancelAuthentication', {requestId: 'a'}); // window switched
  await settled; assert.equal(f.instances[0].cancelled, 1);
  f.instances[0].success(); // Must not turn cancellation into success.
});
test('stale cancel and late results cannot affect a newer request', async () => {
  const f = fixture(); const first = denied(f.service.handle('authenticate', request('a')));
  await f.service.handle('cancelAuthentication', {requestId: 'a'}); await first;
  const second = f.service.handle('authenticate', request('b'));
  f.instances[0].success(); await f.other.handle('cancelAuthentication', {requestId: 'a'});
  await f.service.handle('cancel', {}); // Password cancellation is isolated.
  assert.equal(f.instances[1].cancelled, 0);
  await denied(f.other.handle('authenticate', request('c'))); // b still owns prompt
  assert.equal(f.instances.length, 2);
  f.instances[1].success(); assert.equal((await second).token, 'AQID');
});
test('password verification cannot be cancelled by a WebAuthn request', async () => {
  const f = fixture(); const promise = f.service.handle('verify', {});
  await f.other.handle('cancelAuthentication', {requestId: 'a'});
  await f.other.handle('cancel', {}); // Not its password operation.
  assert.equal(f.instances[0].cancelled, 0);
  await f.service.handle('cancel', {});
  assert.equal((await promise).ok, false); assert.equal(f.instances[0].cancelled, 1);
});
test('concurrent password or passkey request never overwrites the active prompt', async () => {
  const f = fixture(); const active = f.service.handle('authenticate', request('a'));
  assert.equal((await f.other.handle('verify', {})).ok, false);
  await denied(f.other.handle('authenticate', request('b')));
  assert.equal(f.instances.length, 1); f.instances[0].success(); await active;
});
for (const failure of ['constructThrows', 'onThrows', 'startThrows', 'cancelThrows']) {
  test(`${failure} does not leave ownership or an unresolved promise`, async () => {
    const options = {[failure]: true}; const f = fixture(options);
    const settled = denied(f.service.handle('authenticate', request('a')));
    if (failure === 'cancelThrows') await f.service.handle('cancelAuthentication', {requestId: 'a'});
    await settled; options[failure] = false;
    const next = f.service.handle('authenticate', request('b'));
    f.instances.at(-1).success(); await next;
  });
}
test('unavailable device and missing request identity never start a prompt', async () => {
  const f = fixture({unavailable: true});
  await denied(f.service.handle('authenticate', request('a')));
  await assert.rejects(f.service.handle('authenticate', {challenge: 'AQ=='}), /requires a requestId/);
  assert.equal((await f.service.handle('verify', {})).ok, false);
  assert.equal(f.instances.length, 0);
});
