#!/usr/bin/env python3
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
"""Take the upstream side of remaining merge conflicts, under a path prefix.

Used during a Chromium version upgrade for conflicts that are feature trimming
rather than OHOS support: deferring them lets the build come up, and the
compiler then says which trims were actually load-bearing.

This resolves conflicts block by block rather than with `git checkout --ours`,
because a file often contains both kinds of conflict -- taking the whole
upstream file would also discard OHOS support already merged into it.

Everything discarded is written to a report. Dropping an adapter change is a
deliberate trade, and the trade is only safe if the discarded side stays
recoverable and reviewable.

Usage:
    scripts/take-upstream-side.py --src SRC --prefix chrome/ --report out.md
    scripts/take-upstream-side.py --src SRC --prefix chrome/ --report out.md --apply
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

CONFLICT_RE = re.compile(
    r'^<<<<<<< +(?P<ol>.*)\n'
    r'(?P<ours>(?:.*\n)*?)'
    r'(?:^\|\|\|\|\|\|\| +(?P<bl>.*)\n(?P<base>(?:.*\n)*?))?'
    r'^=======\n'
    r'(?P<theirs>(?:.*\n)*?)'
    r'^>>>>>>> +(?P<tl>.*)\n',
    re.M,
)
OHOS_RE = re.compile(r'OHOS|ohos|HarmonyOS')


def git(src: Path, *args: str, check: bool = True) -> str:
    r = subprocess.run(['git', '-C', str(src), *args], capture_output=True)
    if check and r.returncode != 0:
        raise RuntimeError(r.stderr.decode('utf-8', 'replace').strip())
    return r.stdout.decode('utf-8', 'replace')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, type=Path)
    ap.add_argument('--prefix', required=True,
                    help='only touch conflicted files under this path prefix')
    ap.add_argument('--report', required=True, type=Path,
                    help='where to write the record of discarded changes')
    ap.add_argument('--include-ohos-blocks', action='store_true',
                    help='also discard blocks whose adapter side mentions OHOS. '
                         'Off by default: those are platform support, not feature '
                         'trimming, and dropping them changes runtime behaviour '
                         'without any compile error to warn you.')
    ap.add_argument('--apply', action='store_true')
    args = ap.parse_args()
    src: Path = args.src

    files = [l for l in git(src, 'diff', '--name-only', '--diff-filter=U').splitlines()
             if l.startswith(args.prefix)]
    print(f'{len(files)} conflicted files under {args.prefix!r}')

    lines = ['# Adapter changes deferred during the 154 upgrade', '',
             'These conflict blocks were resolved by taking the upstream (154) side.',
             'Each one is adapter behaviour that is NOT in the tree right now.',
             'Review this list before treating a green build as a complete port.', '']
    n_blocks = 0
    n_kept_ohos = 0
    fully_resolved: list[str] = []
    still_conflicted: list[str] = []

    for path in files:
        target = src / path
        text = target.read_text(encoding='utf-8', errors='replace')
        out, pos, blocks = [], 0, []
        kept_here = 0

        for m in CONFLICT_RE.finditer(text):
            out.append(text[pos:m.start()])
            pos = m.end()
            if OHOS_RE.search(m.group('theirs')) and not args.include_ohos_blocks:
                # Adapter support, not feature trimming: leave the conflict in
                # place so a human decides how it maps onto the new upstream.
                out.append(m.group(0))
                kept_here += 1
                continue
            out.append(m.group('ours'))          # upstream side wins
            blocks.append(m.group('theirs'))
        out.append(text[pos:])

        n_kept_ohos += kept_here
        if kept_here:
            still_conflicted.append(path)
        elif blocks:
            fully_resolved.append(path)

        if not blocks:
            if args.apply and kept_here:
                target.write_text(''.join(out), encoding='utf-8')
            continue
        n_blocks += len(blocks)

        lines.append(f'## {path}')
        for i, b in enumerate(blocks, 1):
            lines.append('')
            lines.append(f'### block {i}{" (OHOS)" if OHOS_RE.search(b) else ""}')
            lines.append('```')
            lines.append(b.rstrip('\n') if b.strip() else '(empty -- the patch deleted this region)')
            lines.append('```')
        lines.append('')

        if args.apply:
            target.write_text(''.join(out), encoding='utf-8')
            if not kept_here:
                git(src, 'add', '--', path, check=False)

    args.report.write_text('\n'.join(lines), encoding='utf-8')

    print(f'blocks taken from upstream (feature trimming) : {n_blocks}')
    print(f'blocks kept conflicted (carry OHOS support)   : {n_kept_ohos}')
    print(f'files fully resolved                          : {len(fully_resolved)}')
    print(f'files still needing manual work               : {len(still_conflicted)}')
    for f in still_conflicted:
        print(f'    {f}')
    print(f'\nreport written to {args.report}')
    if not args.apply:
        print('Dry run -- no file modified.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
