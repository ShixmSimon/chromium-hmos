#!/usr/bin/env python3
"""Controlled native-adblock acceptance server. No adblock emulation in this server.
Open http://COMPUTER_LAN_IP:8109/ on the device, install the displayed personal
rules, reload, then run. Only controlled probe paths are counted, in memory.
"""
import argparse, base64, hashlib, json, threading, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

counts = {}
lock = threading.Lock()
PAGE = r'''<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<script nonce="probe">window.firstInlineSaw=window.lumieAdblockEarly===true;</script>
<script>window.originalCspBroken=true;</script>
<title>Lumie · Brave 内核验收</title>
<style>body{font:16px system-ui;max-width:850px;margin:24px auto;padding:16px}button,textarea{font:inherit;padding:12px}textarea{width:95%;height:250px}.ok{color:#167a40}.bad{color:#a22}pre{white-space:pre-wrap}</style>
<h1>Brave 广告过滤内核验收</h1>
<p>先复制下列个人规则到 Lumie 广告拦截设置，保存后刷新本页。请在测试完成后删除这组规则。每种进程模式各跑一次；本页不修改应用配置。</p>
<textarea readonly id="rules">RULES</textarea><p><button id="run">开始验证</button> <button id="export">导出结果</button></p>
<p>单纯网络失败不算通过：测试结束会核对服务端是否实际收到被拦截请求。页面脚本未做模拟拦截。</p>
<div class="lumie-static" id="static">静态隐藏目标</div><div class="lumie-procedural" id="proc">Sponsored</div><div id="keep">正文，应保持可见</div>
<pre id="results"></pre>
<script nonce="probe">
const report={created:new Date().toISOString(),userAgent:navigator.userAgent,results:[]};
const output=document.querySelector('#results');
function record(name,ok,detail=''){report.results.push({name,ok,detail});output.textContent+=`${ok?'PASS':'FAIL'} ${name} ${detail}\n`;}
const delay=ms=>new Promise(r=>setTimeout(r,ms));
const withTimeout=(p,ms=5000)=>Promise.race([p,delay(ms).then(()=>{throw Error('timeout')})]);
function script(path){return new Promise(resolve=>{const s=document.createElement('script');s.src=path;s.nonce='probe';s.onload=()=>resolve(true);s.onerror=()=>resolve(false);document.head.append(s)})}
function image(path){return new Promise(resolve=>{const i=new Image();i.onload=()=>resolve(i.naturalWidth);i.onerror=()=>resolve(0);i.src=path})}
async function fetchable(path){try{const r=await fetch(path,{cache:'no-store'});return r.ok}catch{return false}}
function socket(path){return new Promise(resolve=>{const ws=new WebSocket('ws://'+location.host+path);ws.onopen=()=>{ws.close();resolve(true)};ws.onerror=()=>resolve(false);setTimeout(()=>{ws.close();resolve(false)},3000)})}
async function worker(kind,run){const port=kind==='shared'?new SharedWorker('/shared-worker.js').port:new Worker('/worker.js');
return await withTimeout(new Promise(resolve=>{port.onmessage=e=>{resolve(e.data);if(port.terminate)port.terminate();else port.close()};if(port.start)port.start();port.postMessage({run,kind})}))}
async function serviceWorker(run){if(!('serviceWorker' in navigator))return {unsupported:'HTTP LAN origin is not secure; use localhost USB forwarding or HTTPS for service worker test'};
const r=await navigator.serviceWorker.register('/sw.js',{scope:'/sw-scope/'});let w=r.active||r.waiting||r.installing;
if(w.state!=='activated')await withTimeout(new Promise(resolve=>w.addEventListener('statechange',()=>{if(w.state==='activated')resolve()})));
const c=new MessageChannel();const result=await withTimeout(new Promise(resolve=>{c.port1.onmessage=e=>resolve(e.data);w.postMessage({run,kind:'service'},[c.port2])}));await r.unregister();return result}
async function frameProbe(path){return new Promise(resolve=>{const f=document.createElement('iframe');let timer;const done=value=>{clearTimeout(timer);removeEventListener('message',handler);f.remove();resolve(value)};const handler=e=>{if(e.source===f.contentWindow&&e.data?.probe==='frame')done(e.data)};addEventListener('message',handler);timer=setTimeout(()=>done(null),3000);f.src=path;document.body.append(f)})}
async function csp(run){return await withTimeout(new Promise(resolve=>{const f=document.createElement('iframe');const handler=e=>{if(e.source===f.contentWindow&&e.data?.probe==='csp'){removeEventListener('message',handler);f.remove();resolve(e.data)}};addEventListener('message',handler);f.src='/csp-target?run='+run;document.body.append(f)}))}
document.querySelector('#run').onclick=async()=>{
 document.querySelector('#run').disabled=true;output.textContent='';report.results=[];
 const run=Date.now().toString(36);report.run=run;const q='?run='+run;
 try{
 record('scriptlet: 第一段内联脚本',window.firstInlineSaw);
 record('原站点 CSP 仍有效',!window.originalCspBroken);
 record('静态 CSS',getComputedStyle(document.querySelector('#static')).display==='none');
 await delay(200);record('procedural has-text',getComputedStyle(document.querySelector('#proc')).display==='none');
 record('cosmetic 负例',getComputedStyle(document.querySelector('#keep')).display!=='none');
 const d=document.createElement('div');d.className='lumie-procedural';d.textContent='Sponsored';document.body.append(d);await delay(250);record('动态 DOM',getComputedStyle(d).display==='none');d.remove();
 record('script 阻断',!await script('/lumie-block/script.js'+q));record('script 例外',await script('/lumie-allow/script.js'+q));
 record('important 优先',!await script('/lumie-important/script.js'+q));
 record('fetch 阻断',!await fetchable('/lumie-block/fetch'+q));record('fetch 允许',await fetchable('/lumie-allow/fetch'+q));
 record('image 阻断',(await image('/lumie-block/pixel.gif'+q))===0);
 record('image 允许',(await image('/lumie-allow/pixel.gif'+q))===1);
 record('iframe 阻断',(await frameProbe('/lumie-block/frame'+q))===null);
 const frame=await frameProbe('/lumie-allow/frame'+q);record('iframe 允许',!!frame);record('iframe 首段 scriptlet',frame?.early===true);
 record('redirect JS 可消费',await script('/lumie-replace.js'+q));
 record('redirect 原 JS 未执行',!window.originalReplacementExecuted);
 record('redirect image 1x1',(await image('/lumie-replace.gif'+q))===1);
 record('redirect-rule 单独不阻断',await script('/lumie-rule-only.js'+q));record('redirect-rule 原 JS 执行',window.ruleOnlyOriginal===true);
 const clean=await (await fetch('/lumie-clean'+q+'&adtrack=remove&keep=%2F&keep=two')).json();
 record('removeparam 服务端 URL',!clean.query.includes('adtrack=')&&clean.query.includes('keep=%2F&keep=two'),clean.query);
 const redirectClean=await (await fetch('/jump-clean'+q+'&adtrack=remove&keep=x')).json();record('重定向后 removeparam',!redirectClean.query.includes('adtrack=')&&redirectClean.query.includes('keep=x'));
 const c=await csp(run);record('新增 CSP 限制',c.imageBlocked);record('CSP 原策略保留',c.originalPreserved);
 for(const kind of ['dedicated','shared']){const r=await worker(kind,run);record(kind+' worker 阻断/允许',!r.blocked&&r.allowed)}
 const sw=await serviceWorker(run);if(sw.unsupported)record('service worker',false,'待安全上下文复测：'+sw.unsupported);else record('service worker 阻断/允许',!sw.blocked&&sw.allowed);
 record('WebSocket 阻断',!await socket('/lumie-block/socket'+q));record('WebSocket 允许',await socket('/lumie-allow/socket'+q));
 await delay(250);const server=await (await fetch('/report'+q)).json();report.server=server;
 const forbidden=Object.keys(server).filter(k=>k.startsWith('/lumie-block/')||k.startsWith('/lumie-important/')||k.startsWith('/lumie-replace.')||k==='/csp-image.gif');
 record('服务端确认：阻断及替代请求均未抵达',forbidden.length===0,JSON.stringify(forbidden));
 record('服务端正例：允许请求确实抵达',server['/lumie-allow/script.js']>0&&server['/lumie-rule-only.js']>0&&server['/lumie-allow/socket']>0);
 }catch(e){record('测试异常',false,String(e))}finally{document.querySelector('#run').disabled=false;}
};
document.querySelector('#export').onclick=()=>{const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(report,null,2)],{type:'application/json'}));a.download='lumie-adblock-'+report.run+'.json';a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)};
</script>'''

WORKER = """async function run(data){const q='?run='+data.run;let blocked=false,allowed=false;try{blocked=(await fetch('/lumie-block/'+data.kind+q)).ok}catch{}try{allowed=(await fetch('/lumie-allow/'+data.kind+q)).ok}catch{}return {blocked,allowed}};"""

def rules(host):
    # Exact hostname, excluding port; bracketed IPv6 is intentionally rejected.
    host=host.split(':')[0]
    if not host or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-' for c in host):
        return '! Use an IPv4 address or hostname for this probe.'
    return '\n'.join([
        '/lumie-block/$script,image,xmlhttprequest,subdocument,websocket',
        '/lumie-important/$script,important', '@@/lumie-important/$script',
        '/lumie-replace.js$script,redirect=noopjs',
        '/lumie-replace.gif$image,redirect=1x1.gif',
        '/lumie-rule-only.js$script,redirect-rule=noopjs',
        '/lumie-clean$removeparam=adtrack',
        "/csp-target$csp=img-src 'none'",
        f'{host}##+js(set-constant, lumieAdblockEarly, true)',
        f'{host}##.lumie-static',f'{host}##.lumie-procedural:has-text(Sponsored)'])

class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args): pass
    def send(self,body,mime='text/plain',status=200,headers=None):
        body=body.encode() if isinstance(body,str) else body
        self.send_response(status)
        self.send_header('Content-Type',mime)
        self.send_header('Cache-Control','no-store')
        self.send_header('Content-Length',str(len(body)))
        for k,v in (headers or {}).items(): self.send_header(k,v)
        self.end_headers();self.wfile.write(body)
    def do_GET(self):
        parsed=urllib.parse.urlsplit(self.path);p=parsed.path
        run=urllib.parse.parse_qs(parsed.query).get('run',[''])[0][:32]
        if run and (p.startswith('/lumie-') or p=='/csp-image.gif'):
            with lock:
                if len(counts)>100:counts.clear()
                bucket=counts.setdefault(run,{})
                bucket[p]=bucket.get(p,0)+1
        if p=='/':
            import html
            return self.send(PAGE.replace('RULES',html.escape(rules(self.headers.get('Host','')))),'text/html',headers={'Content-Security-Policy':"default-src 'self'; script-src 'nonce-probe'; style-src 'unsafe-inline'; connect-src 'self' ws:; worker-src 'self'"})
        if p=='/rules.txt':return self.send(rules(self.headers.get('Host','')))
        if p=='/report':
            with lock: data=dict(counts.get(run,{}))
            return self.send(json.dumps(data),'application/json')
        if p=='/lumie-clean':return self.send(json.dumps({'query':parsed.query}),'application/json')
        if p=='/jump-clean':return self.send('',status=302,headers={'Location':'/lumie-clean?'+parsed.query})
        if p=='/worker.js':return self.send(WORKER+'onmessage=async e=>postMessage(await run(e.data));','application/javascript')
        if p=='/shared-worker.js':return self.send(WORKER+'onconnect=e=>{const p=e.ports[0];p.onmessage=async e=>p.postMessage(await run(e.data));p.start()};','application/javascript')
        if p=='/sw.js':return self.send(WORKER+"self.addEventListener('install',()=>self.skipWaiting());self.addEventListener('activate',e=>e.waitUntil(clients.claim()));self.addEventListener('message',e=>e.waitUntil(run(e.data).then(r=>e.ports[0].postMessage(r))));",'application/javascript')
        if p in ('/lumie-allow/frame','/lumie-block/frame'):
            return self.send('''<!doctype html><script>parent.postMessage({probe:'frame',early:window.lumieAdblockEarly===true},location.origin);</script>''','text/html')
        if p=='/csp-target':
            body='''<!doctype html><script>window.badInline=true;</script><script nonce="probe">let i=new Image();i.onload=()=>parent.postMessage({probe:'csp',imageBlocked:false,originalPreserved:!window.badInline},location.origin);i.onerror=()=>parent.postMessage({probe:'csp',imageBlocked:true,originalPreserved:!window.badInline},location.origin);i.src='/csp-image.gif?run='''+run+"';</script>"
            return self.send(body,'text/html',headers={'Content-Security-Policy':"script-src 'nonce-probe'; img-src 'self'"})
        if self.headers.get('Upgrade','').lower()=='websocket':
            key=self.headers.get('Sec-WebSocket-Key','')
            accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
            self.send_response(101);self.send_header('Upgrade','websocket');self.send_header('Connection','Upgrade');self.send_header('Sec-WebSocket-Accept',accept);self.end_headers();self.wfile.flush();self.connection.settimeout(5)
            try:self.rfile.read(2);self.wfile.write(b'\x88\x00');self.wfile.flush()
            except OSError:pass
            self.close_connection=True;return
        if p.endswith('.gif'):return self.send(base64.b64decode('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7'),'image/gif')
        if p=='/lumie-replace.js':return self.send('window.originalReplacementExecuted=true;','application/javascript')
        if p=='/lumie-rule-only.js':return self.send('window.ruleOnlyOriginal=true;','application/javascript')
        if p.endswith('.js'):return self.send('/* allowed controlled script */','application/javascript')
        if p.startswith('/lumie-'):return self.send('ok')
        self.send('not found',status=404)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind',default='0.0.0.0');parser.add_argument('--port',type=int,default=8109)
    args=parser.parse_args()
    print(f'Open http://COMPUTER_LAN_IP:{args.port}/ ; keep this server running. Service workers require HTTPS or localhost forwarding.',flush=True)
    ThreadingHTTPServer((args.bind,args.port),Handler).serve_forever()
