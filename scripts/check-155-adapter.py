#!/usr/bin/env python3
"""Verify the full native snapshot against its pinned 155 upstream files."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
manifest = json.loads((ROOT / 'patches/migration-155/manifest.json').read_text())
version, revision = sys.argv[1:3]
patch = ROOT / 'patches/migration-155/chromium.patch'
report = ROOT / 'upstream-compatibility.md'
with tempfile.TemporaryDirectory(prefix='adapter-155-') as folder:
    target = Path(folder)
    def git(*args, check=True):
        return subprocess.run(['git', '-C', str(target), *args], check=check,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    git('init', '-q')
    git('remote', 'add', 'origin', 'https://github.com/chromium/chromium.git')
    git('sparse-checkout', 'init', '--no-cone')
    paths = re.findall(r'^diff --git a/\S+ b/(\S+)$', patch.read_text(), re.M)
    (target / '.git/info/sparse-checkout').write_text(''.join(f'/{p}\n' for p in paths))
    result = git('fetch', '--depth=1', '--filter=blob:none', 'origin', revision, check=False)
    if result.returncode == 0:
        result = git('checkout', '-q', '--detach', 'FETCH_HEAD', check=False)
    if result.returncode == 0:
        result = git('fetch', '--depth=1', '--filter=blob:none', 'origin', manifest['repositories'][0]['base'], check=False)
    if result.returncode == 0:
        result = git('checkout', '-q', '--detach', manifest['repositories'][0]['base'], check=False)
    if result.returncode == 0:
        result = git('checkout', '-q', '--detach', revision, check=False)
    if result.returncode == 0:
        result = git('apply', '--3way', '--whitespace=nowarn', str(patch), check=False)
    compatible = result.returncode == 0
    details = result.stdout[-18000:]
    report.write_text(f'# Chromium 155 snapshot compatibility\n\n- Version: `{version}`\n'
                      f'- Revision: `{revision}`\n- Complete native snapshot applies: `{str(compatible).lower()}`\n'
                      '\nDEPS patches and native compilation are validated by the isolated self-hosted build.\n'
                      f'\n```text\n{details}\n```\n')
    print(report.read_text())
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            output.write(f'compatible={str(compatible).lower()}\n')
    if not compatible and os.environ.get('REQUIRE_COMPATIBLE') == '1':
        sys.exit(1)
