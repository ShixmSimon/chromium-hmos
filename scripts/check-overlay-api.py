#!/usr/bin/env python3
# Copyright (c) 2026
# Licensed under the Apache License, Version 2.0.
"""Check overlay classes against a Chromium source tree's base classes.

The overlay adds new files to the Chromium tree, so `git apply` never reports a
conflict for them -- they only fail at compile time, after hours of build. Many
overlay classes derive from Chromium base classes and mark methods `override`,
and Chromium makes no stability promise about those internal APIs: across
versions virtual methods get renamed, resignatured, or removed outright.

This script front-runs the compiler. For every overlay class it resolves the
base class, walks the inheritance chain in the target tree, and reports any
`override` whose method name no longer exists anywhere in that chain.

A clean report does not prove the overlay compiles -- a method kept under the
same name but with changed parameters still breaks, and only the compiler sees
that. A dirty report is reliable in the other direction: every finding is a
real break that would have cost a full build cycle to discover.

Usage:
    scripts/check-overlay-api.py --chromium-src /root/chromium-154/src
    scripts/check-overlay-api.py --chromium-src ... --include-arkweb
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Export annotations sit between `class` and the class name. Chromium uses both
# the plain macro (GFX_EXPORT) and the parameterised one
# (COMPONENT_EXPORT(OZONE_BASE)); missing the latter silently breaks base-class
# resolution for a large fraction of the tree.
EXPORT = r'(?:(?:[A-Z][A-Z0-9_]*_EXPORT|COMPONENT_EXPORT\s*\([^)]*\))\s+)?'

# "class Foo final : public Bar, public Baz {"  and struct equivalents.
CLASS_RE = re.compile(
    r'^\s*(?:class|struct)\s+' + EXPORT + r'(\w+)\s+'
    r'(?:final\s+)?:\s*([^{]+)',
    re.MULTILINE,
)
BASE_RE = re.compile(r'(?:public|protected|private)\s+((?:\w+::)*\w+)')
INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"', re.MULTILINE)

# A declaration ending in `override`, possibly across wrapped lines. We only
# need the method name, so we look backwards from `override` to the last
# identifier followed by '('.
OVERRIDE_RE = re.compile(r'(~?\w+)\s*\([^;{]*?\)\s*(?:const\s*)?(?:noexcept\s*)?override')

# Any virtual or overriding declaration in a base class header.
VIRTUAL_RE = re.compile(
    r'(?:virtual\b[^;{]*?|\b)(~?\w+)\s*\([^;{]*?\)\s*'
    r'(?:const\s*)?(?:noexcept\s*)?(?:override|=\s*0|final|\{|;)'
)

# Base classes outside Chromium (platform SDK, our own overlay) are not
# interesting -- we only care about APIs upstream can change under us.
SKIP_BASES = {
    'enable_shared_from_this', 'true_type', 'false_type',
    'RefCounted', 'RefCountedThreadSafe', 'SupportsWeakPtr',
}


@dataclass
class OverlayClass:
    name: str
    bases: list[str]
    overrides: set[str]
    path: Path
    includes: list[str] = field(default_factory=list)


def strip_comments(text: str) -> str:
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def parse_header(path: Path) -> list[OverlayClass]:
    try:
        raw = path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    includes = INCLUDE_RE.findall(raw)
    text = strip_comments(raw)

    found: list[OverlayClass] = []
    matches = list(CLASS_RE.finditer(text))
    for i, m in enumerate(matches):
        name = m.group(1)
        bases = [b.split('::')[-1] for b in BASE_RE.findall(m.group(2))]
        bases = [b for b in bases if b not in SKIP_BASES]
        if not bases:
            continue
        # Body runs to the next class declaration, which is close enough:
        # overrides belong to the class they follow.
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        body = text[m.end():end]
        overrides = {mm.group(1) for mm in OVERRIDE_RE.finditer(body)}
        overrides = {o for o in overrides if not o.startswith('~')}
        if overrides:
            found.append(OverlayClass(name, bases, overrides, path, includes))
    return found


def header_candidates(src: Path, base: str, includes: list[str]) -> list[str]:
    """Find which of these includes actually declares `base`.

    Deriving a filename from the class name does not work: VSyncProvider lives
    in vsync_provider.h (not v_sync_provider.h) and AudioOutputStream lives in
    audio_io.h. So open the headers and look for the declaration itself.
    """
    decl = re.compile(r'^\s*(?:class|struct)\s+' + EXPORT + r'%s\b' % re.escape(base),
                      re.MULTILINE)
    hits = []
    for inc in includes:
        header = src / inc
        if not header.is_file():
            continue
        try:
            text = strip_comments(header.read_text(encoding='utf-8', errors='replace'))
        except OSError:
            continue
        if decl.search(text):
            hits.append(inc)
    return hits


def collect_chain_methods(src: Path, base: str, includes: list[str],
                          seen: set[str], depth: int = 0) -> tuple[set[str], list[str]]:
    """Walk up the inheritance chain, returning all method names and headers read."""
    if depth > 8 or base in seen:
        return set(), []
    seen.add(base)

    methods: set[str] = set()
    read: list[str] = []
    for rel in header_candidates(src, base, includes):
        header = src / rel
        if not header.is_file():
            continue
        raw = header.read_text(encoding='utf-8', errors='replace')
        text = strip_comments(raw)
        read.append(rel)
        methods |= {m.group(1) for m in VIRTUAL_RE.finditer(text)}

        # Recurse into this header's own bases, using its own include list plus
        # itself -- a parent class is often declared in the same header.
        parent_includes = INCLUDE_RE.findall(raw) + [rel]
        for m in CLASS_RE.finditer(text):
            if m.group(1) != base:
                continue
            for parent in BASE_RE.findall(m.group(2)):
                parent = parent.split('::')[-1]
                if parent in SKIP_BASES:
                    continue
                sub, sub_read = collect_chain_methods(
                    src, parent, parent_includes, seen, depth + 1)
                methods |= sub
                read += sub_read
    return methods, read


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--chromium-src', required=True, type=Path,
                    help='path to the target Chromium src/ tree')
    ap.add_argument('--overlay', type=Path, default=Path('overlay'),
                    help='overlay directory (default: overlay)')
    ap.add_argument('--include-arkweb', action='store_true',
                    help='also check overlay/arkweb (thousands of files, slow)')
    ap.add_argument('--quiet', action='store_true',
                    help='only print classes with problems')
    args = ap.parse_args()

    src: Path = args.chromium_src
    if not (src / 'chrome' / 'VERSION').is_file():
        print(f'error: {src} does not look like a Chromium src tree', file=sys.stderr)
        return 2
    version = dict(
        line.split('=', 1) for line in
        (src / 'chrome' / 'VERSION').read_text().strip().splitlines() if '=' in line)
    print('target Chromium: {}.{}.{}.{}'.format(
        version.get('MAJOR', '?'), version.get('MINOR', '?'),
        version.get('BUILD', '?'), version.get('PATCH', '?')))

    if not args.overlay.is_dir():
        print(f'error: overlay directory {args.overlay} not found', file=sys.stderr)
        return 2

    headers = sorted(args.overlay.rglob('*.h'))
    if not args.include_arkweb:
        headers = [h for h in headers if 'arkweb' not in h.parts]
    print(f'scanning {len(headers)} overlay headers\n')

    classes: list[OverlayClass] = []
    for h in headers:
        classes.extend(parse_header(h))

    total_missing = 0
    unresolved = 0
    checked = 0

    for cls in sorted(classes, key=lambda c: (str(c.path), c.name)):
        methods: set[str] = set()
        read: list[str] = []
        for base in cls.bases:
            m, r = collect_chain_methods(src, base, cls.includes, set())
            methods |= m
            read += r

        rel_path = cls.path.as_posix()
        if not read:
            # Base header not found in the target tree: either the base lives in
            # the overlay itself, or -- the case worth knowing about -- the file
            # moved or was deleted upstream.
            unresolved += 1
            if not args.quiet:
                print(f'?  {rel_path}: {cls.name} : {", ".join(cls.bases)}')
                print(f'      base header not found in target tree '
                      f'(overlay-internal base, or moved/removed upstream)')
            continue

        checked += 1
        missing = sorted(cls.overrides - methods)
        if missing:
            total_missing += len(missing)
            print(f'!! {rel_path}: {cls.name} : {", ".join(cls.bases)}')
            print(f'      chain: {", ".join(dict.fromkeys(read))}')
            for name in missing:
                print(f'      MISSING in base chain: {name}()')
        elif not args.quiet:
            print(f'ok {rel_path}: {cls.name} : {", ".join(cls.bases)} '
                  f'({len(cls.overrides)} overrides)')

    print()
    print(f'summary: {checked} classes verified against the target tree, '
          f'{unresolved} with unresolved bases, {total_missing} broken overrides')
    if total_missing:
        print('\nEach MISSING entry is a virtual method the overlay overrides that no '
              'longer exists\nanywhere in its base chain -- a guaranteed compile error. '
              'Signature changes are\nNOT detected here; only the compiler finds those.')
    return 1 if total_missing else 0


if __name__ == '__main__':
    sys.exit(main())
