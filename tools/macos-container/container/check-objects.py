#!/usr/bin/env python3
"""Syntax-checks object files by name using ninja's recorded compile commands.

Read-only with respect to the ninja state. Usage: check-objects.py <substr>...
"""

import os
import pathlib
import shlex
import subprocess
import sys

OUT = pathlib.Path('/work/chromium/src/out/plan_kirin_pc')
NINJA = '/work/chromium/src/third_party/ninja/ninja'


def compile_command(target):
    result = subprocess.run([NINJA, '-C', str(OUT), '-t', 'commands', '-s', target],
                            capture_output=True, text=True, check=True)
    return result.stdout.strip().splitlines()[-1]


def syntax_args(command):
    args, skip = [], False
    for arg in shlex.split(command):
        if skip:
            skip = False
            continue
        if arg in ('-MF', '-o'):
            skip = True
            continue
        if arg in ('-MD', '-c'):
            continue
        args.append(arg)
    return args + ['-fsyntax-only']


targets = subprocess.run([NINJA, '-C', str(OUT), '-t', 'targets', 'all'],
                         capture_output=True, text=True, check=True).stdout
failures = 0
for needle in sys.argv[1:]:
    matches = [line.split(':')[0] for line in targets.splitlines()
               if line.split(':')[0].endswith('.o') and needle in line.split(':')[0]]
    if not matches:
        print(f'NO TARGET: {needle}')
        failures += 1
        continue
    for target in matches:
        result = subprocess.run(syntax_args(compile_command(target)), cwd=OUT,
                                capture_output=True, text=True)
        ok = result.returncode == 0
        print(f'{"OK" if ok else "FAIL"}: {target}')
        if not ok:
            failures += 1
            print('\n'.join(l for l in result.stderr.splitlines() if 'error' in l)[:3000])

sys.exit(1 if failures else 0)
