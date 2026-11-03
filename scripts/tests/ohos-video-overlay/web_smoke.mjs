// Node 22+. Local test-page QA, not an HDR/display or hardware-overlay oracle.
// Creates/closes its own tab; never changes existing tabs.
import fs from 'node:fs/promises';
const endpoint = process.argv[2] || 'http://127.0.0.1:19333';
const url = process.argv[3] || 'http://127.0.0.1:18880/video-overlay-hdr/';
const output = process.argv[4] || '/tmp/lumie-hdr-web-qa';
await fs.mkdir(output, {recursive: true});
async function connect(url) {
  const ws = new WebSocket(url);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
  let seq = 0;
  const pending = new Map();
  ws.onmessage = event => {
    const message = JSON.parse(event.data), item = pending.get(message.id);
    if (!item) return;
    pending.delete(message.id); clearTimeout(item.timer);
    message.error ? item.reject(Error(JSON.stringify(message.error))) : item.resolve(message.result);
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
const version = await (await fetch(`${endpoint}/json/version`)).json();
const browser = await connect(version.webSocketDebuggerUrl);
let target, client;
const checks = [];
try {
  target = (await browser.call('Target.createTarget', {url: 'about:blank'})).targetId;
  const page = (await (await fetch(`${endpoint}/json/list`)).json()).find(t => t.id === target);
  client = await connect(page.webSocketDebuggerUrl);
  const evaluate = async expression => {
    const result = await client.call('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true});
    if (result.exceptionDetails) throw Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  const until = async expression => evaluate(`(async () => {
    const deadline = performance.now() + 20000;
    while (!(${expression})) {
      if (performance.now() > deadline) throw Error('condition timed out: ' + ${JSON.stringify(expression)});
      await new Promise(r => setTimeout(r, 100));
    }
    return true;
  })()`);
  const check = async (name, expression) => {
    if (!await evaluate(expression)) throw Error(`FAIL: ${name}`);
    checks.push(name);
  };
  await client.call('Emulation.setDeviceMetricsOverride', {width: 412, height: 915, deviceScaleFactor: 1, mobile: true});
  await client.call('Page.navigate', {url});
  await until(`window.videoOverlayTest && !document.querySelector('#play').disabled`);
  await check('four verified fixtures', `videoOverlayTest.snapshot().manifest.clips.length === 4`);
  await check('no horizontal overflow', `document.documentElement.scrollWidth <= innerWidth`);
  for (const id of ['sdr-avc', 'sdr-hevc10', 'hdr10-hevc', 'pq-vp9']) {
    await evaluate(`document.querySelector('#clip').value=${JSON.stringify(id)}; document.querySelector('#play').click()`);
    await until(`videoOverlayTest.snapshot().activeClip === ${JSON.stringify(id)} && document.querySelector('video').currentTime > 0.2`);
    await check(`decoded ${id}`, `document.querySelector('video').videoWidth === 1280`);
  }
  await evaluate(`document.querySelector('#small').click();document.querySelector('#occlusion').click();document.querySelector('#danmaku').click()`);
  await check('layout controls', `videoOverlayTest.snapshot().layout.occlusion && videoOverlayTest.snapshot().layout.danmaku && videoOverlayTest.snapshot().layout.stageClass.includes('small')`);
  const screenshot = await client.call('Page.captureScreenshot', {format: 'png'});
  await fs.writeFile(`${output}/mobile.png`, Buffer.from(screenshot.data, 'base64'));
  await evaluate(`document.querySelector('#cycle').click()`);
  await until(`videoOverlayTest.snapshot().events.some(e => e.type === 'cycle-complete')`);
  await check('SDR HDR SDR returned to SDR', `videoOverlayTest.snapshot().activeClip === 'sdr-avc'`);
  await evaluate(`document.querySelector('#cycle').click(); document.querySelector('#stop').click()`);
  await evaluate(`new Promise(r => setTimeout(r, 6500))`);
  await check('stop cancels pending transition', `videoOverlayTest.snapshot().activeClip === null && !document.querySelector('video').getAttribute('src')`);
  await evaluate(`document.querySelector('#clip').value='sdr-avc'; document.querySelector('#play').click(); document.querySelector('#clip').value='pq-vp9'; document.querySelector('#play').click()`);
  await until(`videoOverlayTest.snapshot().activeClip === 'pq-vp9' && document.querySelector('video').currentTime > 0.2`);
  await check('rapid selection latest wins', `!document.querySelector('#status').textContent.includes('中止')`);
  await evaluate(`document.querySelector('#seek').click()`);
  await until(`!document.querySelector('video').seeking && document.querySelector('video').currentTime >= 2`);
  await check('native evidence remains unmeasured', `Object.values(videoOverlayTest.snapshot().nativeEvidence).every(v => v === 'not-measured')`);
  const report = await evaluate('videoOverlayTest.snapshot()');
  await fs.writeFile(`${output}/report.json`, JSON.stringify({browser: version.Browser, checks, report}, null, 2));
  console.log(JSON.stringify({checks, output}, null, 2));
} finally {
  client?.ws.close();
  if (target) await browser.call('Target.closeTarget', {targetId: target}).catch(() => {});
  browser.ws.close();
}
