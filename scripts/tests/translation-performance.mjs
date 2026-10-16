// Synthetic host benchmark; native/OHOS frame-time and process memory are pending.
import fs from 'node:fs/promises';
import assert from 'node:assert/strict';
const {chromium}=await import(process.env.PLAYWRIGHT_MODULE||'playwright');
const browser=await chromium.launch({executablePath:process.env.CHROME_PATH,headless:true});
try {
 const page=await browser.newPage({viewport:{width:480,height:800}});
 await page.setContent(`<style>body{font:18px/1.7 sans-serif}p{min-height:80px}</style>${Array.from({length:4500},(_,i)=>`<p>Synthetic paragraph ${i}. The browser schedules visible bilingual text.</p>`).join('')}`);
 const cdp=await page.context().newCDPSession(page);
 const {frameTree}=await cdp.send('Page.getFrameTree');
 const {executionContextId}=await cdp.send('Page.createIsolatedWorld',{frameId:frameTree.frame.id,worldName:'translation-perf'});
 async function run(expression){const r=await cdp.send('Runtime.evaluate',{contextId:executionContextId,expression,returnByValue:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;}
 await run(`globalThis.timerSamples=[];globalThis.longTasks=[];
 const originalTimeout=setTimeout;
 globalThis.setTimeout=(fn,delay,...args)=>originalTimeout(()=>{const t=performance.now();try{fn(...args)}finally{timerSamples.push(performance.now()-t)}},delay);
 new PerformanceObserver(list=>{for(const e of list.getEntries())longTasks.push(e.duration)}).observe({type:'longtask',buffered:false});`);
 await run(await fs.readFile('overlay/chrome/renderer/ohos/translation_resources/runtime.js','utf8'));
 const samples={take:[],apply:[],presentation:[],stop:[]};
 async function call(cmd){const r=await run(`(()=>{const start=performance.now();const value=__lumieTranslation.call(${JSON.stringify({sessionId:'perf',...cmd})});return {value,ms:performance.now()-start}})()`);if(samples[cmd.op])samples[cmd.op].push(r.ms);return r.value;}
 await call({op:'start',targetLanguage:'zh-CN',style:'standard',prefetchViewportRatio:0});await page.waitForTimeout(1200);
 for(let i=0;i<20;i++){
  await page.evaluate(y=>scrollTo(0,y),i*900);await page.waitForTimeout(230);
  const b=await call({op:'take',batchId:String(i),maxSegments:32,maxTextUnits:12000});
  if(b.segments.length)await call({op:'apply',batchId:String(i),segments:b.segments.map(p=>({...p,translatedText:'【测试】'+p.text}))});
  await page.waitForTimeout(50);
 }
 assert.equal(samples.apply.length,20,'each settled viewport must provide work');
 for(const style of ['quote','gray','standard'])await call({op:'presentation',style,showTranslation:true});
 const state=await call({op:'status'});await call({op:'stop'});
 const metrics=await run('({timerSamples,longTasks})');
 const summarize=a=>{const s=[...a].sort((a,b)=>a-b);return {count:a.length,p50Ms:s[Math.floor((s.length-1)*.5)]||0,p95Ms:s[Math.floor((s.length-1)*.95)]||0,maxMs:s.at(-1)||0};};
 const result={scope:'macOS arm64 host Chrome, 4500 synthetic paragraphs; isolated JS only; timer instrumentation overhead included',browser:browser.version(),viewport:{width:480,height:800},state,commands:Object.fromEntries(Object.entries(samples).map(([k,v])=>[k,summarize(v)])),scheduledTasks:summarize(metrics.timerSamples),longTasks:metrics.longTasks,nativeDevice:'pending'};
 await fs.writeFile('artifacts/translation/performance.json',JSON.stringify(result,null,2)+'\n');console.log(JSON.stringify(result,null,2));
}finally{await browser.close();}
