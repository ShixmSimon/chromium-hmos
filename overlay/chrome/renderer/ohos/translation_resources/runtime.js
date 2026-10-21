// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
// Executed only by the native translation service in its dedicated isolated
// world. There is no DOM event/message listener that can invoke this API.
(() => {
  if (globalThis.__lumieTranslation) return;
  const LIMIT = 1048576; // cumulative retained source + result UTF-16 units
  const MAX = 5000, LEASE = 45000;
  const protectedInline='code,kbd,samp,var,math';
  const tokenPattern=/\[\[LUMIE_KEEP_\d+_\d+\]\]/g;
  // Deliberately closed Chinese alphabet: unknown characters mean "translate",
  // not a guess at simplified/traditional conversion. Ambiguous conversions
  // (e.g. 后/後, 发/發/髮) are intentionally absent. No downloaded model/data.
  const chinesePairs='这這 个個 经經 译譯 为為 语語 说說 们們 现現 页頁 关關 闭閉 时時 间間 设設 备備 进進 开開 过過 还還 会會 应應 该該 显顯 示示 载載 动動 态態 处處 请請 选選 择擇 传傳 统統 简簡 体體 读讀 书書 写寫 学學 习習 试試 验驗 证證 复復 杂雜 网網 络絡 资資 源源 视視 频頻 软軟 件件 观觀 点點 长長 短短 实實 际際 机機 务務 数數 据據 对對 话話 结結 果果 从從 里裡 边邊 与與 无無 线線 连連 接接 义義 变變 让讓 带帶 认認 识識 给給 吗嗎 呢呢 吧吧'.split(' ').filter(p=>[...p].length===2&&p[0]!==p[1]);
  const chineseCommon=new Set([...'是一已中文的段落不需要再次翻本有在和及或也就都很更最可以用到上下面前容字文章保存取消完成正在等待支持入出成功失注意安全信息你好世界使用正常方法如果只能有新同第一二三四五六七八九十百千年月日天人大小多少今明昨因此但是而且所以所有每任何示件短果接呢吧']);
  const chineseSimplified=new Set(chinesePairs.map(p=>p[0])),chineseTraditional=new Set(chinesePairs.map(p=>p[1]));
  const scriptRules={
    ko:/^\p{Script=Hangul}+$/u,el:/^\p{Script=Greek}+$/u,
    hy:/^\p{Script=Armenian}+$/u,ka:/^\p{Script=Georgian}+$/u,
    th:/^\p{Script=Thai}+$/u,lo:/^\p{Script=Lao}+$/u,
    km:/^\p{Script=Khmer}+$/u
  };
  const mathTags=new Set('math mrow mi mn mo mtext ms mspace msup msub msubsup mfrac msqrt mroot mtable mtr mtd munder mover munderover semantics'.split(' '));
  const exclude = 'script,style,noscript,input,textarea,select,option,button,form,pre,code,kbd,samp,var,math,svg,canvas,iframe,object,embed,nav,footer,[role="navigation"],[role="toolbar"],[role="menu"],[role="menubar"],[role="button"],[role="tab"],time,[contenteditable]:not([contenteditable="false"]),[translate="no"],.notranslate,[hidden],[aria-hidden="true"]';
  const block = 'p,h1,h2,h3,h4,h5,h6,li,dt,dd,blockquote,div,section,article,main,body,td,th,caption,figcaption,summary,ul,ol,dl,table,thead,tbody,tfoot,tr';
  // Small, local allowlists; never download executable site rules. Fall back to
  // generic discovery when a site's known content selectors no longer match.
  const siteRules = [
    {hosts:['github.com'],selectors:'.markdown-body,.markdown-title,.js-issue-title'},
    {hosts:['news.ycombinator.com'],selectors:'.titleline,.comment,.toptext'},
    {hosts:['x.com','twitter.com','mobile.twitter.com'],selectors:'[data-testid="tweetText"],[data-testid="UserDescription"]'},
    {hosts:['www.youtube.com'],selectors:'#content-text,#description-inline-expander'}
  ];
  let s = null;
  const now = () => performance.now();
  const loadingLabels={zh:'正在翻译...',en:'Translating...',ja:'翻訳中...',ko:'번역 중...',fr:'Traduction en cours...',de:'Wird übersetzt...',es:'Traduciendo...',pt:'Traduzindo...',ru:'Перевод...',ar:'جارٍ الترجمة...'};
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
  function releaseLayout(a) {
    for(const el of a.layouts||[]) {
      const entry=s.layouts.get(el);if(!entry)continue;
      entry.owners.delete(a);
      if(entry.owners.size){updateLayout(el,entry);continue;}
      for(const [name,p] of entry.properties) {
        // Never overwrite a page's subsequent change to an owned property.
        if(el.style.getPropertyValue(name)!==p.applied||el.style.getPropertyPriority(name)!=='important')continue;
        if(p.value)el.style.setProperty(name,p.value,p.priority);else el.style.removeProperty(name);
      }
      s.layoutWrites.set(el,el.getAttribute('style'));s.layouts.delete(el);
    }
    a.layouts=new Set();
  }
  function clean(a,release=true) {
    if(a.host) { s.own.add(a.host); a.host.remove(); a.host=null; a.content=null; }
    if(release)releaseLayout(a);
  }
  function setLayoutProperty(el,entry,name,value) {
    if(!entry.properties.has(name))entry.properties.set(name,{value:el.style.getPropertyValue(name),priority:el.style.getPropertyPriority(name),applied:value});
    el.style.setProperty(name,value,'important');entry.properties.get(name).applied=el.style.getPropertyValue(name);
  }
  function updateLayout(el,entry) {
    if(entry.height===null){s.layoutWrites.set(el,el.getAttribute('style'));return;}
    const owned=entry.properties.get('height');
    if(owned&&(el.style.getPropertyValue('height')!==owned.applied||el.style.getPropertyPriority('height')!=='important'))return;
    const extra=[...entry.owners.values()].reduce((sum,value)=>sum+value,0);
    setLayoutProperty(el,entry,'height',`${entry.height+extra}px`);
    if(Number.isFinite(entry.maxHeight))setLayoutProperty(el,entry,'max-height',`${Math.max(entry.maxHeight,entry.height)+extra}px`);
    s.layoutWrites.set(el,el.getAttribute('style'));
  }
  function relaxLayout(a) {
    const hostStyle=getComputedStyle(a.content);
    const parentStyle=getComputedStyle(a.host.parentElement);
    const added=a.host.getBoundingClientRect().height+(parseFloat(hostStyle.marginTop)||0)+(parseFloat(hostStyle.marginBottom)||0)+(parseFloat(parentStyle.rowGap)||0);
    for(let el=a.host.parentElement,depth=0;el&&depth++<64;el=el.parentElement) {
      // Do not uncap the document viewport or replace intentional scrollers.
      if(el===document.body||el===document.documentElement)break;
      const cs=getComputedStyle(el), overflow=[cs.overflowX,cs.overflowY];
      if(overflow.some(v=>v==='auto'||v==='scroll'))continue;
      const clipped=overflow.some(v=>v==='hidden'||v==='clip');
      const clamped=el===a.el&&cs.webkitLineClamp&&cs.webkitLineClamp!=='none';
      let entry=s.layouts.get(el);
      if(!entry&&(!clipped&&!clamped||el.scrollHeight<=el.clientHeight&&!clamped))continue;
      if(entry&&entry.height===null&&!clipped&&!clamped)continue;
      if(!entry){entry={owners:new Map(),properties:new Map(),height:parseFloat(cs.height)||el.clientHeight,maxHeight:parseFloat(cs.maxHeight)};s.layouts.set(el,entry);}
      if(entry.height===null){entry.height=parseFloat(cs.height)||el.clientHeight;entry.maxHeight=parseFloat(cs.maxHeight);}
      entry.owners.set(a,added);a.layouts.add(el);
      if(clamped)setLayoutProperty(el,entry,'-webkit-line-clamp','unset');
      // A clipped leaf's complete source was already selected for translation;
      // reserve room for it plus the result. Ancestor clips retain their source
      // viewport, so unrelated hidden paragraphs are not expanded wholesale.
      if(el===a.el&&!el.querySelector(block))entry.height=Math.max(entry.height,el.scrollHeight-added);
      updateLayout(el,entry);
    }
  }
  function validElement(el) {
    return el.isConnected && !s.own.has(el) && !el.closest(exclude) && !el.isContentEditable &&
      (!s.scope || !!el.closest(s.scope));
  }
  function priority(a) {
    const el=a.el;
    if(!validElement(el) || !s.intersections.get(el)) return '';
    const cs=getComputedStyle(el);
    if(cs.visibility!=='visible' || cs.display==='none' || cs.contentVisibility==='hidden') return '';
    let r=el.getBoundingClientRect();
    if(el.querySelector(block)) {
      if(a.nodes.some(n=>n.parentNode!==el))return '';
      const range=document.createRange();range.setStartBefore(a.nodes[0]);range.setEndAfter(a.nodes[a.nodes.length-1]);
      r=range.getBoundingClientRect();
    }
    const v=window.visualViewport;
    const top=v?v.offsetTop:0, left=v?v.offsetLeft:0, height=v?v.height:innerHeight, width=v?v.width:innerWidth;
    if(!r.width || !r.height || r.right<=left || r.left>=left+width || r.bottom<=top || r.top>=top+height*(1+s.prefetch)) return '';
    return r.top<top+height?'visible':'prefetch';
  }
  function protectedValue(root) {
    let visited=0,units=0;
    function copy(n,depth=0) {
      if(++visited>256||depth>32)throw Error('protectedLimit');
      if(n.nodeType===3){units+=n.data.length;if(units>8000)throw Error('protectedLimit');return n.data;}
      if(n.nodeType!==1||s.own.has(n)||n.isContentEditable)return null;
      if(n.matches('[hidden],[aria-hidden="true"],[translate="no"],.notranslate'))return null;
      if(n!==root&&n.matches(exclude)&&!n.matches(protectedInline))return null;
      const cs=getComputedStyle(n);if(cs.display==='none'||cs.visibility!=='visible')return null;
      if(root.localName==='math'&&!mathTags.has(n.localName))return null;
      if(n.tagName==='BR'){units++;return '\n';}
      const children=[];
      for(const child of n.childNodes){const value=copy(child,depth+1);if(value!==null)children.push(value);}
      // Only known MathML tags and text; no original attributes, links, event
      // handlers, styles or arbitrary page markup are copied into the shadow.
      return {tag:root.localName==='math'?n.localName:'',children};
    }
    const shape=copy(root);
    return {kind:root.localName==='math'?'math':'code',shape};
  }
  function read(a) {
    // Read text nodes only; never form values, markup, attributes or shadow DOM.
    let visited=0,units=0;const pieces=[],keeps=[];
    // A run consists of adjacent direct children, bounded by nested paragraph
    // blocks. Do not flatten text from opposite sides of a nested paragraph.
    let previous=null;
    for(const root of a.nodes) {
      if(root.parentNode!==a.el) return null;
      if(previous) {let n=previous.nextSibling;while(n&&s.own.has(n))n=n.nextSibling;if(n!==root)return null;}
      previous=root;
      const stack=[root];
      while(stack.length) {
        const n=stack.pop();
        if(++visited>10000) {s.limit=true;return null;}
        if(n.nodeType===3){pieces.push(n.data);units+=n.data.length;}
        else if(n.nodeType===1) {
          if(s.own.has(n)||n.isContentEditable)continue;
          const cs=getComputedStyle(n);
          if(cs.display==='none'||cs.visibility!=='visible'||cs.contentVisibility==='hidden')continue;
          if(n.matches(protectedInline)&&!n.matches('[hidden],[aria-hidden="true"],[translate="no"],.notranslate')) {
            if(keeps.length>=64){s.limit=true;return null;}
            let value;try{value=protectedValue(n);}catch(_){s.limit=true;return null;}
            keeps.push(value);pieces.push(value);units+=JSON.stringify(value).length;
          } else if(n.matches(exclude))continue;
          else if(n.tagName==='BR'){pieces.push(' ');units++;}
          else {
            if(stack.length+n.childNodes.length>10000){s.limit=true;return null;}
            for(let i=n.childNodes.length-1;i>=0;i--)stack.push(n.childNodes[i]);
          }
        }
        if(units>LIMIT) {s.limit=true;return null;}
      }
    }
    const ordinary=pieces.filter(p=>typeof p==='string').join('');
    let nonce=0;while(ordinary.includes(`[[LUMIE_KEEP_${nonce}_`))nonce++;
    keeps.forEach((p,i)=>{p.token=`[[LUMIE_KEEP_${nonce}_${i}]]`;});
    const source=pieces.map(p=>typeof p==='string'?p:p.token).join('').replace(/[\t\r\n ]+/g,' ').trim();
    const keepKey=JSON.stringify(keeps);
    return {source:keeps.length&&!ordinary.trim()?'':source,keeps,keepKey,units:keeps.length?keepKey.length:0};
  }
  function sameSource(a) {
    const value=read(a);return value!==null&&value.source===a.source&&value.keepKey===a.keepKey;
  }
  function alreadyTarget(source,keeps) {
    // Bound the synchronous heuristic independently of the document quota.
    // Long runs remain translatable rather than scanning/allocating unboundedly.
    if(source.length>8000)return false;
    // Only remove markers we generated. Literal marker-looking page text and
    // foreign prose remain evidence against skipping; code/formulas do not.
    let text=source;
    for(const keep of keeps)text=text.replace(keep.token,' ');
    // Unknown symbols and unprotected URLs/identifiers are deliberately not
    // stripped as "noise". A foreign word, however small, keeps the paragraph.
    if(/[^\p{L}\p{M}\p{N}\p{P}\p{Z}\s]/u.test(text))return false;
    const letters=(text.match(/[\p{L}\p{M}]/gu)||[]).join('');
    if([...letters].length<8)return false;
    const target=s.target.toLowerCase(),tags=target.split('-'),base=tags[0];
    const script=tags.slice(1).find(tag=>/^[a-z]{4}$/.test(tag));
    const defaultScripts={ja:'jpan',ko:'kore',el:'grek',hy:'armn',ka:'geor',th:'thai',lo:'laoo',km:'khmr'};
    if(script&&base!=='zh'&&script!==defaultScripts[base])return false;
    if(base==='zh') {
      // Generic zh does not request script conversion. Explicit variants must
      // contain only known invariant characters or their requested forms.
      if(!/^\p{Script=Han}+$/u.test(letters))return false;
      if(!/这是|這是|已经|已經|不需要|可以|因为|因為|我们|我們/.test(letters))return false;
      if(script&&script!=='hans'&&script!=='hant')return false;
      const traditional=tags.includes('hant')||tags.includes('tw')||tags.includes('hk')||tags.includes('mo');
      const simplified=tags.includes('hans')||tags.includes('cn')||tags.includes('sg');
      if(traditional&&simplified)return false;
      if(target==='zh')return true;
      if(!traditional&&!simplified)return false;
      const alphabet=traditional?chineseTraditional:chineseSimplified;
      return [...letters].every(c=>chineseCommon.has(c)||alphabet.has(c));
    }
    if(base==='ja') {
      // Kanji alone could be Chinese. Each sentence needs kana evidence; an
      // embedded Chinese-only sentence must not disappear with a Japanese one.
      if(!/^[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}ー\p{M}]+$/u.test(letters))return false;
      return text.split(/[。！？.!?\n]+/u).every(sentence=>{
        if(!/\p{L}/u.test(sentence))return true;
        return (sentence.match(/[\p{Script=Hiragana}\p{Script=Katakana}]/gu)||[]).length>=4;
      });
    }
    // Latin, Cyrillic and Arabic scripts are shared by many languages. Without
    // a local statistical detector they remain uncertain, even with lang=en.
    const rule=Object.hasOwn(scriptRules,base)?scriptRules[base]:null;
    return !!rule&&rule.test(letters);
  }
  function validTokens(source,result) {
    const before=source.match(tokenPattern)||[],after=result.match(tokenPattern)||[];
    return before.length===after.length&&before.every(t=>after.filter(v=>v===t).length===before.filter(v=>v===t).length);
  }
  function appendResult(content,a,result) {
    const values=new Map(a.keeps.map(p=>[p.token,p]));let offset=0;
    function materialize(value,math=false) {
      if(typeof value==='string')return document.createTextNode(value);
      const el=math?document.createElementNS('http://www.w3.org/1998/Math/MathML',value.tag):document.createElement('span');
      for(const child of value.children)el.append(materialize(child,math));return el;
    }
    for(const match of result.matchAll(tokenPattern)) {
      const value=values.get(match[0]);if(!value)continue;
      content.append(document.createTextNode(result.slice(offset,match.index)));
      const node=value.kind==='math'?materialize(value.shape,true):document.createElement('code');
      if(value.kind!=='math')node.append(materialize(value.shape));
      content.append(node);offset=match.index+match[0].length;
    }
    content.append(document.createTextNode(result.slice(offset)));
  }
  function invalidate(a) {
    for(const p of a.parts) if(p.batch) s.invalid.add(p.batch);
    a.revision++; a.parts=[]; a.source='';a.keeps=[];a.keepKey=''; clean(a);
  }
  function refresh(a) {
    if(!validElement(a.el)){invalidate(a);a.priority='';return;}
    a.priority=priority(a);
    if(!a.priority) return;
    const value=read(a);
    if(value===null) {invalidate(a);a.priority='';return;}
    const source=value.source;
    if(source===a.source&&value.keepKey===a.keepKey) {if(a.loading&&!a.host)s.paint.add(a);return;}
    invalidate(a);
    if(!source || s.limit) return;
    // Before splitting, loading UI, provider dispatch, or retained-text budget.
    // Do not retain skipped prose: recheck the visible run on future mutations.
    if(alreadyTarget(source,value.keeps))return;
    const parts=[];
    for(let offset=0;offset<source.length;) {
      let end=Math.min(offset+2000,source.length);
      const tokenStart=source.lastIndexOf('[[LUMIE_KEEP_',end-1);
      if(tokenStart>offset&&source.indexOf(']]',tokenStart)+2>end)end=tokenStart;
      if(end<source.length && /[\uD800-\uDBFF]/.test(source[end-1])) end--;
      parts.push(source.slice(offset,end)); offset=end;
    }
    if(s.created+parts.length>MAX || s.units+source.length+value.units>LIMIT) {s.limit=true;return;}
    s.units+=source.length+value.units; s.created+=parts.length; a.source=source;
    a.keeps=value.keeps;a.keepKey=value.keepKey;
    a.parts=parts.map((value,i)=>({id:`${a.id}:${a.revision}:${i}`,text:value,revision:a.revision,done:false,failed:false,batch:'',tries:0,retryAt:0,result:''}));
    s.paint.add(a);
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
    if(!validElement(el))return;
    const parentLayout=el.parentElement&&/flex|grid/.test(getComputedStyle(el.parentElement).display);
    if(!el.matches(block)&&!(s.scope&&el.matches(s.scope))&&!el.querySelector(block)&&!parentLayout)return;
    const ownLayout=/flex|grid/.test(getComputedStyle(el).display);
    const runs=[];let run=[],visited=0;
    const flush=()=>{if(run.some(n=>n.nodeType===1||n.nodeType===3&&n.data.trim()))runs.push(run);run=[];};
    for(const n of el.childNodes) {
      if(s.own.has(n))continue;
      if(++visited>10000){s.limit=true;return;}
      if(n.nodeType===1&&(n.matches(block)||n.querySelector(block)||ownLayout&&!n.matches(protectedInline)))flush();
      else if(n.nodeType===3||n.nodeType===1)run.push(n);
    }
    flush();
    const existing=s.byNode.get(el)||new Map(), keep=new Set();
    for(const nodes of runs) {
      const key=nodes[0];keep.add(key);let a=existing.get(key);
      if(a) {
        if(a.nodes.length!==nodes.length||nodes.some((n,i)=>n!==a.nodes[i]))invalidate(a);
        a.nodes=nodes;s.dirty.add(a);continue;
      }
      if(s.anchors.size>=MAX){s.limit=true;break;}
      a={id:String(++s.next),el,nodes,revision:0,source:'',keeps:[],keepKey:'',layouts:new Set(),parts:[],priority:'',host:null};
      existing.set(key,a);s.anchors.set(a.id,a);
      if(s.intersections.get(el)){s.visible.add(a);s.dirty.add(a);}
    }
    for(const [key,a] of existing)if(!keep.has(key)) {
      invalidate(a);s.anchors.delete(a.id);s.visible.delete(a);s.dirty.delete(a);existing.delete(key);
    }
    if(existing.size){s.byNode.set(el,existing);s.io.observe(el);}
    else {s.byNode.delete(el);s.io.unobserve(el);}
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
        if(a.el.isConnected) refresh(a); else {invalidate(a);s.io.unobserve(a.el);s.byNode.get(a.el)?.delete(a.nodes[0]);s.anchors.delete(a.id);s.visible.delete(a);}
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
    if(!a.el.isConnected || !a.parts.length)return;
    const loading=!a.parts.every(p=>p.done);
    // Do not leave an endless activity indicator after terminal failure or for
    // prefetched/offscreen paragraphs. Partial results remain private until complete.
    if(loading && (s.error || a.parts.some(p=>p.failed) || a.priority!=='visible')) {clean(a);return;}
    // Validate again at insertion; a virtual-list reuse cannot receive old text.
    if(!validElement(a.el)||!sameSource(a)) {invalidate(a);s.dirty.add(a);return;}
    clean(a,false);
    const host=document.createElement('span');s.own.add(host);
    host.style.setProperty('display',s.show?'block':'none','important');
    const originalStyle=getComputedStyle(a.el);
    host.style.setProperty('font',originalStyle.font||'inherit','important');
    host.style.setProperty('color',originalStyle.color,'important');
    host.style.setProperty('text-align',originalStyle.textAlign,'important');
    const ownLayout=/flex|grid/.test(originalStyle.display);
    if(ownLayout) {
      host.style.setProperty('min-width','0','important');host.style.setProperty('max-width','100%','important');
      if(/grid/.test(originalStyle.display))host.style.setProperty('grid-column','1 / -1','important');
      else if(originalStyle.flexDirection.startsWith('row')) {
        host.style.setProperty('flex','0 0 100%','important');
        let entry=s.layouts.get(a.el);
        if(!entry){entry={owners:new Map(),properties:new Map(),height:null,maxHeight:NaN};s.layouts.set(a.el,entry);}
        if(!entry.owners.has(a))entry.owners.set(a,0);a.layouts.add(a.el);
        setLayoutProperty(a.el,entry,'flex-wrap','wrap');s.layoutWrites.set(a.el,a.el.getAttribute('style'));
      }
    }
    const shadow=host.attachShadow({mode:'closed'}), style=new CSSStyleSheet(), content=document.createElement('span');
    style.replaceSync(`:host{color-scheme:light dark}
      span.translation{display:block;font:inherit;line-height:1.6;white-space:pre-wrap;overflow-wrap:anywhere;margin-block:.35em}
      span.quote{border-inline-start:4px solid currentColor;padding-inline-start:.75em}
      span.gray{background:light-dark(#eee,#303030);color:light-dark(#171717,#eee);padding:.5em .7em;border-radius:.3em}
      span.loading{font-size:.9em}
      @supports(background-clip:text){span.loading{background-image:linear-gradient(110deg,currentColor 25%,#008e96 43%,#83cbe9 50%,currentColor 65%);background-size:200% 100%;background-clip:text;-webkit-text-fill-color:transparent;animation:lumie-shimmer 1.8s linear infinite}}
      @keyframes lumie-shimmer{from{background-position:200% 0}to{background-position:-200% 0}}
      @media(prefers-reduced-motion:reduce){span.loading{animation:none}}`);
    a.loading=loading;content.className='translation '+s.style+(loading?' loading':'');content.lang=s.target;content.dir='auto';
    if(loading) {content.lang=s.uiLanguage;content.textContent=s.loadingLabel;content.setAttribute('role','status');content.setAttribute('aria-busy','true');}
    else appendResult(content,a,a.parts.map(p=>p.result).join(''));
    shadow.adoptedStyleSheets=[style];shadow.append(content);
    // Native references only: never recover an anchor from a page DOM id.
    a.content=content;a.host=host;
    // Preserve list/table semantics and keep mixed-container runs in order.
    // A regular leaf paragraph keeps the established sibling presentation.
    const parentDisplay=a.el.parentElement?getComputedStyle(a.el.parentElement).display:'';
    if(ownLayout)a.el.append(host);
    else if(a.el.matches('li,dt,dd,td,th,caption,figcaption,summary,body,main')||a.el.querySelector(block)||/flex|grid/.test(parentDisplay))
      a.nodes[a.nodes.length-1].after(host);
    else a.el.after(host);
    if(!loading)relaxLayout(a);
  }
  function presentation(style,show) {
    s.style=style;s.show=show;
    for(const a of s.anchors.values()) if(a.host) {
      a.content.className='translation '+style+(a.loading?' loading':'');a.host.style.setProperty('display',show?'block':'none','important');
      if(!show)releaseLayout(a);else s.paint.add(a);
    }
    if(show)schedule();return stats();
  }
  function settle(id,retry) {
    const b=s.batches.get(id);if(!b)return false;
    for(const {a,p} of b.items) if(p.batch===id) {
      p.batch='';if(!p.done) {p.failed=!retry||p.tries>=3;p.retryAt=now()+Math.min(30000,1000*2**p.tries);s.dirty.add(a);s.paint.add(a);}
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
    const language=(c.uiLanguage||'en').toLowerCase(), base=language.split('-')[0];
    const knownLanguage=Object.hasOwn(loadingLabels,base);
    const loadingLabel=language.startsWith('zh-hant')||language==='zh-tw'||language==='zh-hk'?'正在翻譯...':knownLanguage?loadingLabels[base]:loadingLabels.en;
    const rule=siteRules.find(r=>r.hosts.includes(location.hostname));
    const scope=rule&&document.querySelector(rule.selectors)?rule.selectors:'';
    s={session:c.sessionId,target:c.targetLanguage,uiLanguage:knownLanguage?language:'en',loadingLabel,scope,style:c.style,prefetch:c.prefetchViewportRatio,show:true,paused:false,error:false,frozen:false,
      units:0,created:0,next:0,limit:false,timer:0,scrollTimer:0,scrolled:-Infinity,anchors:new Map(),byNode:new WeakMap(),own:new WeakSet(),
      roots:[],walker:null,layouts:new Map(),layoutWrites:new WeakMap(),intersections:new WeakMap(),visible:new Set(),dirty:new Set(),paint:new Set(),batches:new Map(),invalid:new Set(),listeners:[]};
    const listen=(target,type,fn)=>{const capture=type==='scroll';target.addEventListener(type,fn,{passive:true,capture});s.listeners.push([target,type,fn,capture]);};
    const observeVisibility=()=>{
      if(s.io)s.io.disconnect();
      s.visible.clear();
      s.io=new IntersectionObserver(entries=>{if(!s)return;for(const e of entries){s.intersections.set(e.target,e.isIntersecting);for(const a of s.byNode.get(e.target)?.values()||[]){if(e.isIntersecting){s.visible.add(a);s.dirty.add(a);}else{s.visible.delete(a);a.priority='';if(a.loading)clean(a);if(!a.el.isConnected)s.dirty.add(a);}}}schedule();},{rootMargin:`0px 0px ${Math.round(innerHeight*s.prefetch)}px 0px`});
      for(const a of s.anchors.values()){s.intersections.set(a.el,false);s.io.observe(a.el);}
    };
    observeVisibility();
    s.mo=new MutationObserver(records=>{
      if(!s)return;
      for(const r of records) {
        const el=r.target.nodeType===1?r.target:r.target.parentElement;
        if(!el||s.own.has(el))continue;
        if(r.type==='attributes'&&r.attributeName==='style'&&s.layoutWrites.has(el)&&s.layoutWrites.get(el)===el.getAttribute('style'))continue;
        if(r.type==='childList' && [...r.addedNodes,...r.removedNodes].every(n=>s.own.has(n)))continue;
        for(let parent=el;parent;parent=parent.parentElement)
          for(const a of s.byNode.get(parent)?.values()||[]) {invalidate(a);s.dirty.add(a);}
        if(r.type==='attributes')for(const a of s.anchors.values())
          if(a.el!==el&&el.contains(a.el)){invalidate(a);s.dirty.add(a);}
        if(r.type==='childList')discover(el);
        let added=0;
        for(const n of r.addedNodes) {if(++added>128){s.limit=true;break;}if(n.nodeType===1)discover(n);}
        if(r.type==='attributes')discover(el);
      }
      schedule();
    });
    s.mo.observe(document.documentElement,{childList:true,subtree:true,characterData:true,attributes:true,attributeFilter:['class','style','hidden','translate','contenteditable','aria-hidden','role','data-testid','id']});
    listen(window,'scroll',()=>{s.scrolled=now();clearTimeout(s.scrollTimer);s.scrollTimer=setTimeout(()=>{for(const a of s.visible)s.dirty.add(a);schedule();},180);});
    const repaint=()=>{for(const a of s.anchors.values())if(a.host)s.paint.add(a);schedule();};
    listen(window,'resize',()=>{observeVisibility();repaint();});
    listen(matchMedia('(prefers-color-scheme: dark)'),'change',repaint);
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
    if(c.op==='release'){
      if(c.reason==='authentication'||c.reason==='quota') {s.error=true;for(const a of s.anchors.values())if(a.loading)clean(a);}
      const ok=settle(c.batchId,c.retryable&&!s.error);schedule();return {ok,...stats()};
    }
    if(c.op==='take') {
      tick();const segments=[],items=[];let units=0;
      if(paused()||now()-s.scrolled<180||s.batches.size>=2)return {ok:true,segments,...stats()};
      const ordered=[...s.visible].filter(a=>a.priority).sort((a,b)=>a.priority===b.priority?(a.nodes[0].compareDocumentPosition(b.nodes[0])&Node.DOCUMENT_POSITION_FOLLOWING?-1:1):a.priority==='visible'?-1:1);
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
        if(!a.el.isConnected||a.revision!==p.revision||!validElement(a.el)||!sameSource(a)) reason='staleSegment';
        else if(!value||value.segmentRevision!==p.revision||typeof value.translatedText!=='string'||!value.translatedText.trim()) reason='invalidResult';
        else if(!validTokens(p.text,value.translatedText))reason='invalidResult';
        else if(value.translatedText.length>8000||s.units+value.translatedText.length>LIMIT){reason='tooLarge';s.limit=true;}
        if(!reason){p.result=value.translatedText;s.units+=p.result.length;p.done=true;p.batch='';applied++;s.paint.add(a);}
        else {p.failed=true;p.batch='';s.paint.add(a);}
        results.push({segmentId:p.id,segmentRevision:p.revision,ok:!reason,reason});
      }
      s.batches.delete(c.batchId);schedule();
      return {ok:true,applied,rejected:results.length-applied,results,...stats()};
    }
    return {ok:false,reason:'invalidCommand'};
  }
  Object.defineProperty(globalThis,'__lumieTranslation',{value:{call},configurable:false,writable:false});
})();
