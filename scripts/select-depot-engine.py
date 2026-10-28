#!/usr/bin/env python3
"""Select a pinned Depot engine without trusting synthesized default-branch refs."""
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('engine_selection', ROOT / 'scripts/select-ci-engine.py')
selection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selection)


def select(branch):
    branches = {'hmos-154-adapter': 154, 'hmos-155-adapter': 155,
                'ci/depot-154': 154, 'ci/depot-155': 155}
    if branch not in branches:
        raise ValueError(f'Unsupported Depot engine branch: {branch!r}')
    major = branches[branch]
    values = selection.select(f'hmos-{major}-adapter')
    values['DEPOT_WORKSPACE'] = f'/mnt/chromium-{major}'
    values['CHROMIUM_SRC'] = f'/mnt/chromium-{major}/source/src'
    return values


def resolve_branch(sha, branch, remote_sha, is_ancestor=False):
    select(branch)
    # Queued push jobs may start after another commit reaches the branch.
    # Accept a published ancestor as well as the current head, never unrelated
    # commits. The CLI verifies the actual checkout separately below.
    if (re.fullmatch(r'[0-9a-f]{40}', sha) and
            re.fullmatch(r'[0-9a-f]{40}', remote_sha) and
            (sha == remote_sha or is_ancestor)):
        return branch
    raise ValueError(f'Commit {sha!r} does not belong to published branch {branch}')


def verify_checkout(branch, sha, root=ROOT):
    select(branch)  # Validate before constructing a ref.
    actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    if actual != sha:
        raise ValueError('Checkout does not match the requested commit')
    remote_ref = f'refs/remotes/origin/{branch}'
    remote = subprocess.check_output(['git', 'rev-parse', remote_ref], cwd=root, text=True).strip()
    ancestor = subprocess.run(['git', 'merge-base', '--is-ancestor', sha, remote_ref],
                              cwd=root, check=False).returncode == 0
    return resolve_branch(sha, branch, remote, ancestor)


if __name__ == '__main__':
    # depot ci run synthesizes GITHUB_REF from the default branch. The workflow
    # names its intended branch explicitly, while checkout always uses its SHA.
    branch = os.environ.get('DEPOT_BRANCH', '')
    if os.environ.get('GITHUB_EVENT_NAME') == 'push' and os.environ.get('GITHUB_REF') != f'refs/heads/{branch}':
        raise ValueError('Push ref does not match the configured engine branch')
    verify_checkout(branch, os.environ.get('GITHUB_SHA', ''))
    values = select(branch)
    output = ''.join(f'{key}={value}\n' for key, value in values.items())
    print(output, end='')
    for destination in ('GITHUB_ENV', 'GITHUB_OUTPUT'):
        if os.environ.get(destination):
            with open(os.environ[destination], 'a') as stream:
                stream.write(output)
