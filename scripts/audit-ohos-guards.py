#!/usr/bin/env python3
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
"""Find platform guards widened to OHOS that now enclose platform-specific APIs.

A version upgrade re-applies the adapter's platform-list edits, and the common
edit is appending `BUILDFLAG(IS_OHOS)` to a guard that already lists the desktop
platforms. Doing that by text similarity is safe for the usual case -- a guard
that gates a feature flag -- and wrong for the case where the guard gates code
that calls into one platform's SDK. `scripts/merge-platform-lists.py` cannot see
the difference: both are the same shape.

The linker catches half of those mistakes for free: if the widened block calls a
symbol that does not exist for OHOS, the build fails. What it cannot catch is a
symbol that *does* exist and means something else, or one reached only at
runtime. That is the residue this script looks for.

For every `#if` whose OHOS entry the working tree added, the enclosed block is
scanned for identifiers belonging to one platform. A hit is not proof of a bug:
a block may legitimately mention `base::win::` inside a nested `IS_WIN` guard.
It is a place where a human has to decide, and the point is that there are few
enough of those to actually look at.

Usage:
    scripts/audit-ohos-guards.py --src /path/to/chromium/src
    scripts/audit-ohos-guards.py --src ... --base HEAD --show-body
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Identifiers that belong to exactly one platform's SDK or windowing system.
# Reaching one of these from a block OHOS now enters is the thing worth seeing.
PLATFORM_APIS = {
    'windows': re.compile(
        r'\bbase::win::|\bwin::ScopedHandle|\bHKEY\b|\bHRESULT\b|\bLPWSTR\b'
        r'|\bCoCreateInstance|\bShellExecute|windows_version\.h'),
    'apple': re.compile(
        r'\bbase::mac::|\bbase::apple::|\bCFRelease|\bCFStringRef|\bNSString'
        r'|\bSecKeychain|\bIOKit'),
    'linux-desktop': re.compile(
        r'\bXDG_|\bXOpenDisplay|\bxdg_|\bdbus_|\bDbus|\bGtk|\bgtk_'
        r'|shell_integration_linux|\bWAYLAND_'),
    'chromeos': re.compile(r'\bash::|\bchromeos::|\bcrosapi'),
    'android': re.compile(r'\bJNIEnv\b|\bScopedJavaLocalRef|\bAttachCurrentThread'),
}

# The OHOS token, and the guard shapes the merge script produces.
OHOS_TOKEN = re.compile(r'BUILDFLAG\(IS_OHOS\)')
IF_START = re.compile(r'^\s*#\s*(if|elif)\b')
COND_END = re.compile(r'^\s*#\s*(endif|else|elif)\b')


def git(src: Path, *args: str) -> str:
    r = subprocess.run(['git', '-C', str(src), *args], capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.decode('utf-8', 'replace').strip())
    return r.stdout.decode('utf-8', 'replace')


def added_ohos_lines(src: Path, base: str) -> dict[str, set[str]]:
    """Map path -> set of added lines that introduce an OHOS guard entry."""
    out: dict[str, set[str]] = {}
    diff = git(src, 'diff', base, '-U0', '--', '*.cc', '*.h', '*.mm')
    path = None
    for line in diff.split('\n'):
        if line.startswith('+++ b/'):
            path = line[6:]
        elif line.startswith('+') and path and OHOS_TOKEN.search(line):
            body = line[1:]
            if IF_START.match(body) or body.strip().startswith('BUILDFLAG(IS_'):
                out.setdefault(path, set()).add(body.rstrip())
    return out


def directive_span(lines: list[str], start: int) -> tuple[int, int]:
    """Return (first, last) line indices of the `#if` directive at `start`.

    The directive can be continued with backslashes, so the condition is not
    necessarily one line.
    """
    end = start
    while end < len(lines) - 1 and lines[end].rstrip().endswith('\\'):
        end += 1
    return start, end


def block_body(lines: list[str], after: int) -> tuple[list[str], int]:
    """Lines governed by a directive ending at `after`, to its #else/#endif."""
    depth = 0
    body: list[str] = []
    i = after + 1
    while i < len(lines):
        l = lines[i]
        if IF_START.match(l) and re.match(r'^\s*#\s*if', l):
            depth += 1
        elif COND_END.match(l):
            if depth == 0:
                break
            if re.match(r'^\s*#\s*endif', l):
                depth -= 1
        body.append(l)
        i += 1
    return body, i


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, type=Path)
    ap.add_argument('--base', default='HEAD',
                    help='revision the adapter edits are measured against')
    ap.add_argument('--show-body', action='store_true',
                    help='print the offending lines, not just a count')
    args = ap.parse_args()
    src: Path = args.src

    targets = added_ohos_lines(src, args.base)
    n_guards = sum(len(v) for v in targets.values())
    print(f'{n_guards} widened guards across {len(targets)} files\n')

    flagged = 0
    for path in sorted(targets):
        f = src / path
        if not f.is_file():
            continue
        lines = f.read_text(encoding='utf-8', errors='replace').split('\n')
        wanted = targets[path]

        for i, line in enumerate(lines):
            if line.rstrip() not in wanted:
                continue
            # Walk back to the `#if` this line belongs to (it may be a
            # continuation) so the whole condition is considered.
            top = i
            while top > 0 and lines[top - 1].rstrip().endswith('\\'):
                top -= 1
            if not IF_START.match(lines[top]):
                continue
            _, last = directive_span(lines, top)
            body, _ = block_body(lines, last)

            hits = {name: [l.strip() for l in body if rx.search(l)]
                    for name, rx in PLATFORM_APIS.items()}
            hits = {k: v for k, v in hits.items() if v}
            if not hits:
                continue

            flagged += 1
            print(f'!! {path}:{top + 1}')
            print(f'      {lines[top].strip()[:100]}')
            for name, ls in hits.items():
                print(f'      reaches {name} API:')
                for l in (ls if args.show_body else ls[:2]):
                    print(f'        {l[:96]}')
                if not args.show_body and len(ls) > 2:
                    print(f'        ... {len(ls) - 2} more')
            print()

    print(f'summary: {flagged} of {n_guards} widened guards enclose '
          f'platform-specific API use')
    if flagged:
        print('\nEach is a guard the merge widened to OHOS whose block reaches into one\n'
              'platform\'s SDK. Some are legitimate -- a nested inner guard may still\n'
              'exclude OHOS -- so each needs a human decision. What the linker already\n'
              'proved is only that the symbols resolve, not that they mean the right\n'
              'thing on this platform.')
    return 1 if flagged else 0


if __name__ == '__main__':
    sys.exit(main())
