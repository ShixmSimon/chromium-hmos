// Node 22+, an already forwarded device CDP endpoint and test-page HTTP server.
// Only the new test tab is navigated/closed. No emulation or device settings.
import fs from 'node:fs/promises';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
const exec = promisify(execFile);
const endpoint = process.argv[2] || 'http://127.0.0.1:19422';
const url = process.argv[3] || 'http://127.0.0.1:18880/video-overlay-hdr/';
const output = process.argv[4];
const serial = process.argv[5];
if (!output || !serial) throw Error('Usage: node device_validation.mjs CDP PAGE OUTPUT SERIAL');
await fs.mkdir(output, {recursive: false});
const hdc = process.env.HDC_BIN || '/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc';
const report = {started: new Date().toISOString(), serial, cases: [], checks: [], cleanupErrors: [],
  scope: 'Existing browser playback path; no independent video presenter installed'};
const save = () => fs.writeFile(`${output}/report.json`, JSON.stringify(report, null, 2));
async function connect(url, onEvent = () => {}) {
  const ws = new WebSocket(url);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
  let seq = 0;
  const pending = new Map();
  ws.onmessage = event => {
    const m = JSON.parse(event.data), item = pending.get(m.id);
    if (!item) { if (m.method) onEvent(m); return; }
    pending.delete(m.id); clearTimeout(item.timer);
    m.error ? item.reject(Error(JSON.stringify(m.error))) : item.resolve(m.result);
  };
  return {ws, call(method, params = {}) {
    return new Promise((resolve, reject) => {
      const id = ++seq;
      const timer = setTimeout(() => { pending.delete(id); reject(Error(`${method} timeout`)); }, 30000);
      pending.set(id, {resolve, reject, timer});
      ws.send(JSON.stringify({id, method, params}));
    });
  }};
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
let browser, page, target, evaluate;
let phase = 'setup';
const media = [];
try {
  report.version = await (await fetch(`${endpoint}/json/version`, {signal: AbortSignal.timeout(5000)})).json();
  browser = await connect(report.version.webSocketDebuggerUrl);
  try { report.system = await browser.call('SystemInfo.getInfo'); } catch (e) { report.systemError = String(e); }
  target = (await browser.call('Target.createTarget', {url: 'about:blank'})).targetId;
  report.target = target; await save();
  const targets = await (await fetch(`${endpoint}/json/list`)).json();
  page = await connect(targets.find(t => t.id === target).webSocketDebuggerUrl, event => {
    if (event.method.startsWith('Media.')) media.push({phase, time: new Date().toISOString(), ...event});
  });
  evaluate = async expression => {
    const result = await page.call('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true, userGesture: true});
    if (result.exceptionDetails) throw Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  const until = async (condition, timeout = 20000) => {
    const deadline = Date.now() + timeout;
    while (!await evaluate(condition)) {
      if (Date.now() >= deadline) throw Error(`Timed out: ${condition}`);
      await sleep(250);
    }
  };
  await page.call('Media.enable');
  await browser.call('Target.activateTarget', {targetId: target});
  await page.call('Page.navigate', {url});
  await until('window.videoOverlayTest && !document.querySelector("#play").disabled');
  report.visibility = await evaluate('document.visibilityState');
  if (report.visibility !== 'visible') throw Error('Test page is not visible');
  report.wakeLock = await evaluate(`(async () => {try {window.hdrTestWakeLock=await navigator.wakeLock.request('screen');return 'acquired';}catch(e){return String(e)}})()`);
  const dump = async label => {
    for (const name of ['RSTree', 'screen', 'surface']) {
      try {
        const {stdout} = await exec(hdc, ['-t', serial, 'shell', 'hidumper', '-s', '10', '-a', name], {maxBuffer: 16*1024*1024, timeout: 15000});
        await fs.writeFile(`${output}/${label}-${name}.txt`, stdout);
      } catch (e) { report.checks.push({name: `${label}-${name}`, error: String(e)}); }
    }
  };
  const screenshot = async label => {
    if (process.env.LUMIE_CAPTURE_TEST_SCREEN !== '1') return;
    const {stdout} = await exec(hdc, ['-t', serial, 'shell', 'hidumper', '-s', 'WindowManagerService', '-a', '-a']);
    const focus = stdout.match(/Focus window:\s*(\d+)/)?.[1];
    const owned = stdout.split('\n').some(line => line.startsWith('lumiebrowser') && line.trim().split(/\s+/)[3] === focus);
    if (!owned || await evaluate('document.visibilityState') !== 'visible') {
      report.checks.push({name:`${label}-screenshot`, error:'App/test page not foreground; capture skipped'});
      return;
    }
    await exec('devecocli', ['ui', 'screenshot', '--device', serial, '--path', `${output}/${label}.png`], {timeout:15000});
  };
  for (const id of ['sdr-avc', 'sdr-hevc10', 'hdr10-hevc', 'pq-vp9']) {
    phase = id;
    const entry = {id, started: new Date().toISOString()}; report.cases.push(entry);
    await evaluate(`document.querySelector('#clip').value=${JSON.stringify(id)};document.querySelector('#play').click()`);
    try {
      await until(`videoOverlayTest.snapshot().activeClip === ${JSON.stringify(id)} && document.querySelector('video').currentTime > 0.5 || document.querySelector('#status').textContent.includes('中止')`);
      await evaluate(`document.querySelector('#stage').scrollIntoView({block:'center'})`);
      await sleep(2500);
      entry.snapshot = await evaluate('videoOverlayTest.snapshot()');
      entry.visibility = await evaluate('document.visibilityState');
      entry.status = await evaluate(`document.querySelector('#status').textContent`);
      entry.playing = entry.snapshot.activeClip === id && entry.snapshot.media.currentTime > 0.5 &&
        !entry.snapshot.media.paused && entry.snapshot.media.observedFrameCallbacks > 0 && entry.visibility === 'visible';
      await dump(id);
      if (id === 'hdr10-hevc') await screenshot('hdr10-page');
    } catch (e) { entry.error = String(e); }
    await save();
  }
  phase = 'layout';
  await evaluate(`document.querySelector('#clip').value='hdr10-hevc';document.querySelector('#play').click()`);
  await until(`document.querySelector('video').currentTime > 0.5`);
  for (const id of ['danmaku', 'occlusion', 'small', 'transform']) {
    await evaluate(`document.querySelector('#${id}').click()`);
    await sleep(500);
    report.checks.push({name:id, snapshot:await evaluate('videoOverlayTest.snapshot()')});
    if (id === 'occlusion') await screenshot('hdr10-occlusion');
  }
  await evaluate(`document.querySelector('#fullscreen').click()`);
  await sleep(1000);
  report.checks.push({name:'fullscreen', snapshot:await evaluate('videoOverlayTest.snapshot()')});
  await screenshot('hdr10-fullscreen');
  await evaluate(`document.fullscreenElement ? document.exitFullscreen() : undefined`);
  for (const id of ['danmaku','occlusion','small','transform']) await evaluate(`document.querySelector('#${id}').click()`);
  phase = 'sdr-hdr-sdr';
  await evaluate(`document.querySelector('#cycle').click()`);
  await until(`videoOverlayTest.snapshot().events.some(e=>e.type==='cycle-complete')`, 25000);
  report.checks.push({name:'sdr-hdr-sdr', snapshot:await evaluate('videoOverlayTest.snapshot()')});
  await dump('returned-sdr');
  phase = 'stop';
  await evaluate(`document.querySelector('#cycle').click();document.querySelector('#stop').click()`);
  await sleep(6500);
  report.checks.push({name:'stop-cancels', snapshot:await evaluate('videoOverlayTest.snapshot()')});
  phase = 'rapid-seek';
  await evaluate(`document.querySelector('#clip').value='hdr10-hevc';document.querySelector('#play').click();document.querySelector('#clip').value='sdr-avc';document.querySelector('#play').click()`);
  await until(`videoOverlayTest.snapshot().activeClip==='sdr-avc' && document.querySelector('video').currentTime>0.5`);
  await evaluate(`document.querySelector('#seek').click()`);
  await until(`!document.querySelector('video').seeking && document.querySelector('video').currentTime>=2`);
  report.checks.push({name:'rapid-seek', snapshot:await evaluate('videoOverlayTest.snapshot()')});
  const byName = name => report.checks.find(c => c.name === name)?.snapshot;
  report.assertions = {
    allClipsAdvance: report.cases.length === 4 && report.cases.every(c => c.playing),
    fullscreenEntered: byName('fullscreen')?.layout.fullscreen === true,
    returnedToSdr: byName('sdr-hdr-sdr')?.activeClip === 'sdr-avc',
    stopped: byName('stop-cancels')?.activeClip === null && byName('stop-cancels')?.media.paused === true,
    rapidSelectionAndSeek: byName('rapid-seek')?.activeClip === 'sdr-avc' && byName('rapid-seek')?.media.currentTime >= 2,
  };
  if (Object.values(report.assertions).some(value => !value)) throw Error('Playback assertion failed; inspect report');
} catch (e) {
  report.error = String(e);
  if (evaluate) report.failureSnapshot = await evaluate('window.videoOverlayTest?.snapshot()').catch(() => null);
  process.exitCode = 1;
} finally {
  if (evaluate) await evaluate(`document.querySelector('#stop')?.click();window.hdrTestWakeLock?.release()`).catch(e => report.cleanupErrors.push(String(e)));
  page?.ws.close();
  if (target) await browser.call('Target.closeTarget', {targetId:target}).then(() => {report.targetClosed=true;}).catch(e=>report.cleanupErrors.push(String(e)));
  browser?.ws.close();
  report.finished = new Date().toISOString();
  await fs.writeFile(`${output}/media-events.json`, JSON.stringify(media, null, 2));
  await save();
  console.log(JSON.stringify({output, error:report.error, cases:report.cases.map(c=>({id:c.id,playing:c.playing,error:c.error,status:c.status})),checks:report.checks.map(c=>c.name),cleanupErrors:report.cleanupErrors},null,2));
}
