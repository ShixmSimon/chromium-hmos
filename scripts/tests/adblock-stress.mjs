// Copyright 2026. SPDX-License-Identifier: MPL-2.0
// Host executor bounds/cancellation tests; not native phone performance evidence.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {spawnSync} from 'node:child_process';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
const {chromium}=await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE));
const root=path.resolve(import.meta.dirname,'../../overlay/chrome/renderer/ohos/adblock_resources')+'/';
const runtime='(()=>{'+fs.readFileSync(root+'procedural_filters.js','utf8')+'\n'+fs.readFileSync(root+'controller.js','utf8')+'\n})()';
const input={config:{sources:[{id:'stress',strict:true,text:'stress.test##.many:has-text(Sponsored)'}]},queries:[{op:'cosmetic',url:'https://stress.test/'}]};
const r=spawnSync(process.env.ADBLOCK_PROBE,[],{input:JSON.stringify(input),encoding:'utf8'});assert.equal(r.status,0,r.stderr);const payload=JSON.parse(r.stdout).results[0];
const b=await chromium.launch({executablePath:process.env.CHROME_PATH,headless:true});
try{
 const p=await b.newPage();await p.setContent('<!doctype html><body>');
 await p.evaluate(()=>{const f=document.createDocumentFragment();for(let i=0;i<25000;i++){const e=document.createElement('div');e.className='many';e.textContent='Sponsored';f.append(e)}document.body.append(f)});
 await p.evaluate(runtime);await p.evaluate(payload=>globalThis.__lumieAdblock.set(payload,'stress'),payload);
 await p.waitForFunction(()=>document.querySelectorAll('[data-lumie-stress-0]').length===20000,{},{timeout:20000});
 await p.evaluate(()=>globalThis.__lumieAdblock.stop());
 assert.equal(await p.locator('[data-lumie-stress-0]').count(),0);
 console.log('PASS 25000 matches capped at 20000; stop removes all retained marks');
 await p.evaluate(runtime);
 const partial=await p.evaluate(async payload=>{
   const old=Element.prototype.setAttribute;let applied=0;let atStop=0;
   Element.prototype.setAttribute=function(name,value){old.call(this,name,value);if(name==='data-lumie-batch-0'&&++applied===100){setTimeout(()=>{atStop=document.querySelectorAll('[data-lumie-batch-0]').length;globalThis.__lumieAdblock.stop()},0)}};
   globalThis.__lumieAdblock.set(payload,'batch');
   await new Promise(r=>setTimeout(r,500));Element.prototype.setAttribute=old;
   return {atStop,left:document.querySelectorAll('[data-lumie-batch-0]').length,applied};
 },payload);
 assert.ok(partial.atStop>0&&partial.atStop<20000);assert.equal(partial.left,0);console.log('PASS cancellation during partially applied batch '+JSON.stringify(partial));
 console.log(JSON.stringify({chrome:b.version(),elements:25000,passed:2}));
}finally{await b.close()}
