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
 await page.setContent(html);
 const {frameTree}=await client.send('Page.getFrameTree');
 ({executionContextId:contextId}=await client.send('Page.createIsolatedWorld',{frameId:frameTree.frame.id,worldName:'translation-test'}));
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
 await page.waitForTimeout(500);b=await take('tail');assert(b.segments.some(s=>s.text.includes('Paragraph 39')));assert(b.segments.length<10);ok('fast scroll settles on current viewport without queued middle article');
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
 await init('<p>Strict policy text.</p><meta http-equiv="Content-Security-Policy" content="default-src \'none\'; style-src \'none\'; require-trusted-types-for \'script\'">');
 await start({targetLanguage:'ar',style:'gray'});await page.waitForTimeout(250);b=await take();await apply(b);await page.waitForTimeout(200);
 await page.emulateMedia({colorScheme:'dark'});await page.screenshot({path:'artifacts/translation/gray-dark-rtl.png'});
 assert.equal(await evaluate('document.querySelector("p").nextElementSibling.tagName'),'SPAN');ok('strict CSP/Trusted Types text insertion and dark RTL screenshot');
 console.log(JSON.stringify({passed,browser:browser.version(),scope:'host isolated-world executor; native IPC not tested'}));
} finally {await browser.close();}
