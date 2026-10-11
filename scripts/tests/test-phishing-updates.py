import base64
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('updates', Path(__file__).parents[1] / 'phishing-update-tools.py')
updates = importlib.util.module_from_spec(spec)
spec.loader.exec_module(updates)
NOW = 1791700000000
# Test-only deterministic key. Never installed in the application configuration.
KEY = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
PUBLIC = base64.b64encode(KEY.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)).decode()
CONFIG = dict(format=1, manifestUrl='https://rules.example.org/manifest.json',
              minimumSequence=5, keys={'test': PUBLIC})
RAW, _ = updates.snapshot.build(b'https://example.com/login?a=1#one\nhttps://example.com/login?a=2\n',
                                'a' * 40, NOW - 1000, NOW)
MANIFEST = updates.sign(RAW, 10, 'https://rules.example.org/v10.json', 'test', KEY, NOW)


def changed_metadata(**fields):
    envelope = json.loads(MANIFEST)
    info = json.loads(base64.b64decode(envelope['signed']))
    info.update(fields)
    signed = updates.encode(info)
    envelope['signed'] = base64.b64encode(signed).decode()
    envelope['signature'] = base64.b64encode(KEY.sign(signed)).decode()
    return updates.encode(envelope)


def cases():
    yield 'valid', MANIFEST, True, True
    for name, values in [
        ('rollback', dict(sequence=4)), ('future', dict(generatedAt=NOW + 1)),
        ('expired', dict(expiresAt=NOW)),
        ('lifetime', dict(expiresAt=NOW + updates.snapshot.MAX_AGE_MS)),
        ('http', dict(rulesUrl='http://rules.example.org/v10.json')),
        ('foreign_origin', dict(rulesUrl='https://other.example.org/v10.json')),
        ('private', dict(rulesUrl='https://127.0.0.1/v10.json')),
        ('credentials', dict(rulesUrl='https://user:pass@rules.example.org/v10.json')),
        ('oversize', dict(size=updates.snapshot.MAX_BYTES + 1)),
        ('too_many', dict(count=200001)), ('wrong_version', dict(version='main')),
    ]:
        yield name, changed_metadata(**values), False, False
    for name, values in [('digest', dict(sha256='b' * 64)), ('count', dict(count=3)),
                         ('length', dict(size=len(RAW) - 1))]:
        yield name, changed_metadata(**values), True, False
    envelope = json.loads(MANIFEST)
    envelope['keyId'] = 'unknown'
    yield 'unknown_key', updates.encode(envelope), False, False
    envelope = json.loads(MANIFEST)
    envelope['signature'] = base64.b64encode(bytes(64)).decode()
    yield 'bad_signature', updates.encode(envelope), False, False


class UpdateTests(unittest.TestCase):
    def test_valid_signed_snapshot(self):
        self.assertEqual(updates.verify(MANIFEST, RAW, CONFIG, 5, NOW)['sequence'], 10)

    def test_attack_matrix(self):
        for name, manifest, _, payload_valid in cases():
            if payload_valid:
                continue
            with self.subTest(name=name), self.assertRaises((ValueError, KeyError, InvalidSignature)):
                updates.verify(manifest, RAW, CONFIG, 5, NOW)

    def test_payload_tampering(self):
        with self.assertRaises(ValueError):
            updates.verify(MANIFEST, RAW.replace(b'login', b'other'), CONFIG, 5, NOW)

    def test_persisted_high_watermark(self):
        with self.assertRaises(ValueError):
            updates.verify(MANIFEST, RAW, CONFIG, 11, NOW)

    def test_key_rotation_requires_package_trust(self):
        key2 = Ed25519PrivateKey.generate()
        signed = updates.sign(RAW, 11, 'https://rules.example.org/v11.json', 'next', key2, NOW)
        with self.assertRaises(KeyError):
            updates.verify(signed, RAW, CONFIG, 5, NOW)
        config = dict(CONFIG, keys=dict(CONFIG['keys'], next=base64.b64encode(
            key2.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)).decode()))
        self.assertEqual(updates.verify(signed, RAW, config, 10, NOW)['sequence'], 11)

    def test_expiry_not_extended_by_signing(self):
        with self.assertRaises(ValueError):
            updates.sign(RAW, 12, 'https://rules.example.org/v12.json', 'test', KEY,
                         NOW + updates.snapshot.MAX_AGE_MS)

    def test_invalid_config(self):
        for patch in [dict(manifestUrl='http://rules.example.org/manifest.json'),
                      dict(manifestUrl='https://localhost/'), dict(keys={}),
                      dict(minimumSequence=True), dict(keys={'test': 'AA=='}),
                      dict(manifestUrl='https://rules.example.org/?token=a')]:
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                updates.validate_config(dict(CONFIG, **patch))


def write_native_fixtures(path):
    path.mkdir(parents=True, exist_ok=True)
    (path / 'config.json').write_bytes(updates.encode(CONFIG))
    (path / 'rules.json').write_bytes(RAW)
    matrix = []
    for name, manifest, valid_manifest, valid_payload in cases():
        (path / f'{name}.json').write_bytes(manifest)
        matrix.append(dict(name=name, manifest=valid_manifest, payload=valid_payload))
    (path / 'cases.json').write_bytes(updates.encode(dict(now=NOW, cases=matrix)))
    print(f'Wrote {len(matrix)} native signature/metadata cases')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--native-fixtures':
        write_native_fixtures(Path(sys.argv[2]))
    else:
        unittest.main()
