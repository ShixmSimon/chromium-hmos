import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('rules', Path(__file__).parents[1] / 'build-phishing-rules.py')
rules = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rules)
NOW = 1791700000000
COMMIT = 'a' * 40


class SnapshotTests(unittest.TestCase):
    def test_preserves_url_scope_and_provenance(self):
        raw = b'https://example.com/login?a=1\nhttps://example.com/login?a=2\nhttps://example.com/login?a=1\n'
        encoded, omitted = rules.build(raw, COMMIT, NOW - 1000, NOW)
        result = json.loads(encoded)
        self.assertEqual(len(result['urls']), 2)
        self.assertEqual(omitted, 1)
        self.assertEqual(result['sourceCommit'], COMMIT)
        self.assertEqual(result['generatedAt'], NOW - 1000)

    def test_does_not_refresh_old_or_future_feed(self):
        for timestamp in [NOW - rules.MAX_AGE_MS, NOW + 1000]:
            with self.assertRaises(ValueError):
                rules.build(b'https://example.com/login', COMMIT, timestamp, NOW)

    def test_private_and_credential_urls_excluded(self):
        for url in ['http://localhost/', 'http://127.0.0.1/', 'http://10.1.2.3/',
                    'http://[::ffff:127.0.0.1]/', 'http://host.local/',
                    'http://user:pass@example.com/', 'http://0x7f000001/',
                    'http://2130706433/', 'file:///secret', 'http://example.com:99999/']:
            self.assertFalse(rules.public_url(url), url)

    def test_empty_and_oversized_feed_rejected(self):
        for raw in [b'', b'# comment\nhttp://localhost/', b'a' * (rules.MAX_BYTES + 1)]:
            with self.assertRaises(ValueError):
                rules.build(raw, COMMIT, NOW, NOW)

    def test_invalid_metadata_and_invalid_entries_rejected(self):
        encoded, _ = rules.build(b'https://example.com/login', COMMIT, NOW, NOW)
        for key, value in [('format', 2), ('license', 'unknown'), ('sourceCommit', 'main'),
                           ('expiresAt', NOW + rules.MAX_AGE_MS + 1), ('generatedAt', float('nan')),
                           ('urls', ['http://localhost/'])]:
            bundle = json.loads(encoded)
            bundle[key] = value
            with self.assertRaises(ValueError):
                rules.validate(bundle, NOW)


if __name__ == '__main__':
    unittest.main()
