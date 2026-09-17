#!/usr/bin/env python3
"""Re-runs the compiler command of FAILED ninja edges with -fsyntax-only.

Checks fixes without starting a second ninja in the output directory that the
running build owns. Usage: recheck-failed.py [substring-filter ...]
"""

import os
import pathlib
import shlex
import subprocess
import sys

LOG = pathlib.Path('/mnt/exchange/logs/50-build.log')
OUT = pathlib.Path('/work/chromium/src/out/plan_kirin_pc')

filters = sys.argv[1:]
lines = LOG.read_text(errors='replace').splitlines()
failures = 0

for index, line in enumerate(lines):
    if not line.startswith('FAILED: ') or index + 1 >= len(lines):
        continue
    target = line[len('FAILED: '):].strip()
    if filters and not any(f in target for f in filters):
        continue
    args = shlex.split(lines[index + 1])
    cleaned = []
    skip_next = False
    for arg in args:
        if skip_next:
            skip_next = False
            continue
        if arg in ('-MF', '-o'):
            skip_next = True
            continue
        if arg in ('-MD', '-c'):
            continue
        cleaned.append(arg)
    # Flags that a pending GN regeneration would add, e.g. new defines.
    cleaned.extend(shlex.split(os.environ.get('RECHECK_EXTRA_FLAGS', '')))
    cleaned.append('-fsyntax-only')
    result = subprocess.run(cleaned, cwd=OUT, capture_output=True, text=True)
    status = 'OK' if result.returncode == 0 else 'STILL FAILING'
    print(f'{status}: {target}')
    if result.returncode != 0:
        failures += 1
        errors = [l for l in result.stderr.splitlines() if 'error:' in l]
        print('\n'.join(errors[:8]))

sys.exit(1 if failures else 0)
