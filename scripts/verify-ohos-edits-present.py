#!/usr/bin/env python3
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
"""Check that every OHOS edit in a previous adapter patch survived the upgrade.

A version upgrade re-applies the adapter patch onto a newer Chromium, and
conflicts under a heavily-refactored tree like `chrome/` are often resolved by
taking the upstream side. That is a reasonable default for feature trimming and
a silent disaster for platform support: the OHOS entry simply disappears from a
platform list, the tree still compiles, and the defect surfaces later as a
missing symbol at link time -- or, worse, as a `NOTREACHED()` at runtime.

The 154 upgrade hit both. Twenty-odd link errors traced back to dropped
`is_ohos` entries in `chrome/browser/ui/BUILD.gn`, and a device crash
(SIGTRAP in SetDMTokenStorageDelegate) traced back to one dropped
`BUILDFLAG(IS_OHOS)` in a `#elif` chain whose sibling edits in the same file
had survived.

Rather than wait for each one, this reads the old patch, extracts every added
line that mentions OHOS, and reports the ones that are not present in the new
tree. Matching is on normalised text, because upstream reformats: a condition
may be rewrapped across lines, so both the tree and the patch line are
whitespace-collapsed and line continuations are joined before comparing.

A reported line is not automatically a bug. Upstream may have deleted the
construct the adapter was patching, or restructured it so the edit is no longer
meaningful. Each one needs a look. What the script guarantees is that no
dropped edit goes unnoticed.

Usage:
    scripts/verify-ohos-edits-present.py --patch patches/chromium-150-harmonyos.patch \
                                         --src /path/to/chromium/src
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

OHOS_RE = re.compile(r'OHOS|ohos|HarmonyOS')
FILE_RE = re.compile(r'^\+\+\+ b/(.+)$')

# Lines that carry no OHOS decision of their own: a bare continuation, a closing
# comment echoing the condition above it, or a comment line. Reporting these
# produces noise that buries the real findings.
NOISE_RE = re.compile(
    r'^\s*$'
    r'|^\s*#\s*(endif|else)\b'
    r'|^\s*(//|#)\s*BUILDFLAG'
    r'|^\s*//'
)


def normalise(line: str) -> str:
    """Collapse whitespace and drop trailing continuations, for comparison."""
    s = line.strip()
    if s.endswith('\\'):
        s = s[:-1]
    return re.sub(r'\s+', ' ', s).strip()


def patch_additions(patch: Path) -> dict[str, list[str]]:
    """Map path -> added lines mentioning OHOS."""
    out: dict[str, list[str]] = defaultdict(list)
    path = None
    for line in patch.read_text(encoding='utf-8', errors='replace').split('\n'):
        m = FILE_RE.match(line)
        if m:
            path = m.group(1)
            continue
        if not path or not line.startswith('+') or line.startswith('+++'):
            continue
        body = line[1:]
        if not OHOS_RE.search(body) or NOISE_RE.match(body):
            continue
        out[path].append(body)
    return out


def joined_source(text: str) -> str:
    """Whole file, continuations joined, whitespace collapsed per line."""
    lines: list[str] = []
    for raw in text.split('\n'):
        if lines and lines[-1].rstrip().endswith('\\'):
            lines[-1] = lines[-1].rstrip()[:-1].rstrip() + ' ' + raw.strip()
        else:
            lines.append(raw)
    return '\n'.join(normalise(l) for l in lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--patch', required=True, type=Path)
    ap.add_argument('--src', required=True, type=Path)
    ap.add_argument('--show-present', action='store_true')
    args = ap.parse_args()

    additions = patch_additions(args.patch)
    total = sum(len(v) for v in additions.values())
    print(f'{total} OHOS-bearing added lines across {len(additions)} files in the patch\n')

    missing_files = 0
    missing_lines = 0
    gone = 0
    cache: dict[str, str] = {}

    for path in sorted(additions):
        target = args.src / path
        if not target.is_file():
            # Upstream deleted or moved the file. The edit cannot apply, and
            # whatever it provided has to come from somewhere else now.
            gone += 1
            print(f'?? {path}')
            print(f'      file no longer exists in the target tree '
                  f'({len(additions[path])} OHOS lines)')
            continue

        if path not in cache:
            cache[path] = joined_source(
                target.read_text(encoding='utf-8', errors='replace'))
        haystack = cache[path]

        absent = [l for l in additions[path] if normalise(l) not in haystack]
        if not absent:
            if args.show_present:
                print(f'ok {path} ({len(additions[path])} lines)')
            continue

        missing_files += 1
        missing_lines += len(absent)
        print(f'!! {path}')
        for l in absent:
            print(f'      MISSING: {l.strip()[:104]}')
        print()

    print(f'summary: {missing_lines} OHOS lines missing across {missing_files} files, '
          f'{gone} files gone from the tree')
    if missing_lines or gone:
        print('\nEach MISSING line is an OHOS edit the previous adapter made that is not\n'
              'in the new tree. Some are legitimately obsolete -- upstream may have\n'
              'removed what was being patched -- but a platform-list entry that quietly\n'
              'vanished compiles fine and fails at link time or, as with a dropped\n'
              '#elif branch, at runtime on the device.')
    return 1 if (missing_lines or gone) else 0


if __name__ == '__main__':
    sys.exit(main())
