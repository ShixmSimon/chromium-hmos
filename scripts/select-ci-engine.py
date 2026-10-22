#!/usr/bin/env python3
"""Fail closed: only release adapter branches can select persistent trees."""
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def select(branch):
    branches = {'hmos-154-adapter': 154, 'hmos-155-adapter': 155}
    if branch not in branches:
        raise ValueError(f'Native CI is restricted to {", ".join(branches)}; got {branch!r}')
    major = branches[branch]
    config = json.loads((ROOT / f'config/engines/{major}.json').read_text())
    if config['source'] != f'/root/chromium-{major}/src' or not config['version'].startswith(f'{major}.'):
        raise ValueError('Engine configuration does not match its branch')
    return {'CHROMIUM_SRC': config['source'], 'CHROMIUM_VERSION': config['version'],
            'CHROMIUM_REVISION': config['revision'], 'CHROMIUM_MAJOR': str(major)}


if __name__ == '__main__':
    values = select(os.environ.get('GITHUB_BASE_REF') or os.environ.get('GITHUB_REF_NAME', ''))
    output = ''.join(f'{key}={value}\n' for key, value in values.items())
    print(output, end='')
    if os.environ.get('GITHUB_ENV'):
        with open(os.environ['GITHUB_ENV'], 'a') as stream:
            stream.write(output)
