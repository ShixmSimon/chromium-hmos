#!/usr/bin/env python3
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
"""Find functions that fall off the end when no platform branch matches.

Chromium writes plenty of functions shaped like this:

    Foo* Foo::GetInstance() {
      ...
    #if BUILDFLAG(IS_CHROMEOS)
      return chromeos_thing;
    #elif BUILDFLAG(IS_LINUX) && BUILDFLAG(USE_DBUS)
      return linux_thing;
    #endif
    }

On every platform upstream supports, one arm matches and the function returns.
On a platform upstream does not list -- OHOS -- the whole chain preprocesses
away and control reaches the closing brace of a non-void function.

The build disables `-Wreturn-type`, so the compiler says nothing. It does
enable `-fsanitize=return -fsanitize-trap=return`, so the same defect becomes a
`brk` instruction and the process dies with SIGTRAP at whatever point first
calls the function. That is an expensive way to find out: one rebuild and one
device install per occurrence.

This reads the source instead. For each `#endif` immediately followed by a
closing brace at column 0, it walks back to the opening `#if`, and reports the
site when that chain has no `#else` and the enclosing function does not return
void. Being a text heuristic it will produce false positives -- a chain whose
arms all `NOTREACHED()`, a function returning a type it cannot recognise -- so
each hit needs a look. The value is that the list is short and arrives in
seconds rather than one crash at a time.

Usage:
    scripts/find-missing-returns.py --src /path/to/chromium/src
    scripts/find-missing-returns.py --src ... --paths chrome ui content
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# `#endif` followed by the closing brace of a function, i.e. the preprocessor
# chain is the last thing in the body.
TAIL_RE = re.compile(r'^#\s*endif.*\n\}', re.M)

IF_RE = re.compile(r'^#\s*if')
ELSE_RE = re.compile(r'^#\s*else')
ENDIF_RE = re.compile(r'^#\s*endif')

# A function definition line: a type, a name, an open paren. Deliberately loose;
# the check that matters is whether it starts with void.
SIGNATURE_RE = re.compile(r'^([A-Za-z_][\w:<>,\s\*&]*?)\s+([\w:~]+)\s*\(')

VOID_RE = re.compile(r'^\s*(?:static\s+|inline\s+|const\s+)*void\b')

SKIP_DIRS = {'test', 'tests', 'testing', 'third_party'}


def enclosing_if(lines: list[str], endif_idx: int) -> tuple[int, bool]:
    """Walk back from an #endif to its #if. Returns (index, chain_has_else)."""
    depth = 0
    has_else = False
    for i in range(endif_idx - 1, -1, -1):
        l = lines[i]
        if ENDIF_RE.match(l):
            depth += 1
        elif IF_RE.match(l):
            if depth == 0:
                return i, has_else
            depth -= 1
        elif ELSE_RE.match(l) and depth == 0:
            has_else = True
    return -1, has_else


def function_signature(lines: list[str], if_idx: int) -> str | None:
    """Find the signature of the function containing this directive.

    Scans back for the nearest line starting at column 0 that looks like a
    definition. Good enough: Chromium formats definitions that way.
    """
    for i in range(if_idx - 1, max(0, if_idx - 400), -1):
        l = lines[i]
        if not l or l[0].isspace() or l.startswith(('#', '/', '*', '}')):
            continue
        if SIGNATURE_RE.match(l):
            # Join the preceding line too: the return type is often on its own.
            prev = lines[i - 1] if i > 0 else ''
            if prev and not prev[0].isspace() and not prev.startswith(('#', '/', '}')):
                return f'{prev.strip()} {l.strip()}'
            return l.strip()
        if l.endswith((';', ')')) or l.startswith(('namespace', 'class', 'struct')):
            return None
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, type=Path)
    ap.add_argument('--paths', nargs='*', default=['chrome', 'ui', 'content',
                                                   'components', 'services'],
                    help='subtrees to scan')
    ap.add_argument('--all-files', action='store_true',
                    help='include test files, normally skipped')
    args = ap.parse_args()

    files: list[Path] = []
    for p in args.paths:
        root = args.src / p
        if root.is_dir():
            files += list(root.rglob('*.cc'))
    if not args.all_files:
        files = [f for f in files
                 if not SKIP_DIRS & set(f.parts)
                 and 'unittest' not in f.name and 'browsertest' not in f.name]

    print(f'scanning {len(files)} files\n')

    found = 0
    for f in files:
        try:
            text = f.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        if '#endif' not in text:
            continue
        lines = text.split('\n')

        for m in TAIL_RE.finditer(text):
            endif_line = text[:m.start()].count('\n')
            if_line, has_else = enclosing_if(lines, endif_line)
            if if_line < 0 or has_else:
                continue
            sig = function_signature(lines, if_line)
            if not sig or VOID_RE.match(sig):
                continue

            found += 1
            rel = f.relative_to(args.src).as_posix()
            print(f'!! {rel}:{if_line + 1}')
            print(f'      {sig[:100]}')
            print(f'      chain: {lines[if_line].strip()[:88]}')
            print(f'      no #else, so a platform matching no arm falls off the end')
            print()

    print(f'summary: {found} functions whose platform chain has no #else')
    if found:
        print('\nEach is a candidate, not a confirmed defect: the arms may all diverge\n'
              '(NOTREACHED, CHECK) or the signature may have been misread. Confirm by\n'
              'checking whether any arm actually matches the OHOS build.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
