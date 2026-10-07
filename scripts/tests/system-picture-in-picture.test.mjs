import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { stripTypeScriptTypes } from 'node:module';
import test from 'node:test';
import vm from 'node:vm';

const sourceUrl = new URL('../../overlay/chromium-ui/engine/src/main/ets/components/webwindow/PictureInPictureSurface.ets', import.meta.url);
const source = readFileSync(sourceUrl, 'utf8')
  .replace(/^import .*;\r?\n/gm, '')
  .replace('@Component', '')
  .replace('export struct PictureInPictureSurface', 'class PictureInPictureSurface')
  .replace(/@(Prop|State) /g, '');
const methods = source.slice(0, source.indexOf('  build() {')) + '\n}\n';
const executable = stripTypeScriptTypes(methods) + '\nglobalThis.Surface = PictureInPictureSurface;';

function deferred() {
  let resolve;
  const promise = new Promise(complete => { resolve = complete; });
  return { promise, resolve };
}

function fixture() {
  const listeners = new Map();
  const timers = new Map();
  const commands = [];
  let nativeCallback;
  let stopCount = 0;
  const controller = {
    on: (event, callback) => listeners.set(event, callback),
    off: event => listeners.delete(event),
    startPiP: async () => listeners.get('stateChange')?.(2),
    stopPiP: async () => {
      stopCount++;
      listeners.get('stateChange')?.(4);
    },
    updatePiPControlStatus: (_control, status) => { controller.status = status; }
  };
  const nativeContext = {
    ExecuteBrowserCommand: (_id, text) => { commands.push(JSON.parse(text)); return true; },
    SetBrowserEventCallback: (_id, callback) => { nativeCallback = callback; }
  };
  const api = {
    isPiPEnabled: () => true,
    create: async () => controller,
    PiPTemplateType: { VIDEO_PLAY: 0 },
    PiPState: { STARTED: 2, STOPPED: 4, ABOUT_TO_RESTORE: 5, ERROR: 6 },
    PiPControlType: { VIDEO_PLAY_PAUSE: 0 },
    PiPControlStatus: { PLAY: 1, PAUSE: 0 }
  };
  const sandbox = {
    PiPWindow: api,
    XComponentController: class { getXComponentContext() { return nativeContext; } },
    hilog: { warn: () => {} },
    canIUse: () => true,
    setInterval: callback => { timers.set(1, callback); return 1; },
    clearInterval: timer => timers.delete(timer)
  };
  vm.createContext(sandbox);
  vm.runInContext(executable, sandbox);
  const surface = new sandbox.Surface();
  surface.getUIContext = () => ({ getHostContext: () => ({}) });
  surface.widget = 42;
  surface.componentId = 'aura_aux_42';
  return {
    surface, controller, api, listeners, timers, commands,
    state: value => listeners.get('stateChange')?.(value),
    reply: value => nativeCallback(JSON.stringify(value)),
    stops: () => stopCount
  };
}

test('system close pauses only the matching Chromium PiP window', async () => {
  const current = fixture();
  assert.equal(current.surface.systemWindowActive, false);
  await current.surface.start();
  assert.equal(current.surface.systemWindowActive, true);
  current.state(4);
  assert.equal(current.surface.systemWindowActive, false);
  assert.equal(current.commands.at(-1).pipAction, 'close');
  assert.equal(current.commands.at(-1).pipWidget, 42);
  assert.equal(current.timers.size, 0);
  assert.equal(current.listeners.size, 0);
});

test('restore requests return to the opener instead of pause', async () => {
  const current = fixture();
  await current.surface.start();
  current.state(5);
  assert.equal(current.surface.systemWindowActive, true);
  current.state(4);
  assert.equal(current.surface.systemWindowActive, false);
  assert.equal(current.commands.at(-1).pipAction, 'restore');
});

test('lifecycle failure restores the in-app surface presentation', async () => {
  const current = fixture();
  await current.surface.start();
  const surfaceController = current.surface.surfaceController;
  current.state(6);
  assert.equal(current.surface.systemWindowActive, false);
  assert.equal(current.surface.surfaceController, surfaceController);
});

test('disposal during creation never starts the obsolete window', async () => {
  const current = fixture();
  const pending = deferred();
  let starts = 0;
  current.api.create = () => pending.promise;
  current.controller.startPiP = async () => { starts++; };
  const starting = current.surface.start();
  current.surface.aboutToDisappear();
  pending.resolve(current.controller);
  await starting;
  assert.equal(starts, 0);
  assert.equal(current.listeners.size, 0);
});

test('disposal during start stops the completed window without closing a new session', async () => {
  const current = fixture();
  const pending = deferred();
  const entered = deferred();
  current.controller.startPiP = () => { entered.resolve(); return pending.promise; };
  const starting = current.surface.start();
  await entered.promise;
  current.surface.aboutToDisappear();
  pending.resolve();
  await starting;
  assert.equal(current.stops(), 1);
  assert.equal(current.commands.length, 0);
  assert.equal(current.timers.size, 0);
});

test('start rejection preserves the Chromium in-app window', async () => {
  const current = fixture();
  current.controller.startPiP = async () => { throw new Error('unsupported'); };
  await current.surface.start();
  assert.equal(current.commands.length, 0);
  assert.equal(current.surface.systemWindowActive, false);
  assert.equal(current.listeners.size, 0);
  assert.equal(current.timers.size, 0);
});

test('playback controls use the native bridge without an ArkUI state update', async () => {
  const current = fixture();
  await current.surface.start();
  current.listeners.get('controlEvent')({ controlType: 0, status: 0 });
  assert.equal(current.commands.at(-1).pipAction, 'pause');
  current.listeners.get('controlEvent')({ controlType: 0, status: 1 });
  assert.equal(current.commands.at(-1).pipAction, 'play');
  current.reply({ event: 'pictureInPictureState', pipWidget: 999, pipPlaying: false });
  assert.equal(current.controller.status, undefined);
  current.reply({ event: 'pictureInPictureState', pipWidget: 42, pipPlaying: false });
  assert.equal(current.controller.status, 0);
});

test('loss of the source stops the system window without affecting another session', async () => {
  const current = fixture();
  await current.surface.start();
  for (let count = 0; count < 3; count++) {
    current.timers.get(1)?.();
  }
  await Promise.resolve();
  assert.equal(current.stops(), 1);
  assert.equal(current.timers.size, 0);
  assert.ok(current.commands.every(command => command.pipAction === 'state'));
});
