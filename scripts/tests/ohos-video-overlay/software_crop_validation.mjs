// Node 22+, opt-in engine installed, forwarded CDP and fixture HTTP ports.
// This test requires RS layer evidence in addition to advancing video frames.
import fs from 'node:fs/promises';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
const exec = promisify(execFile);
const [endpoint, url, output, serial] = process.argv.slice(2);
if (!serial) throw Error('Usage: node software_crop_validation.mjs CDP PAGE OUTPUT SERIAL');
const hdc = process.env.HDC_BIN || '/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc';
await fs.mkdir(output, {recursive: false});
const report = {started: new Date().toISOString(), scope: 'Software SDR crop promotion and screenshot capture', cases: [], cleanupErrors: []};
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
const visible = (rows, plane) => rows.some(line => line.includes(`Name [Lumie${plane}Layer]`) && /Visible: 1\b/.test(line) && /SurfaceNode::hasSurfaceBuffer: 1\b/.test(line));
const snapshot = () => evaluate(`({time:v.currentTime,frames:v.getVideoPlaybackQuality().totalVideoFrames,paused:v.paused,error:v.error?.message,visibility:document.visibilityState,rect:v.getBoundingClientRect().toJSON(),dpr:devicePixelRatio,hdr:matchMedia('(dynamic-range: high)').matches})`);
try {
  report.version = await (await fetch(`${endpoint}/json/version`, {signal: AbortSignal.timeout(5000)})).json();
  browser = await connect(report.version.webSocketDebuggerUrl);
  report.initialSystem = await browser.call('SystemInfo.getInfo');
  target = (await browser.call('Target.createTarget', {url:'about:blank'})).targetId;
  const targets = await (await fetch(`${endpoint}/json/list`)).json();
  page = await connect(targets.find(item => item.id===target).webSocketDebuggerUrl);
  evaluate = async expression => {
    const result = await page.call('Runtime.evaluate', {expression, awaitPromise:true, returnByValue:true, userGesture:true});
    if (result.exceptionDetails) throw Error(JSON.stringify(result.exceptionDetails));
    return result.result.value;
  };
  await browser.call('Target.activateTarget', {targetId:target});
  await page.call('Page.navigate', {url:new URL('automation.html',url).href});
  await waitFor(() => evaluate('!!document.querySelector("video")'), Boolean, 'Fixture load');
  await evaluate(`(async()=>{
    window.v=document.querySelector('video');v.pause();v.removeAttribute('src');v.load();v.remove();
    document.body.replaceChildren();document.documentElement.style.cssText='margin:0;background:white';
    document.body.style.cssText='margin:0;background:white';
    window.c=document.createElement('div');document.body.append(c);c.append(v);
    const marker=document.createElement('div');marker.style.cssText='position:fixed;left:0;top:0;width:12px;height:12px;background:rgb(1,254,7);z-index:99999';document.body.append(marker);
    v.controls=false;v.muted=true;v.loop=true;
    try {window.testWakeLock=await navigator.wakeLock.request('screen')} catch(e) {}
    v.src=${JSON.stringify(new URL('media/software-crop.mp4',url).href)};await v.play();
  })()`);
  await logs();
  // Keep a sufficiently large visible video region: promotion heuristics can
  // legitimately keep tiny clipped videos in the normal compositor.
  const cases = [
    ['full', '20.25,80.25,800,450'],
    ['viewport_right_4px', '0,80,innerWidth*devicePixelRatio+4,742'],
    ['left', '-20.25,80.25,800,450'],
    ['top', '20.25,-40.25,800,450'],
    ['bottom', '20.25,innerHeight*devicePixelRatio-400.25,1000,650'],
    ['source_cover', '20.25,80.25,1000,500'],
    ['overflow_parent', '-50.25,-30.25,1100,650'],
  ];
  for (const [name, dims] of cases) {
    const entry={name};report.cases.push(entry);
    try {
      entry.geometry=await evaluate(`(()=>{
        const [x,y,w,h]=[${dims}];const d=devicePixelRatio;
        c.style.cssText='position:fixed;left:0;top:0;width:100%;height:100%;overflow:visible';
        if(${JSON.stringify(name)}==='overflow_parent')c.style.cssText='position:fixed;left:'+20/d+'px;top:'+80/d+'px;width:'+900/d+'px;height:'+500/d+'px;overflow:hidden';
        v.style.cssText='position:absolute;max-width:none;max-height:none;left:'+x/d+'px;top:'+y/d+'px;width:'+w/d+'px;height:'+h/d+'px;object-fit:'+(${JSON.stringify(name)}==='source_cover'?'cover':'fill')+';filter:none';
        const r=v.getBoundingClientRect(),p=c.getBoundingClientRect();
        const l=Math.max(0,r.left,p.left),t=Math.max(0,r.top,p.top),rr=Math.min(innerWidth,r.right,p.right),b=Math.min(innerHeight,r.bottom,p.bottom);
        return {viewport:{width:innerWidth,height:innerHeight,visualHeight:visualViewport.height},rect:r.toJSON(),parent:p.toJSON(),dpr:d,roi:[l*d,t*d,rr*d,b*d]};
      })()`);
      entry.promoted=await waitFor(layers, rows=>visible(rows,'Web')&&visible(rows,'Video'), name+' promoted');
      const start=await snapshot();
      entry.sustained=await waitFor(snapshot,s=>s.frames>=start.frames+60&&!s.paused&&!s.error,name+' frames');
      await exec('devecocli',['ui','screenshot','--device',serial,'--path',output+'/'+name+'-promoted.png'],{timeout:15000});
      await evaluate("v.style.filter='blur(0.01px)'");
      entry.demoted=await waitFor(layers,rows=>visible(rows,'Web')&&!visible(rows,'Video'),name+' composited baseline');
      await sleep(300);
      await exec('devecocli',['ui','screenshot','--device',serial,'--path',output+'/'+name+'-composited.png'],{timeout:15000});
      events.push(...await logs());
    } catch(e) {
      entry.error=String(e);
      entry.failureSnapshot=await snapshot().catch(()=>null);
      entry.failureLayers=await layers().catch(()=>[]);
      events.push(...await logs().catch(()=>[]));
    }
    await fs.writeFile(`${output}/report.json`,JSON.stringify(report,null,2));
  }
  report.finalSystem=await browser.call('SystemInfo.getInfo');
  if(report.finalSystem.gpu.auxAttributes.processCrashCount!==report.initialSystem.gpu.auxAttributes.processCrashCount)throw Error('GPU restarted');
  report.passed=report.cases.length===7&&report.cases.every(c=>!c.error);
  if(!report.passed)process.exitCode=1;
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
