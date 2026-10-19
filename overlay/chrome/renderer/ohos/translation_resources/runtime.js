// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
// Executed only by the native translation service in its dedicated isolated
// world. There is no DOM event/message listener that can invoke this API.
(() => {
  if (globalThis.__lumieTranslation) return;
  const LIMIT = 1048576; // cumulative retained source + result UTF-16 units
  const MAX = 5000, LEASE = 45000;
  const exclude = 'script,style,noscript,input,textarea,select,option,button,form,pre,code,iframe,object,embed,[contenteditable]:not([contenteditable="false"]),[translate="no"],.notranslate,[hidden],[aria-hidden="true"]';
  const block = 'p,h1,h2,h3,h4,h5,h6,li,dt,dd,blockquote,div,section,article';
  let s = null;
  const now = () => performance.now();
  function paused() { return !s || s.paused || s.error || !s.show || document.hidden || document.prerendering || s.frozen; }
  function stats() {
    if (!s) return {state:'stopped',hasMore:false};
    let inFlight=0, translated=0, failed=0, ready=0;
    for (const a of s.anchors.values()) for (const p of a.parts) {
      inFlight += p.batch ? 1 : 0; translated += p.done ? 1 : 0;
      failed += p.failed ? 1 : 0;
      ready += (!p.batch && !p.done && !p.failed && a.priority && now() >= p.retryAt) ? 1 : 0;
    }
    return {state:s.error?'error':paused()?'paused':inFlight?'translating':'waiting',
      discovered:s.created,inFlight,translated,failed,hasMore:!paused() && ready>0,
      limitReached:s.limit,retainedTextUnits:s.units,style:s.style,showTranslation:s.show};
  }
  function clean(a) { if(a.host) { s.own.add(a.host); a.host.remove(); a.host=null; a.content=null; } }
  function validElement(el) {
    return el.isConnected && !s.own.has(el) && !el.closest(exclude) && !el.isContentEditable;
  }
  function priority(el) {
    if(!validElement(el) || !s.byNode.get(el)?.intersects) return '';
    const cs=getComputedStyle(el);
    if(cs.visibility!=='visible' || cs.display==='none' || cs.contentVisibility==='hidden') return '';
    const r=el.getBoundingClientRect(), v=window.visualViewport;
    const top=v?v.offsetTop:0, left=v?v.offsetLeft:0, height=v?v.height:innerHeight, width=v?v.width:innerWidth;
    if(!r.width || !r.height || r.right<=left || r.left>=left+width || r.bottom<=top || r.top>=top+height*(1+s.prefetch)) return '';
    return r.top<top+height?'visible':'prefetch';
  }
  function text(el) {
    // Read text nodes only; never form values, markup, attributes or shadow DOM.
    let out='', visited=0;
    const w=document.createTreeWalker(el,NodeFilter.SHOW_ELEMENT|NodeFilter.SHOW_TEXT,{
      acceptNode(n) {
        if(n.nodeType===1) {
          if(s.own.has(n) || n.matches(exclude) || n.isContentEditable) return NodeFilter.FILTER_REJECT;
          const cs=getComputedStyle(n);
          if(cs.display==='none'||cs.visibility!=='visible'||cs.contentVisibility==='hidden') return NodeFilter.FILTER_REJECT;
        }
        return NodeFilter.FILTER_ACCEPT;
      }
    });
    for(let n=w.nextNode();n;n=w.nextNode()) {
      if(++visited>10000) {s.limit=true;return null;}
      if(n.nodeType===3) out+=n.data;
      else if(n.tagName==='BR') out+='\n';
      if(out.length>LIMIT) {s.limit=true;return null;}
    }
    return out.replace(/[\t\r\n ]+/g,' ').trim();
  }
  function invalidate(a) {
    for(const p of a.parts) if(p.batch) s.invalid.add(p.batch);
    a.revision++; a.parts=[]; a.source=''; clean(a);
  }
  function refresh(a) {
    a.priority=priority(a.el);
    if(!a.priority) return;
    if(a.el.querySelector(block)){invalidate(a);a.priority='';return;}
    const source=text(a.el);
    if(source===null || source===a.source) return;
    invalidate(a);
    if(!source || s.limit) return;
    const parts=[];
    for(let offset=0;offset<source.length;) {
      let end=Math.min(offset+2000,source.length);
      if(end<source.length && /[\uD800-\uDBFF]/.test(source[end-1])) end--;
      parts.push(source.slice(offset,end)); offset=end;
    }
    if(s.created+parts.length>MAX || s.units+source.length>LIMIT) {s.limit=true;return;}
    s.units+=source.length; s.created+=parts.length; a.source=source;
    a.parts=parts.map((value,i)=>({id:`${a.id}:${a.revision}:${i}`,text:value,revision:a.revision,done:false,failed:false,batch:'',tries:0,retryAt:0,result:''}));
  }
  function schedule() {
    if(!s || s.timer || paused()) return;
    s.timer=setTimeout(work,0);
  }
  function discover(root) {
    if(!s || s.limit || !root || root.nodeType!==1 || s.own.has(root)) return;
    if(s.roots.length>=128) {s.limit=true;return;}
    s.roots.push(root); schedule();
  }
  function candidate(el) {
    if(s.limit)return;
    if(!el.matches(block)||!validElement(el)) return;
    // Only leaf paragraph blocks: avoids uploading parent/child copies.
    if(el.querySelector(block)) return;
    if(s.byNode.has(el)) {s.dirty.add(s.byNode.get(el));return;}
    if(s.limit || s.anchors.size>=MAX) {s.limit=true;return;}
    const a={id:String(++s.next),el,revision:0,source:'',parts:[],priority:'',intersects:false,host:null};
    s.byNode.set(el,a);s.anchors.set(a.id,a);s.io.observe(el);
  }
  function work() {
    if(!s) return;
    s.timer=0;if(paused())return;
    if(s.limit){s.roots=[];s.walker=null;}
    const start=now();let ops=0;
    while(now()-start<4 && ++ops<=64) {
      if(s.paint.size) {
        const a=s.paint.values().next().value;s.paint.delete(a); paint(a);continue;
      }
      if(s.dirty.size && now()-s.scrolled>=180) {
        const a=s.dirty.values().next().value;s.dirty.delete(a);
        if(a.el.isConnected) refresh(a); else {invalidate(a);s.io.unobserve(a.el);s.anchors.delete(a.id);s.visible.delete(a);}
        continue;
      }
      if(!s.walker) {
        const root=s.roots.shift();if(!root)break;
        candidate(root);s.walker=document.createTreeWalker(root,NodeFilter.SHOW_ELEMENT,{
          acceptNode:n=>(s.own.has(n)||n.matches(exclude))?NodeFilter.FILTER_REJECT:NodeFilter.FILTER_ACCEPT
        });
      }
      const el=s.walker.nextNode();if(el)candidate(el);else s.walker=null;
    }
    if(s.paint.size||s.roots.length||s.walker||(s.dirty.size && now()-s.scrolled>=180))schedule();
  }
  function paint(a) {
    if(!a.el.isConnected || !a.parts.length || !a.parts.every(p=>p.done))return;
    // Validate again at insertion; a virtual-list reuse cannot receive old text.
    if(!validElement(a.el)||text(a.el)!==a.source) {invalidate(a);s.dirty.add(a);return;}
    clean(a);
    const host=document.createElement('span');s.own.add(host);
    host.style.setProperty('display',s.show?'block':'none','important');
    host.style.setProperty('font','inherit','important');
    const shadow=host.attachShadow({mode:'closed'}), style=new CSSStyleSheet(), content=document.createElement('span');
    style.replaceSync(':host{color-scheme:light dark}span{display:block;font:inherit;line-height:1.6;white-space:pre-wrap;overflow-wrap:anywhere;margin-block:.35em}span.quote{border-inline-start:3px solid #888;padding-inline-start:.75em}span.gray{background:light-dark(#eee,#303030);color:light-dark(#171717,#eee);padding:.5em .7em;border-radius:.3em}');
    content.className=s.style;content.lang=s.target;content.dir='auto';
    content.textContent=a.parts.map(p=>p.result).join('');
    shadow.adoptedStyleSheets=[style];shadow.append(content);
    // Native references only: never recover an anchor from a page DOM id.
    a.content=content;a.host=host;a.el.after(host);
  }
  function presentation(style,show) {
    s.style=style;s.show=show;
    for(const a of s.anchors.values()) if(a.host) {
      a.content.className=style;a.host.style.setProperty('display',show?'block':'none','important');
    }
    if(show)schedule();return stats();
  }
  function settle(id,retry) {
    const b=s.batches.get(id);if(!b)return false;
    for(const {a,p} of b.items) if(p.batch===id) {
      p.batch='';if(!p.done) {p.failed=!retry||p.tries>=3;p.retryAt=now()+Math.min(30000,1000*2**p.tries);s.dirty.add(a);}
    }
    s.batches.delete(id);return true;
  }
  function tick() {
    const expired=[];
    for(const [id,b] of s.batches) if(now()>=b.until) {settle(id,true);expired.push(id);}
    const invalidated=[...s.invalid];s.invalid.clear();
    return {...stats(),expired,invalidated};
  }
  function stop() {
    if(!s)return;
    clearTimeout(s.timer);clearTimeout(s.scrollTimer);s.io.disconnect();s.mo.disconnect();
    for(const [target,type,fn,capture] of s.listeners)target.removeEventListener(type,fn,capture);
    for(const a of s.anchors.values())clean(a);
    s=null;
  }
  function start(c) {
    stop();
    if(!document.documentElement)return {ok:false,reason:'notReady'};
    s={session:c.sessionId,target:c.targetLanguage,style:c.style,prefetch:c.prefetchViewportRatio,show:true,paused:false,error:false,frozen:false,
      units:0,created:0,next:0,limit:false,timer:0,scrollTimer:0,scrolled:-Infinity,anchors:new Map(),byNode:new WeakMap(),own:new WeakSet(),
      roots:[],walker:null,visible:new Set(),dirty:new Set(),paint:new Set(),batches:new Map(),invalid:new Set(),listeners:[]};
    const listen=(target,type,fn)=>{const capture=type==='scroll';target.addEventListener(type,fn,{passive:true,capture});s.listeners.push([target,type,fn,capture]);};
    const observeVisibility=()=>{
      if(s.io)s.io.disconnect();
      s.visible.clear();
      s.io=new IntersectionObserver(entries=>{if(!s)return;for(const e of entries){const a=s.byNode.get(e.target);if(a){a.intersects=e.isIntersecting;if(e.isIntersecting){s.visible.add(a);s.dirty.add(a);}else{s.visible.delete(a);a.priority='';if(!a.el.isConnected)s.dirty.add(a);}}}schedule();},{rootMargin:`0px 0px ${Math.round(innerHeight*s.prefetch)}px 0px`});
      for(const a of s.anchors.values()){a.intersects=false;s.io.observe(a.el);}
    };
    observeVisibility();
    s.mo=new MutationObserver(records=>{
      if(!s)return;
      for(const r of records) {
        const el=r.target.nodeType===1?r.target:r.target.parentElement;
        if(!el||s.own.has(el))continue;
        const a=s.byNode.get(el)||s.byNode.get(el.closest(block));
        if(a && !(r.type==='childList' && [...r.addedNodes,...r.removedNodes].every(n=>s.own.has(n)))) {invalidate(a);s.dirty.add(a);}
        let added=0;
        for(const n of r.addedNodes) {if(++added>128){s.limit=true;break;}if(n.nodeType===1)discover(n);}
        if(r.type==='attributes')discover(el);
      }
      schedule();
    });
    s.mo.observe(document.documentElement,{childList:true,subtree:true,characterData:true,attributes:true,attributeFilter:['class','style','hidden','translate','contenteditable','aria-hidden']});
    listen(window,'scroll',()=>{s.scrolled=now();clearTimeout(s.scrollTimer);s.scrollTimer=setTimeout(()=>{for(const a of s.visible)s.dirty.add(a);schedule();},180);});
    listen(window,'resize',()=>{observeVisibility();schedule();});
    listen(document,'visibilitychange',schedule);
    listen(window,'pagehide',()=>{s.frozen=true;});
    listen(window,'pageshow',()=>{s.frozen=false;schedule();});
    discover(document.body||document.documentElement);return stats();
  }
  function call(c) {
    if(c.op==='start')return start(c);
    if(!s && c.op==='stop')return {ok:true,state:'stopped'};
    if(!s||s.session!==c.sessionId)return {ok:false,reason:'sessionEnded'};
    if(c.op==='stop') {stop();return {ok:true,state:'stopped'};}
    if(c.op==='status'){s.paused=!!c.paused;if(!paused())schedule();return {ok:true,...tick()};}
    if(c.op==='presentation')return {ok:true,...presentation(c.style,c.showTranslation)};
    if(c.op==='release'){if(c.reason==='authentication'||c.reason==='quota')s.error=true;return {ok:settle(c.batchId,c.retryable&&!s.error),...stats()};}
    if(c.op==='take') {
      tick();const segments=[],items=[];let units=0;
      if(paused()||now()-s.scrolled<180||s.batches.size>=2)return {ok:true,segments,...stats()};
      const ordered=[...s.visible].filter(a=>a.priority).sort((a,b)=>a.priority===b.priority?(a.el.compareDocumentPosition(b.el)&Node.DOCUMENT_POSITION_FOLLOWING?-1:1):a.priority==='visible'?-1:1);
      const started=now();
      for(const a of ordered) {
        if(now()-started>=4)break;
        // Visibility and source may have changed since observer delivery.
        refresh(a);if(!a.priority)continue;
        for(const p of a.parts) {
          if(p.done||p.failed||p.batch||now()<p.retryAt)continue;
          if(segments.length>=c.maxSegments||units+p.text.length>c.maxTextUnits)continue;
          p.batch=c.batchId;p.tries++;units+=p.text.length;items.push({a,p});
          segments.push({segmentId:p.id,segmentRevision:p.revision,text:p.text,priority:a.priority});
        }
      }
      if(items.length)s.batches.set(c.batchId,{items,until:now()+LEASE});
      return {ok:true,segments,...stats()};
    }
    if(c.op==='apply') {
      const b=s.batches.get(c.batchId);
      if(!b||now()>=b.until){settle(c.batchId,true);return {ok:false,reason:'batchExpired'};}
      const results=[];let applied=0;
      const values=new Map(c.segments.map(x=>[x.segmentId,x]));
      for(const {a,p} of b.items) {
        const value=values.get(p.id);let reason='';
        if(!a.el.isConnected||a.revision!==p.revision||!validElement(a.el)||text(a.el)!==a.source) reason='staleSegment';
        else if(!value||value.segmentRevision!==p.revision||typeof value.translatedText!=='string'||!value.translatedText.trim()) reason='invalidResult';
        else if(value.translatedText.length>8000||s.units+value.translatedText.length>LIMIT){reason='tooLarge';s.limit=true;}
        if(!reason){p.result=value.translatedText;s.units+=p.result.length;p.done=true;p.batch='';applied++;s.paint.add(a);}
        else {p.failed=true;p.batch='';}
        results.push({segmentId:p.id,segmentRevision:p.revision,ok:!reason,reason});
      }
      s.batches.delete(c.batchId);schedule();
      return {ok:true,applied,rejected:results.length-applied,results,...stats()};
    }
    return {ok:false,reason:'invalidCommand'};
  }
  Object.defineProperty(globalThis,'__lumieTranslation',{value:{call},configurable:false,writable:false});
})();
