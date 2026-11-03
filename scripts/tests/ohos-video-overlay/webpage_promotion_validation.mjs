// Node 22+, opt-in engine installed, forwarded CDP and fixture HTTP ports.
// This test requires RS layer evidence in addition to advancing video frames.
import fs from 'node:fs/promises';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
const exec = promisify(execFile);
const [endpoint, url, output, serial] = process.argv.slice(2);
if (!serial) throw Error('Usage: node webpage_promotion_validation.mjs CDP PAGE OUTPUT SERIAL');
const hdc = process.env.HDC_BIN || '/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc';
await fs.mkdir(output, {recursive: false});
const report = {started: new Date().toISOString(), scope: 'Chromium webpage promotion and CSS fallback', cases: [], cleanupErrors: []};
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function connect(url) {
  const ws = new WebSocket(url);
  await new Promise((resolve, reject) => {ws.onopen = resolve; ws.onerror = reject;});
  let sequence = 0;
  const pending = new Map();
  ws.onmessage = event => {
    const message = JSON.parse(event.data), entry = pending.get(message.id);
    if (!entry) return;
    pending.delete(message.id); clearTimeout(entry.timer);
    message.error ? entry.reject(Error(JSON.stringify(message.error))) : entry.resolve(message.result);
  };
  return {ws, call(method, params = {}) {
    return new Promise((resolve, reject) => {
      const id = ++sequence;
      const timer = setTimeout(() => {pending.delete(id); reject(Error(`${method} timed out`));}, 15000);
      pending.set(id, {resolve, reject, timer}); ws.send(JSON.stringify({id, method, params}));
    });
  }};
}
let browser, page, target, evaluate;
const seenLogs = new Set();
const events = [];
async function layers() {
  const {stdout} = await exec(hdc, ['-t', serial, 'shell', 'hidumper', '-s', '10', '-a', 'RSTree'], {maxBuffer: 16*1024*1024, timeout: 15000});
  // Save only test layer lines, not other apps' window tree.
  return stdout.split('\n').filter(line => /Name \[Lumie(?:Web|Video)Layer\]/.test(line));
}
async function logs() {
  const {stdout} = await exec(hdc, ['-t', serial, 'shell', 'hilog', '-x', '-t', 'app', '-T', 'Chromium', '-e', 'OHOS overlay:'], {maxBuffer: 4*1024*1024, timeout: 10000});
  const fresh = stdout.split('\n').filter(line => line.includes('OHOS overlay:') && !seenLogs.has(line));
  fresh.forEach(line => seenLogs.add(line));
  return fresh;
}
async function waitFor(read, accept, description, timeout = 20000) {
  const deadline = Date.now()+timeout;
  let value;
  do {value = await read(); if (accept(value)) return value; await sleep(250);} while (Date.now()<deadline);
  throw Error(`${description}: ${JSON.stringify(value)}`);
}
const visible = (rows, plane) => rows.some(line => line.includes(`Name [Lumie${plane}Layer]`) && /Visible: 1\b/.test(line));
const snapshot = () => evaluate(`({time:v.currentTime,frames:v.getVideoPlaybackQuality().totalVideoFrames,paused:v.paused,error:v.error?.message,visibility:document.visibilityState,rect:v.getBoundingClientRect().toJSON(),dpr:devicePixelRatio,hdr:matchMedia('(dynamic-range: high)').matches})`);
try {
  report.version = await (await fetch(`${endpoint}/json/version`, {signal: AbortSignal.timeout(5000)})).json();
  browser = await connect(report.version.webSocketDebuggerUrl);
  report.system = await browser.call('SystemInfo.getInfo');
  target = (await browser.call('Target.createTarget', {url:'about:blank'})).targetId;
  const targets = await (await fetch(`${endpoint}/json/list`)).json();
  page = await connect(targets.find(item => item.id===target).webSocketDebuggerUrl);
  evaluate = async expression => {
    const result = await page.call('Runtime.evaluate', {expression, awaitPromise:true, returnByValue:true, userGesture:true});
    if (result.exceptionDetails) throw Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  await browser.call('Target.activateTarget', {targetId:target});
  await page.call('Page.navigate', {url});
  await waitFor(() => evaluate('!!document.querySelector("video")'), Boolean, 'Fixture load');
  await evaluate(`(async()=>{
    window.v=document.querySelector('video'); v.pause(); v.removeAttribute('src'); v.load();
    v.remove(); document.body.replaceChildren(v); document.documentElement.style.cssText='margin:0;padding:0;background:white';
    document.body.style.cssText='margin:0;padding:0;background:white';
    v.style.cssText='position:fixed;left:0;top:0;max-width:none;max-height:none;object-fit:fill;border:0;border-radius:0;transform:none;filter:none;opacity:1';
    v.style.width=(Math.min(800,Math.floor(innerWidth*devicePixelRatio))/devicePixelRatio)+'px';
    v.style.height=(Math.min(450,Math.floor(innerHeight*devicePixelRatio))/devicePixelRatio)+'px';
    v.controls=false;v.muted=true;v.loop=true;
    try {window.testWakeLock=await navigator.wakeLock.request('screen');return 'acquired';} catch(e) {return String(e)}
  })()`);
  await logs(); // Discard events from previous app sessions.
  for (const clip of ['sdr-avc.mp4','sdr-hevc10.mp4','hdr10-hevc.mp4']) {
    const entry = {clip}; report.cases.push(entry);
    await evaluate(`v.style.filter='none';v.src=${JSON.stringify(new URL('media/'+clip,url).href)};v.play()`);
    await waitFor(snapshot, s => s.time>0.25 && !s.paused && s.visibility==='visible', 'Video advances');
    entry.promoted = await waitFor(layers, rows => visible(rows,'Web') && visible(rows,'Video'), 'Video layer visible');
    entry.initial = await snapshot();
    // The six-second looping fixtures must keep producing frames beyond the
    // former ten-second swap timeout. A running media clock or a retained RS
    // buffer alone does not prove that promoted video is still advancing.
    entry.sustained = await waitFor(snapshot, s => !s.error && !s.paused &&
      s.frames >= entry.initial.frames + 360, 'Promoted playback continues', 25000);
    events.push(...await logs());
    await evaluate('v.pause();v.style.left=(16/devicePixelRatio)+"px"');
    await sleep(400);
    entry.pausedMoved = await snapshot(); entry.pausedLayers = await layers();
    // Chromium may demote a paused frame under its power/damage policy. This
    // is a valid composited fallback, not a requirement to force promotion.
    if (!entry.pausedMoved.paused || entry.pausedMoved.error) throw Error('Paused video failed');
    await evaluate("v.style.left='0px';v.style.filter='blur(1px)';v.play()");
    entry.demoted = await waitFor(layers, rows => visible(rows,'Web') && !visible(rows,'Video'), 'CSS effect demotion');
    const before = (await snapshot()).time;
    await waitFor(snapshot, s => s.time>before+0.2 || s.time<before, 'Composited fallback advances');
    events.push(...await logs());
    await evaluate("v.style.filter='none'");
    entry.repromoted = await waitFor(layers, rows => visible(rows,'Web') && visible(rows,'Video'), 'Re-promotion');
    events.push(...await logs());
    entry.final = await snapshot();
    await evaluate("v.style.filter='blur(1px)'");
    await waitFor(layers, rows => visible(rows,'Web') && !visible(rows,'Video'), 'Between-clip demotion');
    await fs.writeFile(`${output}/report.json`,JSON.stringify(report,null,2));
  }
  report.commandsProcessed = {promotion:events.filter(s=>s.includes('promotion commands processed')).length,
    demotion:events.filter(s=>s.includes('demotion commands processed')).length};
  if (!report.commandsProcessed.promotion || !report.commandsProcessed.demotion || events.some(s=>s.includes('channel/plane rejected'))) throw Error('Presenter promotion/demotion evidence missing or channel rejected');
  report.passed = true;
} catch(e) {
  report.error=String(e); process.exitCode=1;
  report.failureLayers = await layers().catch(()=>[]);
  events.push(...await logs().catch(()=>[]));
  if (evaluate) report.failureSnapshot=await snapshot().catch(()=>null);
} finally {
  if(evaluate) await evaluate('v.pause();v.removeAttribute("src");v.load();window.testWakeLock?.release()').catch(e=>report.cleanupErrors.push(String(e)));
  page?.ws.close();
  if(target) await browser.call('Target.closeTarget',{targetId:target}).catch(e=>report.cleanupErrors.push(String(e)));
  browser?.ws.close();report.finished=new Date().toISOString();
  await fs.writeFile(`${output}/presenter.log`,events.join('\n')+'\n');
  await fs.writeFile(`${output}/report.json`,JSON.stringify(report,null,2));
  console.log(JSON.stringify({output,passed:report.passed,error:report.error},null,2));
}
