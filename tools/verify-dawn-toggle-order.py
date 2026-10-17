"""Check Dawn's enum/table index invariant in an explicitly supplied source tree.

This reads source only; it does not compile or modify CI.
"""

import argparse
import json
import re
from pathlib import Path


def inspect_order(root):
    header = (root / 'src/dawn/native/Toggles.h').read_text()
    source = (root / 'src/dawn/native/Toggles.cpp').read_text()
    enum_body = header.split('enum class Toggle', 1)[1].split('{', 1)[1]
    enum_body = enum_body.split('EnumCount,', 1)[0]
    enum_body = re.sub(r'/\*.*?\*/|//[^\n]*', '', enum_body, flags=re.S)
    names = re.findall(r'^\s*(\w+)\s*,', enum_body, re.M)
    table = source.split('kToggleNameAndInfoList = {{', 1)[1].split('}};', 1)[0]
    entries = re.findall(r'\{Toggle::(\w+)\s*,', table)
    mismatches = [
        {'index': index, 'enum': enum_name, 'table': table_name}
        for index, (enum_name, table_name) in enumerate(zip(names, entries))
        if enum_name != table_name
    ]
    return {
        'enumCount': len(names),
        'tableCount': len(entries),
        'mismatches': mismatches,
        'pass': bool(names) and len(names) == len(entries) and not mismatches,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dawn_root', type=Path, help='Effective Dawn source directory')
    args = parser.parse_args()
    try:
        result = inspect_order(args.dawn_root)
    except (OSError, IndexError) as error:
        parser.error(f'Cannot inspect Dawn sources: {error}')
    print(json.dumps(result))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
