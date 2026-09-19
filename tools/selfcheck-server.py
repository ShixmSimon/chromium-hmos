#!/usr/bin/env python3
"""Serve the self-check pages and collect their results as text.

Reading results off screenshots costs a screenshot per screen and cannot show
anything below the fold. The pages POST their findings here instead, so a run
produces a text file that can be diffed between builds.
"""
import http.server, json, socketserver, sys, datetime, pathlib

PORT = 8137
ROOT = pathlib.Path(__file__).parent
OUT = ROOT / 'results'
OUT.mkdir(exist_ok=True)


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(ROOT), **kw)

    def do_POST(self):
        if self.path != '/result':
            self.send_error(404)
            return
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n).decode('utf-8', 'replace')
        try:
            data = json.loads(body)
        except json.JSONDecodeError:
            data = {'raw': body}

        stamp = datetime.datetime.now().strftime('%H%M%S')
        name = data.get('page', 'run')
        path = OUT / f'{name}-{stamp}.txt'
        with path.open('w', encoding='utf-8') as f:
            f.write(f"# {name}  {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n")
            f.write(f"# {data.get('summary','')}\n\n")
            for r in data.get('rows', []):
                f.write(f"{r.get('flag','  ')} {r.get('name',''):<44} {r.get('value','')}\n")
        print(f'saved {path.name}  ({len(data.get("rows", []))} rows)', flush=True)

        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def end_headers(self):
        if 'Access-Control-Allow-Origin' not in self._headers_buffer[-1].decode('latin-1', 'ignore') \
           if self._headers_buffer else True:
            pass
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


if __name__ == '__main__':
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(('127.0.0.1', PORT), Handler) as srv:
        print(f'serving {ROOT} on 127.0.0.1:{PORT}, results -> {OUT}', flush=True)
        srv.serve_forever()
