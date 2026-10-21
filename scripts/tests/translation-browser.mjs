// Host tests for the shipped renderer executor; not native OHOS acceptance.
import fs from 'node:fs/promises';
import assert from 'node:assert/strict';
const {chromium}=await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const browser=await chromium.launch({executablePath:process.env.CHROME_PATH,headless:true});
const page=await browser.newPage({viewport:{width:480,height:800}});
const client=await page.context().newCDPSession(page);
const source=await fs.readFile('overlay/chrome/renderer/ohos/translation_resources/runtime.js','utf8');
let contextId;
async function init(html) {
 if(contextId)await call({op:'stop'});
 await page.setContent(html);
 const {frameTree}=await client.send('Page.getFrameTree');
 ({executionContextId:contextId}=await client.send('Page.createIsolatedWorld',{frameId:frameTree.frame.id,worldName:'translation-test'}));
 // Capture closed roots only in the test world; production keeps them closed.
 await evaluate(`if(!globalThis.testRoots){globalThis.testRoots=new WeakMap();const attach=Element.prototype.attachShadow;Element.prototype.attachShadow=function(options){const root=attach.call(this,options);testRoots.set(this,root);return root;};}`);
 await evaluate(source);
}
async function evaluate(expression) {
 const r=await client.send('Runtime.evaluate',{contextId,expression,returnByValue:true,awaitPromise:true});
 if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));
 return r.result.value;
}
const call=(c)=>evaluate(`globalThis.__lumieTranslation.call(${JSON.stringify({sessionId:'test',...c})})`);
const start=(x={})=>call({op:'start',style:'standard',targetLanguage:'zh-CN',prefetchViewportRatio:0,...x});
const take=(id='b',x={})=>call({op:'take',batchId:id,maxSegments:32,maxTextUnits:12000,...x});
const apply=(b,id='b')=>call({op:'apply',batchId:id,segments:b.segments.map(s=>({...s,translatedText:'译文 '+s.text}))});
let passed=0;
function ok(name){console.log('PASS',name);passed++;}
try {
 await init('<style>body{font:18px/1.6 sans-serif}</style><p id="loading">A paragraph waiting for translation.</p>');
 await start({style:'quote'});await page.waitForTimeout(300);
 const content=`testRoots.get(document.querySelector('#loading').nextElementSibling)?.querySelector('span')`;
 assert.equal(await evaluate(`(${content}).getAttribute('aria-busy')`),'true');
 assert.equal(await evaluate(`getComputedStyle(${content},'::before').animationName`),'lumie-spin');
 assert.equal(await evaluate(`getComputedStyle(${content}).borderInlineStartWidth`),'4px');
 await page.screenshot({path:'artifacts/translation/loading-quote.png'});
 await page.emulateMedia({reducedMotion:'reduce'});
 assert.equal(await evaluate(`getComputedStyle(${content},'::before').animationName`),'none');
 await page.emulateMedia({reducedMotion:'no-preference'});
 await call({op:'presentation',style:'gray',showTranslation:false});
 assert.equal(await evaluate(`getComputedStyle(document.querySelector('#loading').nextElementSibling).display`),'none');
 await call({op:'presentation',style:'quote',showTranslation:true});
 let loadingBatch=await take();await apply(loadingBatch);await page.waitForTimeout(150);
 assert.equal(await evaluate(`(${content}).getAttribute('aria-busy')`),null);
 assert.equal(await evaluate(`getComputedStyle(${content},'::before').animationName`),'none');
 assert.equal(await evaluate(`getComputedStyle(${content}).borderInlineStartStyle`),'solid');
 await page.screenshot({path:'artifacts/translation/completed-quote.png'});
 ok('pending paragraph animates, respects reduced motion and becomes a quoted translation');
 await init('<p id="loading">A failing paragraph.</p>');await start();await page.waitForTimeout(250);
 loadingBatch=await take();await call({op:'release',batchId:'b',retryable:false,reason:'providerError'});await page.waitForTimeout(100);
 assert.equal(await evaluate('document.querySelector("p + span")'),null);
 ok('terminal failure removes the loading indicator');
 await init('<p>A credential failure.</p><p>Another waiting paragraph.</p>');await start();await page.waitForTimeout(250);
 await take('auth-loading',{maxSegments:1});await call({op:'release',batchId:'auth-loading',retryable:true,reason:'authentication'});
 assert.equal(await evaluate('document.querySelectorAll("p + span").length'),0);
 ok('authentication failure removes all pending indicators');
 await init(`<style>body{font:18px/1.6 sans-serif;margin:20px}p{min-height:170px}</style>${Array.from({length:40},(_,i)=>`<p id="p${i}">Paragraph ${i} text for viewport translation.</p>`).join('')}`);
 assert.equal(await page.evaluate(()=>typeof globalThis.__lumieTranslation),'undefined');ok('isolated-world API is absent from page global');
 await start();await page.waitForTimeout(400);
 let b=await take();assert(b.segments.length>0);assert(b.segments.every(s=>!s.text.includes('Paragraph 39')));ok('initial batch excludes offscreen page tail');
 assert.equal((await take('parallel')).segments.length,0);ok('concurrent take does not duplicate leased segments');
 let applied=await apply(b);assert.equal(applied.applied,b.segments.length);await page.waitForTimeout(200);
 assert.equal((await take('again')).segments.length,0);ok('translated viewport reuses results');
 for(const style of ['standard','quote','gray']) {
  await call({op:'presentation',style,showTranslation:true});
  await page.screenshot({path:`artifacts/translation/${style}.png`});
  assert.equal((await take('style-'+style)).segments.length,0);
 }
 ok('three styles reuse results without new batches');
 await call({op:'presentation',style:'gray',showTranslation:false});assert.equal((await take()).segments.length,0);
 await call({op:'presentation',style:'gray',showTranslation:true});ok('original-only view pauses work and restores cached translation');
 await page.evaluate(()=>scrollTo(0,document.body.scrollHeight));assert.equal((await take('fast')).segments.length,0);
 await page.waitForTimeout(500);
 // Newly inserted pending rows add height below the original paragraphs.
 await page.locator('#p39').scrollIntoViewIfNeeded();await page.waitForTimeout(500);
 b=await take('tail');assert(b.segments.some(s=>s.text.includes('Paragraph 39')));assert(b.segments.length<10);ok('fast scroll settles on current viewport without queued middle article');
 await page.evaluate(()=>document.querySelector('#p39').textContent='Recycled virtual row');
 applied=await apply(b,'tail');assert(applied.results.some(r=>r.reason==='staleSegment'));ok('changed source rejects late results');
 assert.equal((await apply(b,'tail')).reason,'batchExpired');ok('settled batch cannot be replayed');
 await call({op:'stop'});assert.equal(await evaluate('document.querySelectorAll("p + span").length'),0);ok('stop removes inserted nodes');
 await init(`<p id="good">Allowed <b>inline emphasis</b><span hidden>SECRET</span><input value="PASSWORD"></p><p translate="no">NO_TRANSLATE</p><pre>CODE</pre><p contenteditable="true">EDITABLE</p><form><p>FORM</p></form><p class="notranslate">NOTRANSLATE</p><p style="display:none">HIDDEN</p>`);
 await start();await page.waitForTimeout(250);b=await take();assert.deepEqual(b.segments.map(s=>s.text),['Allowed inline emphasis']);ok('excluded form/editable/code/hidden/translate=no text never leaves renderer');
 const malicious=b.segments.map(s=>({...s,translatedText:'<img src=x onerror="globalThis.pwned=1">'}));
 await call({op:'apply',batchId:'b',segments:malicious});await page.waitForTimeout(150);
 assert.equal(await page.evaluate(()=>globalThis.pwned),undefined);assert.equal(await evaluate('document.querySelector("#good").nextElementSibling.tagName'),'SPAN');ok('HTML-like result remains plain text');
 await call({op:'stop'});
 await init(`<p id="long">${'a'.repeat(1999)}😀${'z'.repeat(2100)}</p>`);await start();await page.waitForTimeout(300);b=await take();assert(b.segments.length>=3);assert.equal(b.segments.map(s=>s.text).join(''),'a'.repeat(1999)+'😀'+'z'.repeat(2100));assert(b.segments.every(s=>!/[\uD800-\uDBFF]$/.test(s.text)));ok('long paragraphs split without breaking surrogate pairs');
 assert.equal((await call({op:'apply',sessionId:'old',batchId:'b',segments:[]})).reason,'sessionEnded');ok('old session cannot write');
 await call({op:'release',batchId:'b',retryable:false});assert.equal((await take('released')).segments.length,0);ok('nonretryable failure is terminal');
 await init('<style>body{color-scheme:light dark;color:light-dark(#111,#eee);background:light-dark(white,#111)}</style><p>Strict policy text.</p><meta http-equiv="Content-Security-Policy" content="default-src \'none\'; style-src \'none\'; require-trusted-types-for \'script\'">');
 await start({targetLanguage:'ar',style:'gray'});await page.waitForTimeout(250);b=await take();await call({op:'apply',batchId:'b',segments:b.segments.map(p=>({...p,translatedText:'ترجمة تجريبية للنص الأصلي.'}))});await page.waitForTimeout(200);
 await page.emulateMedia({colorScheme:'dark'});await page.screenshot({path:'artifacts/translation/gray-dark-rtl.png'});
 assert.equal(await evaluate('document.querySelector("p").nextElementSibling.tagName'),'SPAN');ok('strict CSP/Trusted Types text insertion and dark RTL screenshot');
 await init('<p>Retryable paragraph</p>');await start();await page.waitForTimeout(250);
 await evaluate('globalThis.clockOffset=0;globalThis.realClock=performance.now.bind(performance);performance.now=()=>realClock()+clockOffset');
 b=await take('lease');assert(b.segments.length);
 await evaluate('clockOffset+=46000');let status=await call({op:'status'});assert(status.expired.includes('lease'));ok('45-second lease expires and frees concurrency slot');
 await evaluate('clockOffset+=3000');b=await take('retry2');assert(b.segments.length);await call({op:'release',batchId:'retry2',reason:'providerError',retryable:true});
 await evaluate('clockOffset+=5000');b=await take('retry3');assert(b.segments.length);await call({op:'release',batchId:'retry3',reason:'timeout',retryable:true});
 await evaluate('clockOffset+=60000');assert.equal((await take('retry4')).segments.length,0);ok('at most two additional retries, with backoff');
 await evaluate('performance.now=realClock');
 await init('<p>Quota control</p>');await start();await page.waitForTimeout(250);await take('quota');
 status=await call({op:'release',batchId:'quota',reason:'authentication',retryable:true});assert.equal(status.state,'error');assert.equal((await take('auth')).segments.length,0);ok('authentication failure pauses provider work until explicit new session');
 await init('<p id="one">First paragraph</p><p id="two">Second paragraph</p>');await start();await page.waitForTimeout(250);b=await take('partial');
 await page.evaluate(()=>document.querySelector('#two').remove());let result=await apply(b,'partial');assert.equal(result.applied,1);assert.equal(result.rejected,1);ok('removed source partially rejects an otherwise valid batch');
 await call({op:'stop'});await start();await page.waitForTimeout(250);b=await take('huge');
 result=await call({op:'apply',batchId:'huge',segments:b.segments.map(s=>({...s,translatedText:'x'.repeat(8001)}))});assert.equal(result.applied,0);assert(result.results.every(r=>r.reason==='tooLarge'));ok('oversized translation is rejected');
 await init('<p id="a">Dynamic initial</p>');await start();await page.waitForTimeout(250);await apply(await take());await page.waitForTimeout(100);
 await page.evaluate(()=>document.body.append(Object.assign(document.createElement('p'),{textContent:'Added after translation'})));await page.waitForTimeout(250);b=await take('dynamic');assert.deepEqual(b.segments.map(s=>s.text),['Added after translation']);ok('new visible nodes are discovered without translating existing results');
 await call({op:'stop'});await page.evaluate(()=>document.body.append(Object.assign(document.createElement('p'),{textContent:'After stop'})));assert.equal((await take()).reason,'sessionEnded');ok('stopped observer does not collect later mutations');
 await init('<p>First batch one</p><p>Second batch two</p>');await start();await page.waitForTimeout(200);
 const first=await take('order1',{maxSegments:1}),second=await take('order2',{maxSegments:1});assert.equal(first.segments.length,1);assert.equal(second.segments.length,1);assert.equal((await apply(second,'order2')).applied,1);assert.equal((await apply(first,'order1')).applied,1);ok('two in-flight batches can resolve out of order');
 await init(`<p>${'x'.repeat(600000)}</p><p>${'y'.repeat(600000)}</p>`);await start();await page.waitForTimeout(500);status=await call({op:'status'});assert.equal(status.limitReached,true);assert(status.retainedTextUnits<=1048576);ok('logical text quota is bounded with limitReached');
 await init(Array.from({length:6000},(_,i)=>`<p>Row ${i}</p>`).join(''));await start();await page.waitForTimeout(2500);status=await call({op:'status'});assert.equal(status.limitReached,true);assert(status.discovered<=5000);ok('node/segment cap bounds large pages');
 await init('<div id="scroller" style="height:80px;overflow:hidden"><p>Visible clipped-container text</p><p style="margin-top:100px">CLIPPED_SECRET</p></div>');await start();await page.waitForTimeout(250);b=await take();assert(b.segments.length);assert(b.segments.every(p=>!p.text.includes('CLIPPED_SECRET')));ok('ancestor overflow clipping excludes non-visible text');
 await init('<div id="scroller" style="height:150px;overflow:auto"><p>Top inner paragraph</p><p style="margin-top:400px">Bottom inner paragraph</p></div>');await start();await page.waitForTimeout(250);await page.evaluate(()=>document.querySelector('#scroller').scrollTop=500);await page.waitForTimeout(30);assert.equal((await take()).segments.length,0);await page.waitForTimeout(250);b=await take('inner');assert(b.segments.some(p=>p.text.includes('Bottom inner')));ok('nested scroll also waits for settling');
 console.log(JSON.stringify({passed,browser:browser.version(),scope:'host isolated-world executor; native IPC not tested'}));
} finally {await browser.close();}
