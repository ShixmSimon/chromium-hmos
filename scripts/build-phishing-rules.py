#!/usr/bin/env python3
"""Package a reviewed Phishing.Database snapshot; never fetch visited URLs.

The input must be an active URL list from a pinned source commit. Its source
timestamp is explicit so rebuilding an old feed cannot silently refresh it.
This tool prepares a package asset, not a signed online update.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
from pathlib import Path
import re
from urllib.parse import urlsplit

MAX_BYTES = 16 * 1024 * 1024
MAX_RULES = 200000
MAX_AGE_MS = 7 * 24 * 60 * 60 * 1000


def public_url(value):
    if not isinstance(value, str) or not value or len(value.encode('utf-8')) > 4096:
        return False
    if any(c.isspace() or ord(c) < 32 for c in value):
        return False
    try:
        parsed = urlsplit(value)
        host = (parsed.hostname or '').rstrip('.').lower()
        _ = parsed.port  # reject malformed ports
        if parsed.scheme not in ('http', 'https') or parsed.username is not None or parsed.password is not None:
            return False
        if not host or '\\' in value or '%' in host:
            return False
        try:
            address = ipaddress.ip_address(host)
            if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
                address = address.ipv4_mapped
            return address.is_global
        except ValueError:
            # Avoid alternate numeric IP spellings that GURL canonicalizes.
            if re.fullmatch(r'[0-9xa-f.]+', host) and not re.search('[g-z]', host):
                return False
            return '.' in host and not host.endswith(('.localhost', '.local', '.internal', '.home.arpa'))
    except (ValueError, UnicodeError):
        return False


def validate(bundle, now_ms):
    if not isinstance(bundle, dict) or bundle.get('format') != 1 or bundle.get('source') != 'Phishing.Database':
        raise ValueError('invalid format/source')
    version = bundle.get('version')
    if not isinstance(version, str) or not 1 <= len(version) <= 80:
        raise ValueError('invalid version')
    generated, expires = bundle.get('generatedAt'), bundle.get('expiresAt')
    if type(generated) not in (int, float) or type(expires) not in (int, float):
        raise ValueError('invalid timestamps')
    if not 0 <= generated <= now_ms < expires or not 0 < expires - generated <= MAX_AGE_MS:
        raise ValueError('future, stale or excessive snapshot lifetime')
    urls = bundle.get('urls')
    if not isinstance(urls, list) or not 0 < len(urls) <= MAX_RULES or not all(map(public_url, urls)):
        raise ValueError('invalid rule entries')
    if bundle.get('license') != 'MIT' or not re.fullmatch('[0-9a-f]{40}', bundle.get('sourceCommit', '')):
        raise ValueError('missing pinned source and license')
    if not re.fullmatch('[0-9a-f]{64}', bundle.get('sourceSha256', '')):
        raise ValueError('missing input digest')
    return bundle


def build(raw, source_commit, generated_ms, now_ms):
    if len(raw) > MAX_BYTES or not re.fullmatch('[0-9a-f]{40}', source_commit):
        raise ValueError('invalid input size or source commit')
    lines = raw.decode('utf-8-sig').splitlines()
    candidates = [line.strip() for line in lines if line.strip() and not line.lstrip().startswith('#')]
    urls = sorted(set(filter(public_url, candidates)))
    result = dict(format=1, source='Phishing.Database', license='MIT',
                  version=source_commit, sourceCommit=source_commit,
                  sourceSha256=hashlib.sha256(raw).hexdigest(), generatedAt=generated_ms,
                  expiresAt=generated_ms + MAX_AGE_MS, urls=urls)
    validate(result, now_ms)
    encoded = (json.dumps(result, ensure_ascii=True, separators=(',', ':')) + '\n').encode()
    if len(encoded) > MAX_BYTES:
        raise ValueError('output exceeds size limit')
    return encoded, len(candidates) - len(urls)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--source-commit')
    parser.add_argument('--source-time', help='UTC ISO timestamp of source snapshot, not packaging time')
    parser.add_argument('--expected-sha256')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--validate', type=Path, help='validate an existing package asset')
    args = parser.parse_args()
    now_ms = datetime.now(timezone.utc).timestamp() * 1000
    try:
        if args.validate:
            raw = args.validate.read_bytes()
            if len(raw) > MAX_BYTES:
                raise ValueError('snapshot too large')
            validate(json.loads(raw), now_ms)
            print('Valid package snapshot')
            return
        if not all([args.input, args.source_commit, args.source_time, args.expected_sha256, args.output]):
            parser.error('supply input, pinned commit, source time, expected digest and output')
        raw = args.input.read_bytes()
        if hashlib.sha256(raw).hexdigest() != args.expected_sha256:
            raise ValueError('input digest mismatch')
        timestamp = datetime.fromisoformat(args.source_time.replace('Z', '+00:00'))
        if timestamp.tzinfo is None:
            raise ValueError('source timestamp must include timezone')
        encoded, omitted = build(raw, args.source_commit, timestamp.timestamp() * 1000, now_ms)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(encoded)
        print(f'Wrote {len(encoded)} bytes; omitted {omitted} invalid/duplicate records. Review omission count before packaging.')
    except (ValueError, OSError, UnicodeError) as error:
        # No raw URL or feed content in diagnostics.
        parser.exit(1, f'Snapshot rejected: {type(error).__name__}\n')


if __name__ == '__main__':
    main()
