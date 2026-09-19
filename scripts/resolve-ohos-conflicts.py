#!/usr/bin/env python3
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
"""Resolve the OHOS side of three-way merge conflicts in a Chromium upgrade.

When the adapter patch is applied to a newer Chromium with `git apply --3way`,
most conflicts are not semantic disagreements. They are the same expression
edited twice: upstream restructured a platform list while the adapter appended
an OHOS entry to it. Taking either side alone is wrong -- `theirs` loses the
upstream change (and can reference macros upstream has since renamed), `ours`
silently drops OHOS support.

The correct result is `ours` plus the OHOS delta, and the delta is computable
because a diff3-style conflict carries all three versions:

    <<<<<<< ours        the new Chromium
    ||||||| base        the Chromium the patch was generated against
    =======
    >>>>>>> theirs      base + adapter patch

    OHOS delta = theirs - base
    resolution = ours + OHOS delta

Only conflicts whose delta is purely OHOS support are resolved. Hunks that
delete upstream code, or that mix OHOS changes with unrelated edits, keep their
conflict markers: those are precisely the cases where a wrong automatic choice
stays invisible until runtime. Unrelated downstream code that happens to carry
no OHOS marker -- the blind spot a keyword search cannot see -- lands in the
manual pile by construction.

Usage:
    scripts/resolve-ohos-conflicts.py --src /path/to/chromium/src
    scripts/resolve-ohos-conflicts.py --src ... --apply
"""

from __future__ import annotations

import argparse
import difflib
import re
import subprocess
import sys
from pathlib import Path

OHOS_RE = re.compile(r'OHOS|ohos|HarmonyOS')

# Lines carrying no logic of their own: block terminators, punctuation, comments.
# An `#if BUILDFLAG(IS_OHOS) ... #endif` block is OHOS support even though its
# `#endif` says nothing about OHOS, so these are exempt from the marker check.
# Lines with actual logic are not exempt -- that is what keeps unrelated
# downstream code out of the automatic path.
STRUCTURAL_RE = re.compile(
    r'^\s*$'
    r'|^[\s{}()\[\];,]*$'
    r'|^\s*#\s*(endif|else)\b'
    r'|^\s*(//|/\*|\*)'
)

CONFLICT_RE = re.compile(
    r'^<<<<<<< +(?P<ol>.*)\n'
    r'(?P<ours>(?:.*\n)*?)'
    r'^\|\|\|\|\|\|\| +(?P<bl>.*)\n'
    r'(?P<base>(?:.*\n)*?)'
    r'^=======\n'
    r'(?P<theirs>(?:.*\n)*?)'
    r'^>>>>>>> +(?P<tl>.*)\n',
    re.M,
)


def git(src: Path, *args: str, check: bool = True) -> str:
    r = subprocess.run(['git', '-C', str(src), *args], capture_output=True)
    if check and r.returncode != 0:
        raise RuntimeError(r.stderr.decode('utf-8', 'replace').strip())
    return r.stdout.decode('utf-8', 'replace')


def classify(added: list[str], removed: list[str]) -> str | None:
    """Return None if this delta is pure OHOS support, else why it is not."""
    if not added:
        return 'patch only deletes upstream code'
    if not any(OHOS_RE.search(l) for l in added):
        return 'no OHOS marker anywhere in the added lines'
    offenders = [l for l in added
                 if not STRUCTURAL_RE.match(l) and not OHOS_RE.search(l)]
    if offenders:
        return f'non-OHOS logic: {offenders[0].strip()[:60]!r}'
    if removed and any(OHOS_RE.search(l) for l in removed):
        return 'delta also removes OHOS lines'
    return None


def resolve_block(ours: list[str], base: list[str], theirs: list[str]
                  ) -> tuple[list[str] | None, str]:
    """Re-apply the base->theirs OHOS delta on top of ours."""
    sm = difflib.SequenceMatcher(None, base, theirs, autojunk=False)
    ops = [op for op in sm.get_opcodes() if op[0] != 'equal']
    if not ops:
        return None, 'no delta between base and theirs'

    added = [l for _, _, _, j1, j2 in ops for l in theirs[j1:j2]]
    removed = [l for _, i1, i2, _, _ in ops for l in base[i1:i2]]
    why = classify(added, removed)
    if why:
        return None, why

    result = list(ours)
    for tag, i1, i2, j1, j2 in reversed(ops):
        new = theirs[j1:j2]
        old = base[i1:i2]
        if tag == 'insert':
            # Anchor on the base line before the insertion; fall back to
            # appending when the insertion is at the very start of the block.
            if i1 == 0:
                result[0:0] = new
                continue
            anchor = base[i1 - 1]
            hits = [k for k, l in enumerate(result) if l == anchor]
            if len(hits) != 1:
                return None, f'insertion anchor not unique: {anchor.strip()[:50]!r}'
            result[hits[0] + 1:hits[0] + 1] = new
        elif tag == 'replace':
            if len(old) == len(result) and len(ops) == 1:
                # The whole block is one rewritten region: upstream rewrote the
                # same lines the adapter edited. Re-deriving the OHOS edit means
                # understanding both changes, which is a human's job.
                return None, 'upstream rewrote the same region the patch edits'
            placed = False
            for k, l in enumerate(result):
                if l == old[0]:
                    result[k:k + len(old)] = new
                    placed = True
                    break
            if not placed:
                return None, f'replaced text not found in ours: {old[0].strip()[:50]!r}'
        else:
            return None, f'unsupported op {tag}'
    return result, 'ok'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, type=Path)
    ap.add_argument('--apply', action='store_true',
                    help='write resolved files (default: dry run)')
    ap.add_argument('--show-diff', action='store_true')
    args = ap.parse_args()
    src: Path = args.src

    files = [l for l in git(src, 'diff', '--name-only', '--diff-filter=U').splitlines() if l]
    print(f'{len(files)} conflicted files')

    # Rewrite the conflict markers in diff3 style so each block carries the base
    # version. Without it the delta has no reference point.
    if files:
        git(src, 'checkout', '--conflict=diff3', '--', *files)
    print('conflict markers rewritten in diff3 form\n')

    stats = {'resolved': 0, 'kept': 0}
    reasons: dict[str, int] = {}
    fully, partly, none_ = [], [], []

    for path in files:
        target = src / path
        text = target.read_text(encoding='utf-8', errors='replace')
        out, pos = [], 0
        n_res = n_keep = 0

        for m in CONFLICT_RE.finditer(text):
            out.append(text[pos:m.start()])
            pos = m.end()
            ours = m.group('ours').splitlines(keepends=True)
            base = m.group('base').splitlines(keepends=True)
            theirs = m.group('theirs').splitlines(keepends=True)

            merged, why = resolve_block(ours, base, theirs)
            if merged is None:
                reasons[why.split(':')[0]] = reasons.get(why.split(':')[0], 0) + 1
                out.append(m.group(0))       # keep the conflict for a human
                n_keep += 1
            else:
                out.append(''.join(merged))
                n_res += 1

        out.append(text[pos:])
        stats['resolved'] += n_res
        stats['kept'] += n_keep

        if n_res and not n_keep:
            fully.append(path)
        elif n_res:
            partly.append(path)
        else:
            none_.append(path)

        if args.apply and n_res:
            target.write_text(''.join(out), encoding='utf-8')
            if not n_keep:
                git(src, 'add', '--', path, check=False)
        if args.show_diff and n_res:
            print(f'--- {path}: {n_res} resolved, {n_keep} kept ---')

    print(f'=== conflict blocks ===')
    print(f'  resolved automatically : {stats["resolved"]}')
    print(f'  kept for a human       : {stats["kept"]}')
    print()
    print(f'=== files ===')
    print(f'  fully resolved (staged): {len(fully)}')
    print(f'  partially resolved     : {len(partly)}')
    print(f'  untouched              : {len(none_)}')
    print()
    print('=== why blocks were kept ===')
    for r, n in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print(f'  {n:3d}  {r}')

    if not args.apply:
        print('\nDry run -- nothing written. Re-run with --apply to write and '
              'stage fully resolved files.')
    else:
        print('\nFully resolved files were staged. Files with remaining conflict '
              'markers need\nmanual work; every kept block is listed by reason above.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
