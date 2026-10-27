#!/usr/bin/env python3
"""Map only the two Depot trial branches to the existing pinned engines."""
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
    branches = {'ci/depot-154': 154, 'ci/depot-155': 155}
    if branch not in branches:
        raise ValueError(f'Depot trials require ci/depot-154 or ci/depot-155; got {branch!r}')
    major = branches[branch]
    values = selection.select(f'hmos-{major}-adapter')
    values['DEPOT_ENGINE_DISK'] = f'chromium-hmos-depot-{major}-native-v1'
    return values


def resolve_branch(ref, sha, trial_branch, remote_sha):
    select(trial_branch)  # Validate the allowlist before constructing a Git ref.
    if ref in (trial_branch, f'refs/heads/{trial_branch}'):
        return trial_branch
    # Depot's local run API sets ref to the commit SHA. Accept only the exact
    # published head of the trial branch selected by this workflow.
    if re.fullmatch(r'[0-9a-f]{40}', ref) and ref == sha == remote_sha:
        return trial_branch
    raise ValueError(f'Ref {ref!r} does not identify the published {trial_branch} trial')


if __name__ == '__main__':
    # Never infer the engine from a PR base ref: these trials run only on their
    # own branches and never accept arbitrary upstream source paths.
    trial = os.environ.get('DEPOT_TRIAL_BRANCH', '')
    select(trial)
    remote_sha = subprocess.check_output(
        ['git', 'rev-parse', f'refs/remotes/origin/{trial}'], cwd=ROOT, text=True).strip()
    sha = os.environ.get('GITHUB_SHA', '')
    actual_sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if actual_sha != sha:
        raise ValueError('Checkout does not match the requested commit')
    branch = resolve_branch(os.environ.get('GITHUB_REF', ''), sha, trial, remote_sha)
    values = select(branch)
    output = ''.join(f'{key}={value}\n' for key, value in values.items())
    print(output, end='')
    for destination in ('GITHUB_ENV', 'GITHUB_OUTPUT'):
        if os.environ.get(destination):
            with open(os.environ[destination], 'a') as stream:
                stream.write(output)
