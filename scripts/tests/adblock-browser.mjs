// Copyright 2026. SPDX-License-Identifier: MPL-2.0
// Host-browser tests of exact shipped Rust payloads and Brave/Lumie JS.
// This does NOT replace native Chromium154/OHOS network-loader acceptance.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import { spawnSync } from 'node:child_process';
import { pathToFileURL } from 'node:url';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE));
const root = path.resolve(import.meta.dirname,'../..');
const sourceRoot=path.join(root,'overlay/chrome/renderer/ohos/adblock_resources');
const runtime='(()=>{'+fs.readFileSync(path.join(sourceRoot,'procedural_filters.js'),'utf8')+'\n'+fs.readFileSync(path.join(sourceRoot,'controller.js'),'utf8')+'\n})()';
const rules=`localhost##+js(set-constant, lumieProbe, true)
localhost##.static-ad
localhost##.text:has-text(Sponsored)
localhost##.min:min-text-length(5)
localhost##.attr:matches-attr(data-kind=advert)
localhost##.color:matches-css(color: rgb(255, 0, 0))
localhost##.before:matches-css-before(content: "ad")
localhost##.after:matches-css-after(content: "ad")
localhost##.path:matches-path(/probe)
localhost##.child:upward(1)
localhost##:xpath(//div[@id="xpath"])
localhost##.remove:remove()
localhost##.removeattr:remove-attr(data-ad)
localhost##.removeclass:remove-class(ad)
localhost##.styled:style(opacity: 0.25)
`;
const server=http.createServer((req,res)=> {
 res.setHeader('Content-Type','text/html');
 res.setHeader('Content-Security-Policy', "default-src 'self'; script-src 'nonce-probe'; style-src 'unsafe-inline'; require-trusted-types-for 'script'");
 res.end(`<!doctype html><html><head><script nonce="probe">window.firstScriptSaw=window.lumieProbe===true;</script><script>window.cspWasBypassed=true;</script><style>.before::before,.after::after{content:'ad'}.color{color:rgb(255,0,0)}</style></head><body>
 <div class="static-ad">static</div><div class="text">Sponsored</div><div class="text" id="allowed">Article</div>
 <div class="min">12345</div><div class="min" id="short">1234</div>
 <div class="attr" data-kind="advert">ad</div><div class="color">ad</div>
 <div class="before">ad</div><div class="after">ad</div><div class="path">ad</div>
 <section id="parent"><span class="child">ad</span></section><div id="xpath">ad</div>
 <div class="remove">ad</div><div class="removeattr" data-ad="x">keep</div>
 <div class="removeclass ad">keep</div><div class="styled">style</div>
 </body></html>`);
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const url=`http://localhost:${server.address().port}/probe`;
const input={config:{sources:[{id:'test',text:rules,strict:true}]},queries:[{op:'cosmetic',url}]};
const probe=spawnSync(process.env.ADBLOCK_PROBE,[],{input:JSON.stringify(input),encoding:'utf8'});
assert.equal(probe.status,0,probe.stderr);
const payload=JSON.parse(probe.stdout).results[0];
assert.equal(payload.resources.procedural_actions.length,13);
const browser=await chromium.launch({executablePath:process.env.CHROME_PATH,headless:true});
let tests=0;const pass=name=>{ tests++; console.log('PASS '+name); };
try {
 const context=await browser.newContext();const page=await context.newPage();const cdp=await context.newCDPSession(page);
 await cdp.send('Page.enable');await cdp.send('Runtime.enable');await cdp.send('DOM.enable');await cdp.send('CSS.enable');
 await cdp.send('Page.addScriptToEvaluateOnNewDocument',{source:'(()=>{const scriptletGlobals=new Map();const deAmpEnabled=false;'+payload.resources.injected_script+'})()'});
 await page.goto(url);
 assert.equal(await page.evaluate(()=>window.firstScriptSaw),true);pass('scriptlet affects first inline script in main world');
 assert.equal(await page.evaluate(()=>window.cspWasBypassed),undefined);pass('site CSP remains enforced');
 assert.equal(await page.evaluate(()=>{try{document.createElement('script').textContent='1';return false;}catch{return true;}}),true);pass('Trusted Types remains enforced');
 const frameId=(await cdp.send('Page.getFrameTree')).frameTree.frame.id;
 const isolated=(await cdp.send('Page.createIsolatedWorld',{frameId,worldName:'lumie-adblock-test'})).executionContextId;
 await cdp.send('Runtime.evaluate',{contextId:isolated,expression:runtime});
 const result=await cdp.send('Runtime.evaluate',{contextId:isolated,expression:'globalThis.__lumieAdblock.set('+JSON.stringify(payload)+',"fixture")'});
 assert.equal(result.exceptionDetails,undefined);
 const sheet=(await cdp.send('CSS.createStyleSheet',{frameId})).styleSheetId;
 await cdp.send('CSS.setStyleSheetText',{styleSheetId:sheet,text:payload.css+result.result.value});
 for(const selector of ['.static-ad','.text:not(#allowed)','.min:not(#short)','.attr','.color','.before','.after','.path','#parent','#xpath']) {
  await page.waitForFunction(sel=>getComputedStyle(document.querySelector(sel)).display==='none',selector);
  pass('cosmetic/operator '+selector);
 }
 assert.equal(await page.locator('#allowed').isVisible(),true);assert.equal(await page.locator('#short').isVisible(),true);pass('negative selectors remain visible');
 await page.waitForFunction(()=>!document.querySelector('.remove'));
 assert.equal(await page.locator('.removeattr').getAttribute('data-ad'),null);
 assert.equal(await page.locator('.removeclass').evaluate(e=>e.classList.contains('ad')),false);
 assert.equal(await page.locator('.styled').evaluate(e=>getComputedStyle(e).opacity),'0.25');pass('remove / remove-attr / remove-class / style actions');
 await page.evaluate(()=>{const e=document.createElement('div');e.className='text';e.id='dynamic';e.textContent='Sponsored';document.body.append(e)});
 await page.waitForFunction(()=>getComputedStyle(document.querySelector('#dynamic')).display==='none');pass('dynamic DOM');
 await page.locator('#dynamic').evaluate(e=>e.textContent='Article');
 await page.waitForFunction(()=>getComputedStyle(document.querySelector('#dynamic')).display!=='none');pass('changed element is unhidden');
 const timings=await page.evaluate(async()=>{const times=[];for(let i=0;i<20;i++){const t=performance.now();const e=document.createElement('div');e.className='text';e.textContent='Article';document.body.append(e);await new Promise(r=>setTimeout(r,0));times.push(performance.now()-t)}return times});
 console.log('mutation_timer_ms '+JSON.stringify(timings));
 await cdp.send('Runtime.evaluate',{contextId:isolated,expression:'globalThis.__lumieAdblock.stop()'});
 await cdp.send('CSS.setStyleSheetText',{styleSheetId:sheet,text:''});
 assert.equal(await page.locator('.static-ad').isVisible(),true);assert.equal(await page.locator('.color').isVisible(),true);pass('disable removes styles and observers');
 await page.evaluate(()=>{const e=document.createElement('div');e.className='remove';e.id='after-disable';document.body.append(e)});
 await page.waitForTimeout(150);assert.equal(await page.locator('#after-disable').count(),1);pass('disabled executor does not remove new DOM');
 const control=await context.newPage();await control.goto(url);
 assert.equal(await control.evaluate(()=>window.firstScriptSaw),false);pass('no-injection control');
 console.log(JSON.stringify({passed:tests,chrome:browser.version(),compileMs:JSON.parse(probe.stdout).compileMs}));
} finally { await browser.close();server.close(); }
