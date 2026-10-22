#!/usr/bin/env python3
"""Apply the repository's builtins PGO choice to the persistent native build."""
import collections
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PROFILE = 'tools/builtins-pgo/profiles/x64.profile'
BUCKET = 'https://storage.googleapis.com/chromium-v8-builtins-pgo/by-version'


def setting(text, name, value):
    pattern = rf'(?m)^[ \t]*{re.escape(name)}[ \t]*=.*$'
    if len(re.findall(pattern, text)) > 1:
        raise ValueError(f'Duplicate GN argument: {name}')
    line = f'{name} = {value}'
    return re.sub(pattern, lambda _: line, text) if re.search(pattern, text) else text.rstrip() + '\n' + line + '\n'


def write_changed(path, content):
    if not path.exists() or path.read_bytes() != content:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)


def validate_meta(meta, revision, version):
    if meta.get('revision') != revision or meta.get('version') != version:
        raise ValueError('Builtins PGO profile does not match this Chromium V8 revision/version')
    if 'x64' not in meta.get('profile', []):
        raise ValueError('Missing x64 profile (also used for arm64 by upstream V8)')


def fetch(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read()


def prepare_profile(src):
    v8 = src / 'v8'
    revision = re.search(r"'v8_revision':\s*'([0-9a-f]{40})'", (src / 'DEPS').read_text()).group(1)
    actual_revision = subprocess.check_output(['git', '-C', str(v8), 'rev-parse', 'HEAD'], text=True).strip()
    if actual_revision != revision:
        raise ValueError('V8 checkout does not match Chromium DEPS')
    header = (v8 / 'include/v8-version.h').read_text()
    version = '.'.join(re.search(rf'#define {key}\s+(\d+)', header).group(1) for key in
                       ['V8_MAJOR_VERSION', 'V8_MINOR_VERSION', 'V8_BUILD_NUMBER', 'V8_PATCH_LEVEL'])
    base = f'{BUCKET}/{version}'
    meta = json.loads(fetch(f'{base}/meta.json'))
    validate_meta(meta, revision, version)
    target = v8 / PROFILE
    # Keep a separate receipt: upstream's meta.json describes ALL four profiles
    # and must not imply they were downloaded when only x64 was requested here.
    receipt = target.with_name('ohos-builtins-profile.json')
    cached = json.loads(receipt.read_text()) if receipt.exists() else {}
    data = target.read_bytes() if target.exists() else b''
    if (cached.get('revision') != revision or cached.get('version') != version or
            cached.get('sha256') != hashlib.sha256(data).hexdigest()):
        data = fetch(f'{base}/x64.profile')
    if not data.startswith(b'block_hint,') or b'builtin_hash,' not in data:
        raise ValueError('Invalid builtins PGO profile contents')
    result = {'revision': revision, 'version': version, 'profile': PROFILE,
              'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
    write_changed(target, data)
    write_changed(receipt, (json.dumps(result, indent=2) + '\n').encode())
    return result


def main():
    src, out, status = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    config = (ROOT / 'config/args.plan_kirin_pc.gn').read_text()
    enabled = re.search(r'(?m)^v8_enable_builtins_optimization\s*=\s*(true|false)\s*$', config).group(1) == 'true'
    context_snapshot = re.search(r'(?m)^use_v8_context_snapshot\s*=\s*(true|false)\s*$', config).group(1) == 'true'
    report = {'enabled': enabled}
    if enabled:
        report.update(prepare_profile(src))
    args = src / out / 'args.gn'
    original = args.read_text()
    updated = setting(original, 'v8_enable_builtins_optimization', str(enabled).lower())
    updated = setting(updated, 'v8_builtins_profiling_log_file', json.dumps(PROFILE if enabled else 'default'))
    updated = setting(updated, 'use_v8_context_snapshot', str(context_snapshot).lower())
    write_changed(args, updated.encode())
    if enabled or updated != original:
        subprocess.run([str(src / 'buildtools/linux64/gn'), 'gen', out], cwd=src, check=True)
    # Counts include any other pending work in this checkout, not just PGO.
    plan = subprocess.check_output([str(src / 'third_party/ninja/ninja'), '-C', out, '-n', 'gn_all'], cwd=src, text=True)
    tasks = re.findall(r'^\[\d+/\d+\]\s+(\S+)', plan, re.MULTILINE)
    report['pending_tasks'] = len(tasks)
    report['pending_by_action'] = dict(collections.Counter(tasks))
    status.mkdir(parents=True, exist_ok=True)
    (status / 'v8-builtins-pgo.json').write_text(json.dumps(report, indent=2) + '\n')
    context_report = {'enabled': context_snapshot, 'file': 'v8_context_snapshot.bin',
                      'pending_tasks': len(tasks)}
    (status / 'v8-context-snapshot.json').write_text(json.dumps(context_report, indent=2) + '\n')
    print('V8 context snapshot: ' + json.dumps(context_report), flush=True)
    print('V8 builtins PGO: ' + json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
