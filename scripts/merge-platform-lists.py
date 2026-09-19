#!/usr/bin/env python3
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
"""Re-apply OHOS platform-list entries onto upstream's version of a line.

The most common conflict in a Chromium version upgrade is a platform list that
both sides edited: upstream restructured it (renaming `is_mac` to `is_apple`,
adding `is_fuchsia`, switching a macro name) while the adapter appended an OHOS
entry. Neither side can be taken whole -- `theirs` reverts the upstream
restructuring, `ours` drops OHOS.

What the adapter actually did is a single insertion:

    base    assert(is_win || is_mac || is_linux || is_chromeos || is_android)
    theirs  assert(is_win || is_mac || is_linux || is_chromeos || is_android || is_ohos)
                                                                             ^^^^^^^^^^^^

That insertion is re-applied to `ours`, anchored on the text that followed it
(here `)`), not on a character offset -- upstream's line has a different length
and different tokens, so an offset would land in the wrong place.

Continuation lines are joined before comparing, because the adapter often turns
a one-line condition into a `\\`-continued two-liner purely to fit the new
entry; comparing physical lines would report that as an unrelated rewrite.

Only insertions that mention OHOS are applied. Anything else is left
conflicted. GN and C++ syntax errors surface at build time rather than
silently, which is what makes this safe to automate; a wrong *semantic* choice
would not, which is why non-OHOS edits are never touched here.

Usage:
    scripts/merge-platform-lists.py --src SRC            # dry run
    scripts/merge-platform-lists.py --src SRC --apply
"""

from __future__ import annotations

import argparse
import difflib
import re
import subprocess
import sys
from pathlib import Path

CONFLICT_RE = re.compile(
    r'^<<<<<<< +.*\n(?P<ours>(?:.*\n)*?)'
    r'^\|\|\|\|\|\|\| +.*\n(?P<base>(?:.*\n)*?)'
    r'^=======\n(?P<theirs>(?:.*\n)*?)'
    r'^>>>>>>> +.*\n', re.M)

OHOS_RE = re.compile(r'OHOS|ohos|HarmonyOS')


def git(src: Path, *a: str, check: bool = True) -> str:
    r = subprocess.run(['git', '-C', str(src), *a], capture_output=True)
    if check and r.returncode != 0:
        raise RuntimeError(r.stderr.decode('utf-8', 'replace'))
    return r.stdout.decode('utf-8', 'replace')


def join_continuations(text: str) -> list[str]:
    """Join backslash-continued lines into single logical lines."""
    out: list[str] = []
    for line in text.splitlines():
        if out and out[-1].rstrip().endswith('\\'):
            out[-1] = out[-1].rstrip()[:-1].rstrip() + ' ' + line.strip()
        else:
            out.append(line)
    return out


def insertion_between(old: str, new: str) -> tuple[str, str] | None:
    """If `new` is `old` with one run of text inserted, return (text, suffix)."""
    sm = difflib.SequenceMatcher(None, old, new, autojunk=False)
    ops = [op for op in sm.get_opcodes() if op[0] != 'equal']
    if len(ops) != 1:
        return None
    tag, i1, i2, j1, j2 = ops[0]
    if tag != 'insert':
        return None
    return new[j1:j2], old[i1:]


def apply_insertion(line: str, inserted: str, suffix: str) -> str | None:
    """Insert `inserted` into `line` before the last occurrence of `suffix`."""
    if suffix == '':
        return line.rstrip() + inserted
    idx = line.rfind(suffix)
    if idx < 0:
        # Upstream dropped the trailing text the insertion was anchored on.
        return None
    return line[:idx] + inserted + line[idx:]


def merge_block(ours: str, base: str, theirs: str) -> tuple[str | None, str]:
    o, b, t = (join_continuations(x) for x in (ours, base, theirs))

    sm = difflib.SequenceMatcher(None, b, t, autojunk=False)
    ops = [op for op in sm.get_opcodes() if op[0] != 'equal']
    if len(ops) != 1:
        return None, 'adapter changed more than one region'
    tag, i1, i2, j1, j2 = ops[0]
    if tag != 'replace' or (i2 - i1) != 1 or (j2 - j1) != 1:
        return None, 'not a single-line edit'

    base_line, theirs_line = b[i1], t[j1]
    ins = insertion_between(base_line, theirs_line)
    if ins is None:
        return None, 'adapter rewrote the line rather than inserting into it'
    inserted, suffix = ins
    if not OHOS_RE.search(inserted):
        return None, 'inserted text does not mention OHOS'

    # Find the line in `ours` that corresponds to the one the adapter edited.
    #
    # A loose match is worse than no match. When upstream restructures a block
    # -- turning a platform list into a `use_aura` test, or moving a value to a
    # different variable -- the closest surviving line is often semantically
    # unrelated, and splicing an OHOS entry into it produces code that compiles
    # and is wrong. So: high similarity, same leading token, never a comment.
    matches = difflib.get_close_matches(base_line, o, n=1, cutoff=0.85)
    if not matches:
        return None, 'no closely matching line in upstream version'
    target = matches[0]

    def lead(s: str) -> str:
        s = s.strip()
        return s.split('(')[0].split()[0] if s.split() else ''

    if lead(target) != lead(base_line):
        return None, f'upstream line starts differently ({lead(target)!r} vs {lead(base_line)!r})'
    if re.match(r'\s*(//|/\*|#\s*endif\b.*//)', target):
        return None, 'corresponding line is a comment'
    if OHOS_RE.search(target):
        return None, 'upstream line already mentions OHOS'
    merged_line = apply_insertion(target, inserted, suffix)
    if merged_line is None:
        return None, f'anchor {suffix.strip()[:20]!r} absent from upstream line'

    result = [merged_line if l == target else l for l in o]
    return '\n'.join(result) + '\n', 'ok'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, type=Path)
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--show', action='store_true', help='print each merged line')
    args = ap.parse_args()
    src: Path = args.src

    files = [f for f in git(src, 'diff', '--name-only', '--diff-filter=U').split() if f]
    done = kept = 0
    reasons: dict[str, int] = {}
    cleared: list[str] = []
    samples: list[str] = []

    for path in files:
        p = src / path
        text = p.read_text(encoding='utf-8', errors='replace')
        out, pos, n_ok, n_keep = [], 0, 0, 0

        for m in CONFLICT_RE.finditer(text):
            out.append(text[pos:m.start()])
            pos = m.end()
            merged, why = merge_block(m.group('ours'), m.group('base'), m.group('theirs'))
            if merged is None:
                reasons[why] = reasons.get(why, 0) + 1
                out.append(m.group(0))
                n_keep += 1
            else:
                out.append(merged)
                n_ok += 1
                # Record the line that actually changed, not merely the first
                # line of the block -- reviewing the wrong line hides mistakes.
                before = set(join_continuations(m.group('ours')))
                changed = [l for l in join_continuations(merged) if l not in before]
                samples.append(f'{path}\n      {(changed[0] if changed else "(no line changed?)")[:110]}')
        out.append(text[pos:])

        done += n_ok
        kept += n_keep
        if n_ok and args.apply:
            p.write_text(''.join(out), encoding='utf-8')
            if not n_keep:
                git(src, 'add', '--', path, check=False)
                cleared.append(path)
        elif n_ok and not n_keep:
            cleared.append(path)

    print(f'blocks merged : {done}')
    print(f'blocks kept   : {kept}')
    print(f'files cleared : {len(cleared)}')
    print()
    print('=== why blocks were kept ===')
    for r, n in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print(f'  {n:3d}  {r}')
    if samples:
        print('\n=== sample merged lines ===')
        for s in samples:
            print(f'  {s}')
    if not args.apply:
        print('\nDry run -- nothing written.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
