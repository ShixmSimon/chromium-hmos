"""Check the registration invariant in a CI-built ELF before device installation.

Requires its unstripped ELF for the table symbol. An optional packaged engine
must have the same Build ID and registration bytes. This never compiles code.
"""

import argparse
import json
import mmap
import re
import struct
import subprocess
from pathlib import Path


def find_table(path):
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[:6] != b'\x7fELF\x02\x01':
            raise ValueError('Expected a little-endian ELF64')
        section_offset = struct.unpack_from('<Q', data, 40)[0]
        section_size, section_count = struct.unpack_from('<HH', data, 58)
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, section_offset + i * section_size)
                    for i in range(section_count)]
        matches = []
        for section in sections:
            if section[1] != 2:  # SHT_SYMTAB
                continue
            strings = sections[section[6]]
            for offset in range(section[4], section[4] + section[5], section[9]):
                name, info, _, _, address, size = struct.unpack_from('<IBBHQQ', data, offset)
                if info & 15 != 1:  # STT_OBJECT
                    continue
                start = strings[4] + name
                end = data.find(b'\0', start, strings[4] + strings[5])
                if b'kToggleNameAndInfoList' in data[start:end]:
                    matches.append((address, size))
        if len(matches) != 1:
            raise ValueError(f'Expected one registration symbol, found {len(matches)}')
        return matches[0]


def read_entries(path, address, count):
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[:6] != b'\x7fELF\x02\x01':
            raise ValueError('Expected a little-endian ELF64')
        headers = struct.unpack_from('<Q', data, 32)[0]
        size, number = struct.unpack_from('<HH', data, 54)
        for i in range(number):
            pos = headers + i * size
            if struct.unpack_from('<I', data, pos)[0] != 1:  # PT_LOAD
                continue
            offset, virtual = struct.unpack_from('<QQ', data, pos + 8)
            length = struct.unpack_from('<Q', data, pos + 32)[0]
            if virtual <= address and address + count * 40 <= virtual + length:
                start = offset + address - virtual
                return [struct.unpack_from('<I', data, start + i * 40)[0] for i in range(count)]
        raise ValueError('Registration table does not fit a file-backed segment')


def build_id(path):
    result = subprocess.run(['readelf', '-n', str(path)], check=True, capture_output=True, text=True)
    match = re.search(r'Build ID: ([0-9a-f]+)', result.stdout)
    if not match:
        raise ValueError(f'No Build ID in {path}')
    return match[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('symbols', type=Path, help='CI unstripped library')
    parser.add_argument('--engine', type=Path, help='Library extracted from the actual HAR/HAP')
    parser.add_argument('--expected-count', type=int, required=True, help='Count from effective source')
    args = parser.parse_args()
    try:
        address, size = find_table(args.symbols)
        # ELF64 ToggleEnumAndInfo: enum32, padding, three pointers, stage32, padding.
        if size % 40 or args.expected_count <= 0:
            raise ValueError('Unexpected table ABI/count; inspect the C++ layout before proceeding')
        count = size // 40
        entries = read_entries(args.symbols, address, count)
        mismatches = [{'index': i, 'enum': value} for i, value in enumerate(entries) if i != value]
        result = {'entries': count, 'expectedCount': args.expected_count, 'mismatches': mismatches,
                  'pass': count == args.expected_count and not mismatches}
        if args.engine:
            result['buildId'] = build_id(args.symbols)
            result['packagedBuildId'] = build_id(args.engine)
            result['packagedTableMatches'] = entries == read_entries(args.engine, address, count)
            result['pass'] = bool(result['pass'] and result['packagedTableMatches'] and
                                  result['buildId'] == result['packagedBuildId'])
    except (OSError, ValueError, struct.error, subprocess.SubprocessError) as error:
        parser.error(str(error))
    print(json.dumps(result))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
