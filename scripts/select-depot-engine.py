#!/usr/bin/env python3
"""Map only the two Depot trial branches to the existing pinned engines."""
import importlib.util
import os
from pathlib import Path

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


if __name__ == '__main__':
    # Never infer the engine from a PR base ref: these trials run only on their
    # own branches and never accept arbitrary upstream source paths.
    values = select(os.environ.get('GITHUB_REF_NAME', ''))
    output = ''.join(f'{key}={value}\n' for key, value in values.items())
    print(output, end='')
    for destination in ('GITHUB_ENV', 'GITHUB_OUTPUT'):
        if os.environ.get(destination):
            with open(os.environ[destination], 'a') as stream:
                stream.write(output)
