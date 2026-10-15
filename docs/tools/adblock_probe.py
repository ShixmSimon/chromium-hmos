#!/usr/bin/env python3
"""Controlled request/CSS probe; run on a host reachable by the test device."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PAGE = '''<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Lumie adblock probe</title><style>body{font:18px sans-serif;max-width:48em;margin:2em}li{margin:1em 0}.lumie-ad-probe{background:#fbb;padding:1em}iframe{height:3em}code{user-select:all}</style>
<h1>Lumie 广告过滤验证</h1><p>先确认关闭时下面各项正常，再加入规则并手动刷新：</p>
<pre>/lumie-ad-probe/$script,image,subdocument,xmlhttprequest
##.lumie-ad-probe</pre>
<p>开启规则后：广告脚本、图片、iframe、fetch、重定向应被拦，红色元素应隐藏；绿色正常请求应继续成功。</p>
<div class="lumie-ad-probe">仅 CSS 隐藏的元素（不应增加网络拦截计数）</div>
<ul><li id="normal">正常请求待加载</li><li id="script">广告脚本未执行</li>
<li>广告图片：<img src="/lumie-ad-probe/pixel.svg" width="30"></li>
<li>重定向图片：<img src="/redirect" width="30"></li>
<li>广告 iframe：<iframe src="/lumie-ad-probe/frame"></iframe></li><li id="fetch">广告 fetch 待完成</li></ul>
<script>fetch('/normal').then(r=>r.text()).then(()=>{normal.textContent='正常请求成功';normal.style.color='green'});
fetch('/lumie-ad-probe/fetch').then(r=>r.text()).then(()=>{document.getElementById('fetch').textContent='广告 fetch 已加载（关闭时应出现）'}).catch(()=>{document.getElementById('fetch').textContent='广告 fetch 被阻止'});</script>
<script src="/lumie-ad-probe/script.js"></script>
<p>检查原生面板：本页应有 5 次网络拦截。切换本站开关后主动刷新，统计重新开始。</p>'''

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass  # No URL logs.

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/lumie-ad-probe/redirected.svg')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if path == '/':
            kind, body = 'text/html; charset=utf-8', PAGE
        elif path.endswith('.js'):
            kind, body = 'text/javascript', "document.getElementById('script').textContent='广告脚本已执行（关闭时应出现）';"
        elif path.endswith('.svg'):
            kind, body = 'image/svg+xml', '<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30"><rect width="30" height="30" fill="red"/></svg>'
        else:
            kind, body = 'text/html; charset=utf-8', '请求已加载'
        data = body.encode()
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8108)
    args = parser.parse_args()
    print(f'Probe listening on port {args.port}; use the host LAN IP on the device.', flush=True)
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()
