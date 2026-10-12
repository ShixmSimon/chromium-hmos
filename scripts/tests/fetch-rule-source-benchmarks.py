#!/usr/bin/env python3
"""Fetch fixed full-size test data, never packaged as runtime protection rules."""
import hashlib
from pathlib import Path
import sys
from urllib.request import urlopen

FEEDS = [
    ('hagezi.txt', 'hagezi/dns-blocklists', 'a4ed692a06f6a7144e245d965e48ecb02c120fc1',
     'wildcard/tif.mini-onlydomains.txt', 'b72f362ab292edfd75a8ba912559a29e1b66eb6a5e2b3a086b7c16aa95a03fda'),
    ('phishdestroy.txt', 'phishdestroy/destroylist', '458be8e3f3f6c4aa028a1d772ecf3a5a107053a8',
     'dns/content_active.txt', '5e44dc561ec077f135a429c5c31aaff67c15a789791f24c1b9cedbdb3794afa6'),
]
root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
for name, repo, revision, path, expected in FEEDS:
    url = f'https://raw.githubusercontent.com/{repo}/{revision}/{path}'
    with urlopen(url, timeout=45) as response:
        raw = response.read(16 * 1024 * 1024 + 1)
    if hashlib.sha256(raw).hexdigest() != expected:
        raise SystemExit(f'Pinned benchmark digest mismatch: {name}')
    (root / name).write_bytes(raw)
    # Keep the upstream license alongside benchmark data. No rule modifications.
    with urlopen(f'https://raw.githubusercontent.com/{repo}/{revision}/LICENSE', timeout=45) as response:
        (root / (name + '.LICENSE')).write_bytes(response.read(65536))
    print(f'Verified full benchmark {name}: {len(raw)} bytes')
