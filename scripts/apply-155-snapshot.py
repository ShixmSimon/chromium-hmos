#!/usr/bin/env python3
"""Three-way port the complete, successfully built 154 native adaptation."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / 'patches/migration-155'


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


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
        run('git', '-C', str(target), 'reset', '--hard', 'HEAD')
        print(f'Migrating {entry["path"]} ({len(data)} bytes)', flush=True)
        if data:
            # The old base blobs are present in the copied Git objects; retain
            # both upstream 155 changes and the complete OHOS modifications.
            run('git', '-C', str(target), 'apply', '--3way', '--whitespace=nowarn', str(patch))
        receipt.write_text(digest + '\n')


if __name__ == '__main__':
    main()
