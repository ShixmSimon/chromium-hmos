#!/usr/bin/env python3
"""Sign/verify phishing updates offline. Keys and endpoints are product-owned.

Signing and verification require cryptography>=45,<47. Config validation uses
only the Python standard library. No command contacts a rule or browsing URL.
"""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
from urllib.parse import urlsplit

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('snapshot', Path(__file__).with_name('build-phishing-rules.py'))
snapshot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snapshot)
MAX_MANIFEST = 16384


def encode(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()


def endpoint(value):
    if not isinstance(value, str) or not snapshot.public_url(value):
        raise ValueError('invalid endpoint')
    parsed = urlsplit(value)
    if parsed.scheme != 'https' or parsed.query or parsed.fragment or '?' in value or '#' in value:
        raise ValueError('endpoint must be HTTPS without query or fragment')
    return parsed.scheme, parsed.hostname.lower(), parsed.port or 443


def integer(value, minimum, maximum):
    return type(value) is int and minimum <= value <= maximum


def validate_config(config):
    if not isinstance(config, dict) or type(config.get('format')) is not int or config['format'] != 1:
        raise ValueError('invalid config format')
    endpoint(config.get('manifestUrl'))
    if not integer(config.get('minimumSequence'), 0, 2147483647):
        raise ValueError('invalid minimum sequence')
    keys = config.get('keys')
    if not isinstance(keys, dict) or not 1 <= len(keys) <= 4:
        raise ValueError('invalid key set')
    for key_id, key in keys.items():
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', key_id) or not isinstance(key, str):
            raise ValueError('invalid key')
        if len(base64.b64decode(key, validate=True)) != 32:
            raise ValueError('invalid key length')
    if len(encode(config)) > MAX_MANIFEST:
        raise ValueError('config too large')
    return config


def metadata(raw, sequence, rules_url, now):
    if len(raw) > snapshot.MAX_BYTES or not integer(sequence, 1, 2147483647):
        raise ValueError('invalid size or sequence')
    data = snapshot.validate(json.loads(raw), now)
    endpoint(rules_url)
    if data['version'] != data['sourceCommit']:
        raise ValueError('version must be the pinned source commit')
    return dict(format=1, sequence=sequence, rulesUrl=rules_url,
                size=len(raw), sha256=hashlib.sha256(raw).hexdigest(),
                count=len(data['urls']), version=data['version'],
                generatedAt=data['generatedAt'], expiresAt=data['expiresAt'])


def sign(raw, sequence, rules_url, key_id, private_key, now):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    if not isinstance(private_key, Ed25519PrivateKey) or not re.fullmatch(r'[A-Za-z0-9_-]{1,32}', key_id):
        raise ValueError('expected Ed25519 signing key')
    signed = encode(metadata(raw, sequence, rules_url, now))
    return encode(dict(format=1, keyId=key_id,
                       signed=base64.b64encode(signed).decode(),
                       signature=base64.b64encode(private_key.sign(signed)).decode()))


def verify(manifest, raw, config, minimum_sequence, now):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    validate_config(config)
    if len(manifest) > MAX_MANIFEST:
        raise ValueError('manifest too large')
    envelope = json.loads(manifest)
    if type(envelope.get('format')) is not int or envelope['format'] != 1:
        raise ValueError('invalid envelope format')
    key = config['keys'][envelope['keyId']]
    signed = base64.b64decode(envelope['signed'], validate=True)
    if len(signed) > 8192:
        raise ValueError('signed metadata too large')
    Ed25519PublicKey.from_public_bytes(base64.b64decode(key, validate=True)).verify(
        base64.b64decode(envelope['signature'], validate=True), signed)
    info = json.loads(signed)
    expected = metadata(raw, info['sequence'], info['rulesUrl'], now)
    if (not integer(info.get('format'), 1, 1) or
            info['sequence'] < max(minimum_sequence, config['minimumSequence']) or
            info != expected or endpoint(info['rulesUrl']) != endpoint(config['manifestUrl'])):
        raise ValueError('manifest metadata, origin or sequence mismatch')
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--validate-config', type=Path)
    parser.add_argument('--snapshot', type=Path)
    parser.add_argument('--sequence', type=int)
    parser.add_argument('--rules-url')
    parser.add_argument('--key-id')
    parser.add_argument('--private-key', type=Path)
    parser.add_argument('--manifest-output', type=Path)
    parser.add_argument('--config', type=Path)
    parser.add_argument('--verify-manifest', type=Path)
    args = parser.parse_args()
    try:
        if args.validate_config:
            raw = args.validate_config.read_bytes()
            if len(raw) > MAX_MANIFEST:
                raise ValueError('config too large')
            validate_config(json.loads(raw))
            print('Valid update configuration')
            return
        if not args.snapshot or not args.config:
            parser.error('provide --snapshot and trusted --config')
        config = validate_config(json.loads(args.config.read_bytes()))
        raw = args.snapshot.read_bytes()
        now = datetime.now(timezone.utc).timestamp() * 1000
        if args.verify_manifest:
            info = verify(args.verify_manifest.read_bytes(), raw, config, config['minimumSequence'], now)
            print(f"Verified sequence {info['sequence']}, {info['count']} rules")
            return
        if not all([args.sequence, args.rules_url, args.key_id, args.private_key, args.manifest_output]):
            parser.error('provide sequence, rules URL, key ID, private key and manifest output')
        from cryptography.hazmat.primitives.serialization import load_pem_private_key
        private = load_pem_private_key(args.private_key.read_bytes(), password=None)
        manifest = sign(raw, args.sequence, args.rules_url, args.key_id, private, now)
        verify(manifest, raw, config, config['minimumSequence'], now)
        args.manifest_output.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_output.write_bytes(manifest)
        print('Signed manifest written; publish the immutable payload before the manifest')
    except Exception as error:
        # Never print keys, feed contents or exception text that could contain them.
        parser.exit(1, f'Update artifact rejected: {type(error).__name__}\n')


if __name__ == '__main__':
    main()
