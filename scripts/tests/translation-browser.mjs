// Host tests for the shipped renderer executor; not native OHOS acceptance.
import fs from 'node:fs/promises';
import assert from 'node:assert/strict';
const {chromium}=await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const browser=await chromium.launch({executablePath:process.env.CHROME_PATH,headless:true});
const page=await browser.newPage({viewport:{width:480,height:800}});
const client=await page.context().newCDPSession(page);
const source=await fs.readFile('overlay/chrome/renderer/ohos/translation_resources/runtime.js','utf8');
let contextId;
async function init(html,url) {
 if(contextId)await call({op:'stop'});
 if(url){await page.route(url,route=>route.fulfill({contentType:'text/html',body:html}));await page.goto(url);}
 else await page.setContent(html);
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
 await init('<html lang="en"><body><p id="native">这是一个已经是中文的段落，不需要再次翻译。</p><p id="foreign">This paragraph still needs translation.</p><p id="mixed">这是中文，但是 this sentence needs translation.</p></body></html>');
 await start();await page.waitForTimeout(250);
 let languageBatch=await take('language-mixed');
 assert.deepEqual(languageBatch.segments.map(s=>s.text),['This paragraph still needs translation.','这是中文，但是 this sentence needs translation.']);
 assert.equal(await evaluate('testRoots.has(document.querySelector("#native").nextElementSibling)'),false);
 assert.equal(await page.locator('#native').textContent(),'这是一个已经是中文的段落，不需要再次翻译。');
 ok('target-language paragraphs skip requests and loading independently of page lang and neighboring mixed prose');
 for(const [targetLanguage,skip,send] of [
  ['zh-CN','这是一个已经是中文的段落，不需要再次翻译。','這是一個已經是中文的段落，不需要再次翻譯。'],
  ['zh-Hant','這是一個已經是中文的段落，不需要再次翻譯。','这是一个已经是中文的段落，不需要再次翻译。'],
  ['zh-TW','這是一個已經是中文的段落，不需要再次翻譯。','这是一个已经是中文的段落，不需要再次翻译。'],
  ['ja','これはすでに日本語の文章です。翻訳する必要はありません。','これは日本語です。這是一個中文段落需要翻譯。'],
  ['ko','이 문장은 이미 한국어로 작성되어 있습니다.','이 문장은 한국어입니다. This needs translation.'],
  ['el','Αυτή η παράγραφος είναι ήδη στα ελληνικά.','Αυτή είναι ελληνική. Translate this sentence.'],
  ['th','ข้อความนี้เป็นภาษาไทยอยู่แล้ว','ข้อความนี้เป็นภาษาไทย Translate this.']
 ]) {
  await init(`<p id="same">${skip}</p><p>${send}</p>`);await start({targetLanguage});await page.waitForTimeout(180);
  languageBatch=await take('language-variants');assert.deepEqual(languageBatch.segments.map(s=>s.text),[send],targetLanguage);
  assert.equal(await evaluate('testRoots.has(document.querySelector("#same").nextElementSibling)'),false);
 }
 ok('Chinese variants remain distinct; Japanese, Korean, Greek and Thai skip only conservative target-script runs');
 for(const [targetLanguage,text] of [
  ['zh-CN','你好'],['zh-CN','这是一个中文段落，其中有 API 这个词。'],
  ['zh-CN','這是一個已经是中文的混合段落。'],['ja','日本語中文漢字文章内容'],
  ['en','This paragraph is already in English but cannot be distinguished safely by script alone.'],
  ['fr','Ce paragraphe est déjà en français et partage son alphabet avec plusieurs langues.'],
  ['ru','Этот абзац написан кириллицей, общей для нескольких языков.'],
  ['ja-Latn','これはすでに日本語の文章です。'],
  ['zh','日本語漢字文章内容段落'],
  ['zh-Latn','这是一个已经是中文的段落，不需要再次翻译。'],
  ['zh-Hant','这是一个含有后发干等歧义转换的中文段落。'],
  ['zh-unknown','这是一个已经是中文的段落，不需要再次翻译。']
 ]) {
  await init(`<p lang="${targetLanguage}">${text}</p>`);await start({targetLanguage});await page.waitForTimeout(180);
  assert.deepEqual((await take('language-uncertain')).segments.map(s=>s.text),[text]);
 }
 ok('short, mixed, ambiguous Chinese and shared-script languages still translate even with matching lang hints');
 await init('<p id="language-change">这是一个已经是中文的段落，不需要再次翻译。</p>');await start();await page.waitForTimeout(180);
 assert.equal((await take('skipped-original')).segments.length,0);
 await page.locator('#language-change').evaluate(el=>el.textContent='Changed to English and now needs translation.');await page.waitForTimeout(180);
 languageBatch=await take('changed-language');assert.equal(languageBatch.segments.length,1);
 await page.locator('#language-change').evaluate(el=>el.textContent='这是一个已经是中文的段落，不需要再次翻译。');
 assert.equal((await apply(languageBatch,'changed-language')).results[0].reason,'staleSegment');await page.waitForTimeout(180);
 assert.equal((await take('skip-again')).segments.length,0);
 assert.equal(await page.locator('#language-change + span').count(),0);
 ok('language changes reclassify skipped paragraphs and invalidate late foreign-language results');
 await init('<p>这是一个已经是中文的段落，<code>English protected code</code>不需要再次翻译。</p>');
 await start();await page.waitForTimeout(180);assert.equal((await take('protected-language')).segments.length,0);
 assert.equal((await call({op:'status'})).retainedTextUnits,0);
 ok('protected code does not change prose language and skipped runs consume no retained-text budget');
 await init('<p>这是一个已经是中文的段落，不需要再次翻译。</p>');
 await start({targetLanguage:'en'});await page.waitForTimeout(180);
 assert.equal((await take('new-target')).segments.length,1);
 await start({targetLanguage:'zh-CN'});await page.waitForTimeout(180);
 assert.equal((await take('same-target')).segments.length,0);
 ok('a new session target reclassifies the same document instead of reusing a skip decision');
 await init(`<p>${'这是一个已经是中文的段落。'.repeat(170)} This English sentence still needs translation.</p>`);
 await start();await page.waitForTimeout(180);languageBatch=await take('long-mixed');
 assert(languageBatch.segments.length>=2);
 assert(languageBatch.segments[0].text.startsWith('这是一个'));
 assert(languageBatch.segments.some(s=>s.text.includes('This English sentence')));
 await init(`<p>${'这是一个已经是中文的段落。'.repeat(650)}</p>`);
 await start();await page.waitForTimeout(180);assert((await take('bounded-language')).segments.length>0);
 ok('language checks precede splitting without dropping Chinese chunks of mixed runs; oversized checks fall back to translation');
 await init('<style>body{font:18px/1.6 sans-serif}</style><p id="loading">A paragraph waiting for translation.</p>');
 await start({style:'quote',uiLanguage:'zh-CN'});await page.waitForTimeout(300);
 const content=`testRoots.get(document.querySelector('#loading').nextElementSibling)?.querySelector('span')`;
 assert.equal(await evaluate(`(${content}).getAttribute('aria-busy')`),'true');
 assert.equal(await evaluate(`(${content}).textContent`),'正在翻译...');
 assert.equal(await evaluate(`getComputedStyle(${content}).animationName`),'lumie-shimmer');
 assert.equal(await evaluate(`getComputedStyle(${content}).backgroundClip`),'text');
 assert.equal(await evaluate(`getComputedStyle(${content}).borderInlineStartWidth`),'4px');
 await page.screenshot({path:'artifacts/translation/loading-quote.png'});
 await page.emulateMedia({reducedMotion:'reduce'});
 assert.equal(await evaluate(`getComputedStyle(${content}).animationName`),'none');
 await page.emulateMedia({reducedMotion:'no-preference'});
 await call({op:'presentation',style:'gray',showTranslation:false});
 assert.equal(await evaluate(`getComputedStyle(document.querySelector('#loading').nextElementSibling).display`),'none');
 await call({op:'presentation',style:'quote',showTranslation:true});
 let loadingBatch=await take();await apply(loadingBatch);await page.waitForTimeout(150);
 assert.equal(await evaluate(`(${content}).getAttribute('aria-busy')`),null);
 assert.equal(await evaluate(`getComputedStyle(${content}).animationName`),'none');
 assert.equal(await evaluate(`getComputedStyle(${content}).borderInlineStartStyle`),'solid');
 await page.screenshot({path:'artifacts/translation/completed-quote.png'});
 ok('pending paragraph animates, respects reduced motion and becomes a quoted translation');
 for(const [uiLanguage,label] of [['en','Translating...'],['zh-Hant','正在翻譯...'],['ja','翻訳中...'],['ar','جارٍ الترجمة...'],['unknown','Translating...'],['constructor','Translating...']]) {
  await init('<p>Localized pending paragraph.</p>');await start({uiLanguage});await page.waitForTimeout(180);
  assert.equal(await evaluate('testRoots.get(document.querySelector("p").nextElementSibling).querySelector("span").textContent'),label);
 }
 ok('loading labels support multiple interface languages with English fallback');
 await init('<nav><div>Navigation secret</div></nav><div role="toolbar"><p>Toolbar secret</p></div><div role="button">Action secret</div><footer><p>Footer secret</p></footer><main><h1>Article heading</h1><p>Article prose <time>Yesterday</time><kbd>Ctrl+C</kbd><math><mi>x</mi></math></p></main>');
 await start();await page.waitForTimeout(250);let rulesBatch=await take('rules');
 assert.deepEqual(rulesBatch.segments.map(s=>s.text),['Article heading','Article prose [[LUMIE_KEEP_0_0]][[LUMIE_KEEP_0_1]]']);
 ok('navigation, actions and timestamps are excluded; shortcuts and formulas leave only opaque markers');
 await init('<p id="protected">Click <code onclick="globalThis.pwned=1">Settings<span hidden>HIDDEN_CODE</span></code> and solve <math onclick="globalThis.pwned=2"><msup><mi>x</mi><mn>2</mn></msup><annotation-xml><script>BAD_MATH</script></annotation-xml></math>.</p>');
 const originalProtected=await page.locator('#protected').innerHTML();
 await start();await page.waitForTimeout(250);rulesBatch=await take('protected');
 assert.deepEqual(rulesBatch.segments.map(s=>s.text),['Click [[LUMIE_KEEP_0_0]] and solve [[LUMIE_KEEP_0_1]].']);
 await call({op:'apply',batchId:'protected',segments:rulesBatch.segments.map(s=>({...s,translatedText:'计算 [[LUMIE_KEEP_0_1]]，再点 [[LUMIE_KEEP_0_0]]。'}))});await page.waitForTimeout(100);
 const protectedRoot='testRoots.get(document.querySelector("#protected").nextElementSibling)';
 assert.equal(await evaluate(`${protectedRoot}.querySelector('code').textContent`),'Settings');
 assert.equal(await evaluate(`getComputedStyle(${protectedRoot}.querySelector('code span')).display`),'inline');
 assert.equal(await evaluate(`${protectedRoot}.querySelector('math msup mi').textContent`),'x');
 assert.equal(await evaluate(`${protectedRoot}.querySelectorAll('[onclick],script,annotation-xml,input').length`),0);
 await page.screenshot({path:'artifacts/translation/protected-inline.png'});
 await call({op:'stop'});assert.equal(await page.locator('#protected').innerHTML(),originalProtected);
 ok('opaque markers restore local code and safe MathML in translated order without copying executable markup');
 for(const result of ['Missing marker','Twice [[LUMIE_KEEP_0_0]][[LUMIE_KEEP_0_0]]','Unknown [[LUMIE_KEEP_9_0]]']) {
  await init('<p>Click <code>Settings</code>.</p>');await start();await page.waitForTimeout(180);rulesBatch=await take('bad-marker');
  const response=await call({op:'apply',batchId:'bad-marker',segments:rulesBatch.segments.map(s=>({...s,translatedText:result}))});
  assert.equal(response.applied,0);assert.equal(response.results[0].reason,'invalidResult');
  await page.waitForTimeout(60);assert.equal(await evaluate('document.querySelector("p + span")'),null);
 }
 ok('missing, duplicated and invented placeholders are rejected rather than showing damaged translations');
 await init('<p>Click <code id="code">Old value</code>.</p>');await start();await page.waitForTimeout(180);rulesBatch=await take('changed-code');
 await page.evaluate(()=>document.querySelector('#code').textContent='New value');
 assert.equal((await apply(rulesBatch,'changed-code')).results[0].reason,'staleSegment');
 ok('protected-content changes invalidate late results even when encoded markers remain identical');
 await init(`<p>${'x'.repeat(1995)}<code>Settings</code> suffix</p>`);await start();await page.waitForTimeout(180);rulesBatch=await take('marker-boundary');
 assert.equal(rulesBatch.segments.length,2);assert.equal(rulesBatch.segments[0].text.length,1995);
 assert(rulesBatch.segments[1].text.startsWith('[[LUMIE_KEEP_0_0]]'));
 ok('2000-unit source splitting never cuts a generated placeholder');
 await init('<p>Literal [[LUMIE_KEEP_0_0]] twice [[LUMIE_KEEP_0_0]] and <code>Preserved</code>.</p>');await start();await page.waitForTimeout(180);rulesBatch=await take('literal-marker');
 assert(rulesBatch.segments[0].text.includes('[[LUMIE_KEEP_1_0]]'));
 assert.equal((await apply(rulesBatch,'literal-marker')).applied,1);await page.waitForTimeout(80);
 assert.equal(await evaluate('testRoots.get(document.querySelector("p").nextElementSibling).querySelector("code").textContent'),'Preserved');
 ok('literal marker text cannot collide with local replacements');
 for(const layout of ['flex','grid']) {
  await init(`<div id="layout" style="display:${layout};${layout==='grid'?'grid-template-columns:1fr 1fr':''}"><p id="layout-p">A paragraph in a layout item</p><span id="layout-span">An inline layout item</span></div>`);
  await start();await page.waitForTimeout(200);rulesBatch=await take('layout');
  assert.deepEqual(rulesBatch.segments.map(s=>s.text),['A paragraph in a layout item','An inline layout item']);
  await apply(rulesBatch,'layout');await page.waitForTimeout(100);
  assert.equal(await page.locator('#layout > *').count(),2);
  assert.equal(await page.locator('#layout-p > span, #layout-span > span').count(),2);
  await call({op:'stop'});assert.equal(await page.locator('#layout > *').count(),2);
 }
 ok('Flex/Grid item translations remain inside their original item without adding rows or columns');
 await init('<div style="display:flex"><h2 id="flex-title" style="display:flex;gap:8px;width:300px;flex-wrap:nowrap">Click <code>Settings</code></h2></div>');
 await start();await page.waitForTimeout(200);rulesBatch=await take('flex-title');
 assert.deepEqual(rulesBatch.segments.map(s=>s.text),['Click [[LUMIE_KEEP_0_0]]']);
 await apply(rulesBatch,'flex-title');await page.waitForTimeout(100);
 assert.equal(await page.locator('#flex-title').evaluate(el=>el.parentElement.children.length),1);
 assert.equal(await page.locator('#flex-title').evaluate(el=>getComputedStyle(el).flexWrap),'wrap');
 assert(await page.locator('#flex-title').evaluate(el=>el.lastElementChild.getBoundingClientRect().top>=el.querySelector('code').getBoundingClientRect().bottom));
 await call({op:'presentation',style:'standard',showTranslation:false});
 assert.equal(await page.locator('#flex-title').evaluate(el=>getComputedStyle(el).flexWrap),'nowrap');
 await call({op:'presentation',style:'standard',showTranslation:true});await page.waitForTimeout(100);
 assert.equal(await page.locator('#flex-title').evaluate(el=>getComputedStyle(el).flexWrap),'wrap');
 await call({op:'stop'});assert.equal(await page.locator('#flex-title').evaluate(el=>el.style.flexWrap),'nowrap');
 ok('a flex heading with direct text and code gets a full-width translation row and reversible wrapping');
 await init('<div id="clip" style="height:80px;max-height:80px;overflow:hidden"><p id="clip-p" style="margin:0;font:20px/20px sans-serif">Visible clipped paragraph</p><p style="margin-top:220px">CLIPPED_SECRET</p></div>');
 await start();await page.waitForTimeout(200);rulesBatch=await take('clipping');
 assert(rulesBatch.segments.every(s=>!s.text.includes('CLIPPED_SECRET')));
 await apply(rulesBatch,'clipping');await page.waitForTimeout(150);
 assert.equal(await page.locator('#clip').evaluate(el=>getComputedStyle(el).overflow),'hidden');
 assert(await page.locator('#clip').evaluate(el=>el.clientHeight>80));
 assert((await take('still-clipped')).segments.every(s=>!s.text.includes('CLIPPED_SECRET')));
 await call({op:'presentation',style:'standard',showTranslation:false});
 assert.equal(await page.locator('#clip').evaluate(el=>el.clientHeight),80);
 await call({op:'presentation',style:'standard',showTranslation:true});
 assert(await page.locator('#clip').evaluate(el=>el.clientHeight>80));
 await call({op:'stop'});
 assert.equal(await page.locator('#clip').evaluate(el=>[el.style.height,el.style.maxHeight,el.style.overflow].join('|')),'80px|80px|hidden');
 ok('clipped containers grow only for translation, preserve clipping and restore on hide/stop');
 await init('<div style="display:flex"><p id="clamped" style="display:-webkit-box;-webkit-box-orient:vertical;-webkit-line-clamp:1;width:180px;height:20px;max-height:20px;overflow:hidden;font:16px/20px sans-serif">A long paragraph with enough original words to require several lines before its translation.</p></div>');
 await start();await page.waitForTimeout(200);rulesBatch=await take('clamped');await apply(rulesBatch,'clamped');await page.waitForTimeout(100);
 assert(await page.locator('#clamped').evaluate(el=>el.scrollHeight<=el.clientHeight+1));
 assert.equal(await page.locator('#clamped').evaluate(el=>getComputedStyle(el).webkitLineClamp),'none');
 await call({op:'stop'});assert.equal(await page.locator('#clamped').evaluate(el=>[el.style.height,el.style.maxHeight,el.style.webkitLineClamp].join('|')),'20px|20px|1');
 ok('a clamped leaf reserves space for its original and result, then restores its exact clamp properties');
 await init('<div id="scroll" style="height:80px;overflow:auto"><p style="margin:0">Scrollable source</p></div>');
 await start();await page.waitForTimeout(200);rulesBatch=await take('scroll-preserved');await apply(rulesBatch,'scroll-preserved');await page.waitForTimeout(100);
 assert.equal(await page.locator('#scroll').evaluate(el=>[el.style.height,getComputedStyle(el).overflow].join('|')),'80px|auto');
 ok('intentional scroll containers keep their viewport and scrolling behavior');
 await init('<div id="clip" style="height:80px;overflow:hidden"><p style="margin:0">First text</p><p style="margin:0">Second text</p></div>');
 await start();await page.waitForTimeout(200);rulesBatch=await take('shared-clip');await apply(rulesBatch,'shared-clip');await page.waitForTimeout(100);
 await page.evaluate(()=>document.querySelector('#clip').style.height='200px');
 await call({op:'stop'});
 assert.equal(await page.locator('#clip').evaluate(el=>el.style.height),'200px');
 ok('shared layout ownership never restores over a subsequent page-authored height');
 await init('<article id="mixed">Introduction <em>with emphasis</em><p>Nested paragraph</p>Afterword <a href="/">with link</a></article>');
 await start();await page.waitForTimeout(250);rulesBatch=await take('mixed');
 assert.deepEqual(rulesBatch.segments.map(s=>s.text),['Introduction with emphasis','Nested paragraph','Afterword with link']);
 await apply(rulesBatch,'mixed');await page.waitForTimeout(100);
 assert.deepEqual(await evaluate('[...document.querySelector("article").childNodes].filter(n=>testRoots.has(n)).map(n=>testRoots.get(n).textContent)'),['译文 Introduction with emphasis','译文 Nested paragraph','译文 Afterword with link']);
 await page.evaluate(()=>document.querySelector('#mixed').firstChild.textContent='Updated introduction ');
 await page.waitForTimeout(200);rulesBatch=await take('mixed-update');
 assert(rulesBatch.segments.some(s=>s.text==='Updated introduction with emphasis'));
 await call({op:'stop'});assert.equal(await evaluate('document.querySelectorAll("article > span").length'),0);
 ok('mixed direct text and nested paragraphs retain order and track updates');
 await init('<ul><li id="item">List item</li></ul><table><caption>Table caption</caption><tbody><tr><th>Column heading</th><td id="cell">Cell prose</td></tr></tbody></table><figure><figcaption>Figure caption</figcaption></figure>');
 await start({style:'quote',targetLanguage:'ar'});await page.waitForTimeout(250);rulesBatch=await take('structure');
 assert.deepEqual(rulesBatch.segments.map(s=>s.text),['List item','Table caption','Column heading','Cell prose','Figure caption']);
 await apply(rulesBatch,'structure');await page.waitForTimeout(100);
 assert.equal(await evaluate('document.querySelectorAll("ul > span, tr > span, tbody > span, table > span").length'),0);
 assert.equal(await evaluate('document.querySelectorAll("li > span, td > span, th > span, caption > span, figcaption > span").length'),5);
 assert.equal(await evaluate('testRoots.get(document.querySelector("#cell > span")).querySelector("span").dir'),'auto');
 await page.screenshot({path:'artifacts/translation/structured-quote.png'});
 await call({op:'stop'});assert.equal(await evaluate('document.querySelectorAll("li > span, td > span, th > span").length'),0);
 ok('list and table translations stay inside their semantic containers and clean up');
 await init('<div id="changing">Before<p>Nested</p></div>');await start();await page.waitForTimeout(200);
 await apply(await take('initial-runs'),'initial-runs');await page.waitForTimeout(100);
 await page.evaluate(()=>document.querySelector('#changing').append('New trailing text'));
 await page.waitForTimeout(200);rulesBatch=await take('new-run');assert(rulesBatch.segments.some(s=>s.text==='New trailing text'));
 await apply(rulesBatch,'new-run');await page.waitForTimeout(100);
 await page.evaluate(()=>document.querySelector('#changing').setAttribute('role','button'));await page.waitForTimeout(200);
 assert.equal(await evaluate('[...document.querySelectorAll("span")].filter(n=>testRoots.has(n)).length'),0);
 assert.equal((await take('now-action')).segments.length,0);
 ok('new runs in an observed container are discovered; newly excluded content is cleaned');
 await init('<article>Visible introduction<p style="height:2000px">Tall nested paragraph</p>OFFSCREEN_DIRECT_TAIL</article>');
 await start();await page.waitForTimeout(250);rulesBatch=await take('run-viewport');
 assert(rulesBatch.segments.some(s=>s.text==='Visible introduction'));
 assert(rulesBatch.segments.every(s=>!s.text.includes('OFFSCREEN_DIRECT_TAIL')));
 ok('mixed-container direct text uses its own range geometry, not the whole article viewport');
 await init('<style>h1{font:700 30px/1.4 serif;color:light-dark(#161616,#ededed);color-scheme:light dark}</style><h1>Styled heading</h1>');
 await start({style:'quote'});await page.waitForTimeout(200);rulesBatch=await take('heading');await apply(rulesBatch,'heading');await page.waitForTimeout(100);
 assert.equal(await evaluate('getComputedStyle(document.querySelector("h1").nextElementSibling).fontSize'),'30px');
 await page.emulateMedia({colorScheme:'dark'});await page.waitForTimeout(150);
 assert.equal(await evaluate('getComputedStyle(document.querySelector("h1").nextElementSibling).color'),await evaluate('getComputedStyle(document.querySelector("h1")).color'));
 await page.emulateMedia({colorScheme:'light'});
 ok('heading font is retained and translated color follows system theme changes');
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
 await init('<header><div>Account controls</div></header><table><tr><td><span class="titleline"><a href="/story">A news title</a></span></td></tr></table><div class="comment">A news comment</div><p>Unrelated sidebar</p>','https://news.ycombinator.com/translation-test');
 await start();await page.waitForTimeout(250);b=await take('site');
 assert.deepEqual(b.segments.map(s=>s.text),['A news title','A news comment']);
 ok('local site rules include inline titles and comments, excluding unrelated interface content');
 await init('<p>Fallback article when known selectors are absent</p>');await start();await page.waitForTimeout(250);b=await take('fallback');
 assert.deepEqual(b.segments.map(s=>s.text),['Fallback article when known selectors are absent']);
 ok('missing site selectors fall back to generic paragraph discovery');
 console.log(JSON.stringify({passed,browser:browser.version(),scope:'host isolated-world executor; native IPC not tested'}));
} finally {await browser.close();}
