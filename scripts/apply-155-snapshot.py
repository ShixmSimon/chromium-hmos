#!/usr/bin/env python3
"""Three-way port the complete, successfully built 154 native adaptation."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / 'patches/migration-155'


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def apply_snapshot(target, patch):
    """Retry from upstream while keeping unchanged compiler inputs untouched."""
    paths = set(subprocess.check_output(
        ['git', '-C', str(target), 'diff', '--name-only', '-z', 'HEAD']).decode().split('\0'))
    paths.update(re.findall(r'^diff --git a/\S+ b/(\S+)$', patch.read_text(), re.M))
    saved = {}
    for name in paths - {''}:
        path = target / name
        if path.is_file() and not path.is_symlink():
            stat = path.stat()
            saved[name] = (hashlib.sha256(path.read_bytes()).digest(), stat.st_atime_ns, stat.st_mtime_ns)
    try:
        run('git', '-C', str(target), 'reset', '--hard', 'HEAD')
        if patch.stat().st_size:
            run('git', '-C', str(target), 'apply', '--3way', '--whitespace=nowarn', str(patch))
    finally:
        restored = 0
        for name, (digest, atime, mtime) in saved.items():
            path = target / name
            if path.is_file() and not path.is_symlink() and hashlib.sha256(path.read_bytes()).digest() == digest:
                os.utime(path, ns=(atime, mtime))
                restored += 1
        print(f'Preserved timestamps for {restored} unchanged compiler inputs', flush=True)


def main():
    src = Path(sys.argv[1]).resolve()
    if src != Path('/root/chromium-155/src'):
        raise ValueError('Migration may only modify /root/chromium-155/src')
    manifest = json.loads((PATCHES / 'manifest.json').read_text())
    actual = subprocess.check_output(['git', '-C', str(src), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != manifest['target_revision']:
        raise ValueError(f'155 pin mismatch: {actual}')
    for entry in manifest['repositories']:
        target = src / entry['path']
        actual = subprocess.check_output(
            ['git', '-C', str(target), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != entry['base']:
            raise ValueError(f'Dependency pin mismatch for {entry["path"]}: '
                             f'expected {entry["base"]}, got {actual}')
        patch = PATCHES / entry['patch']
        data = patch.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != entry['sha256']:
            raise ValueError(f'Patch integrity failure: {patch}')
        receipt = target / '.git/ohos-155-snapshot'
        if receipt.exists() and receipt.read_text().strip() == digest:
            print(f'Already migrated: {entry["path"]}', flush=True)
            continue
        # A failed three-way application leaves staged/conflicted files. A
        # subsequent pushed fix must retry from this repository's pinned
        # upstream revision, rather than stack over that half-applied patch.
        # This path is confined to 155 and never deletes untracked build data.
        print(f'Migrating {entry["path"]} ({len(data)} bytes)', flush=True)
        apply_snapshot(target, patch)
        receipt.write_text(digest + '\n')


if __name__ == '__main__':
    main()
