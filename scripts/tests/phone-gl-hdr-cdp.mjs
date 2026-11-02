import fs from 'node:fs/promises';
// Forward the device's local CDP port before running; Node 22+ is required.
// Creates and closes only its own diagnostic tab.
const endpoint=process.argv[2] || 'http://127.0.0.1:19422';
const version=await (await fetch(endpoint+'/json/version')).json();
async function connect(url) {
 const ws=new WebSocket(url); await new Promise((res,rej)=>{ws.onopen=res;ws.onerror=rej});
 let seq=0;const pending=new Map();
 ws.onmessage=e=>{const m=JSON.parse(e.data);const p=pending.get(m.id);if(p){pending.delete(m.id);m.error?p.reject(Error(JSON.stringify(m.error))):p.resolve(m.result)}};
 return {ws,call(method,params={}) {return new Promise((resolve,reject)=>{const id=++seq;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}));setTimeout(()=>{if(pending.delete(id))reject(Error(method+' timeout'))},15000).unref()})}};
}
const browser=await connect(version.webSocketDebuggerUrl);
let target, client;
try {
 const root=new URL('../../docs/test-pages/color-output/', import.meta.url);
 const script=await fs.readFile(new URL('probe.js',root),'utf8');
 const html=(await fs.readFile(new URL('index.html',root),'utf8')).replace('<script src="probe.js"></script>','<script>'+script+'</script>');
 const url='data:text/html;base64,'+Buffer.from(html).toString('base64');
 target=(await browser.call('Target.createTarget',{url})).targetId;
 await new Promise(r=>setTimeout(r,1800));
 const page=(await (await fetch(endpoint+'/json/list')).json()).find(t=>t.id===target);
 const c=client=await connect(page.webSocketDebuggerUrl);
 const results=[];
 for (const mode of ['2d-srgb','2d-p3-hdr','2d-srgb']) {
  await c.call('Runtime.evaluate',{expression:`document.querySelector('#mode').value=${JSON.stringify(mode)};run();`});
  await new Promise(r=>setTimeout(r,1200));
  const result=await c.call('Runtime.evaluate',{expression:'JSON.stringify(report)',returnByValue:true});
  results.push(JSON.parse(result.result.value));
 }
 if (process.argv[3]) await fs.writeFile(process.argv[3],JSON.stringify(results,null,2));
 console.log(JSON.stringify(results,null,2));
 c.ws.close();
 for (const result of results) {
  if (!result.hdr || result.error || result.leftPixel.join(',') !== '0,255,0,255')
   throw Error('HDR capability/rendering lost during ' + result.mode);
 }
 if (!results[1].hdrConfigured) throw Error('HDR canvas was not configured');
} finally {
 client?.ws.close();
 if(target)await browser.call('Target.closeTarget',{targetId:target}).catch(()=>{});
 browser.ws.close();
}
