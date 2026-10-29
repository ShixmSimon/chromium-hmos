import assert from 'node:assert/strict';
import * as fs from 'node:fs';
import { stripTypeScriptTypes } from 'node:module';
import { tmpdir } from 'node:os';
import path from 'node:path';
import test from 'node:test';
import vm from 'node:vm';

const source = fs.readFileSync(new URL(
  '../../overlay/chromium-ui/engine/src/main/ets/components/webwindow/WebWindow.ets',
  import.meta.url), 'utf8');
const methods = source.slice(source.indexOf('  private prepareRuntimeAssets('),
  source.indexOf('  private closeIcuDataDescriptor('));
const constants = source.slice(source.indexOf('const RUNTIME_VERSION_RAWFILE_PATH:'),
  source.indexOf('@ComponentV2'));
const executable = stripTypeScriptTypes(constants + '\nclass RuntimeAssets {\n' + methods +
  '\n}\nglobalThis.RuntimeAssets = RuntimeAssets;');
const versionA = 'a'.repeat(64);
const versionB = 'b'.repeat(64);

function fixture(t) {
  const root = fs.mkdtempSync(path.join(tmpdir(), 'ohos-runtime-assets-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  let version = versionA;
  let locale = 'zh-CN';
  let failFile = '';
  const reads = [];
  const commonFiles = ['chrome_100_percent.pak', 'chrome_200_percent.pak',
    'headless_command_resources.pak', 'icudtl.dat', 'resources.pak', 'snapshot_blob.bin'];
  const host = {
    filesDir: root,
    resourceManager: {
      getRawFileContentSync(name) {
        reads.push(name);
        if (name === failFile) throw new Error('interrupted extraction');
        return Buffer.from(name === 'chromium/runtime.version' ? version : `${version}:${name}`);
      },
      getRawFileListSync: name => name === 'chromium/locales'
        ? ['zh-CN.pak', 'en-US.pak'] : commonFiles
    }
  };
  const sandbox = {
    i18n: { System: { getSystemLanguage: () => locale } },
    fileIo: {
      accessSync: fs.existsSync,
      mkdirSync: directory => fs.mkdirSync(directory, { recursive: true }),
      unlinkSync: fs.unlinkSync,
      OpenMode: { CREATE: 1, TRUNC: 2, WRITE_ONLY: 4 },
      openSync: file => ({ fd: fs.openSync(file, 'w') }),
      writeSync: (fd, content) => fs.writeSync(fd,
        typeof content === 'string' ? content : new Uint8Array(content)),
      closeSync: file => fs.closeSync(file.fd)
    }
  };
  vm.createContext(sandbox);
  vm.runInContext(executable, sandbox);
  const runtime = new sandbox.RuntimeAssets();
  runtime.getUIContext = () => ({ getHostContext: () => host });
  return {
    root, reads,
    version: value => { version = value; },
    locale: value => { locale = value; },
    fail: value => { failFile = value; },
    prepare() { const config = {}; runtime.prepareRuntimeAssets(config); return config; },
    read: file => fs.readFileSync(file, 'utf8')
  };
}

test('legacy completion markers cannot select overwritten resources', t => {
  const f = fixture(t);
  const legacy = path.join(f.root, 'chromium-runtime');
  fs.mkdirSync(path.join(legacy, 'locales'), { recursive: true });
  fs.writeFileSync(path.join(legacy, `.complete-${versionA}`), versionA);
  fs.writeFileSync(path.join(legacy, `.locale-zh-CN-${versionA}`), versionA);
  fs.writeFileSync(path.join(legacy, 'resources.pak'), 'stale Chromium B');
  fs.writeFileSync(path.join(legacy, 'locales/zh-CN.pak'), 'stale Chromium B locale');
  const config = f.prepare();
  assert.notEqual(config.resourcesDir, legacy);
  assert.equal(f.read(path.join(config.resourcesDir, 'resources.pak')),
    `${versionA}:chromium/resources.pak`);
  assert.equal(f.read(path.join(config.resourcesDir, 'locales/zh-CN.pak')),
    `${versionA}:chromium/locales/zh-CN.pak`);
});

test('A -> B -> A preserves each runtime and reuses completed A', t => {
  const f = fixture(t);
  const a = f.prepare();
  f.version(versionB);
  const b = f.prepare();
  assert.notEqual(a.resourcesDir, b.resourcesDir);
  f.version(versionA);
  f.reads.length = 0;
  assert.equal(f.prepare().resourcesDir, a.resourcesDir);
  assert.deepEqual(f.reads, ['chromium/runtime.version']);
  for (const [config, version] of [[a, versionA], [b, versionB]]) {
    assert.equal(f.read(path.join(config.resourcesDir, 'resources.pak')),
      `${version}:chromium/resources.pak`);
    assert.equal(f.read(path.join(config.resourcesDir, 'locales/zh-CN.pak')),
      `${version}:chromium/locales/zh-CN.pak`);
  }
});

test('interrupted B extraction cannot damage completed A and B is retried', t => {
  const f = fixture(t);
  const a = f.prepare();
  f.version(versionB);
  f.fail('chromium/resources.pak');
  assert.throws(() => f.prepare(), /interrupted extraction/);
  f.version(versionA);
  f.reads.length = 0;
  assert.equal(f.prepare().resourcesDir, a.resourcesDir);
  assert.deepEqual(f.reads, ['chromium/runtime.version']);
  assert.equal(f.read(path.join(a.resourcesDir, 'chrome_100_percent.pak')),
    `${versionA}:chromium/chrome_100_percent.pak`);
  f.version(versionB);
  f.fail('');
  f.reads.length = 0;
  f.prepare();
  assert.ok(f.reads.includes('chromium/chrome_100_percent.pak'));
  assert.ok(f.reads.includes('chromium/resources.pak'));
});

test('locale failure retries only the unfinished locale', t => {
  const f = fixture(t);
  f.fail('chromium/locales/zh-CN.pak');
  assert.throws(() => f.prepare(), /interrupted extraction/);
  f.fail('');
  f.reads.length = 0;
  const config = f.prepare();
  assert.deepEqual(f.reads, ['chromium/runtime.version', 'chromium/locales/zh-CN.pak']);
  assert.equal(config.applicationLocale, 'zh-CN');
  f.locale('en-US');
  f.prepare();
  assert.equal(f.read(path.join(config.resourcesDir, 'locales/en-US.pak')),
    `${versionA}:chromium/locales/en-US.pak`);
});
