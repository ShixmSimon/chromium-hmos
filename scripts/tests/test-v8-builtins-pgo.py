#!/usr/bin/env python3
"""Verify profile revision checks, caching and narrowly scoped GN updates."""
import importlib.util
import json
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('pgo', Path(__file__).resolve().parents[1] / 'configure-v8-builtins-pgo.py')
pgo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pgo)
REVISION = 'b3d6849efaf11696198e15b32290e6d6b0d540c1'
META = {'revision': REVISION, 'version': '15.4.80.20', 'profile': ['x64']}
DATA = b'block_hint,Test,1,2,0\nbuiltin_hash,Test,42\n'


class BuiltinsPgoTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.src = Path(self.tmp.name)
        (self.src / 'v8/include').mkdir(parents=True)
        (self.src / 'DEPS').write_text(f"'v8_revision': '{REVISION}',\n")
        (self.src / 'v8/include/v8-version.h').write_text(
            '#define V8_MAJOR_VERSION 15\n#define V8_MINOR_VERSION 4\n'
            '#define V8_BUILD_NUMBER 80\n#define V8_PATCH_LEVEL 20\n')

    def test_only_requested_gn_argument_changes(self):
        original = '# existing runner options\ntarget_cpu = "arm64"\noptimize_for_size = true\nv8_enable_builtins_optimization = false\n'
        updated = pgo.setting(original, 'v8_enable_builtins_optimization', 'true')
        self.assertEqual(updated, original.replace('optimization = false', 'optimization = true'))
        self.assertEqual(pgo.setting(updated, 'v8_enable_builtins_optimization', 'true'), updated)
        self.assertIn('v8_builtins_profiling_log_file = "default"', pgo.setting(updated, 'v8_builtins_profiling_log_file', '"default"'))

    def test_duplicate_option_rejected(self):
        with self.assertRaises(ValueError):
            pgo.setting('flag = false\nflag = true\n', 'flag', 'true')

    def test_download_and_cache_integrity(self):
        with patch.object(pgo.subprocess, 'check_output', return_value=REVISION + '\n'), patch.object(pgo, 'fetch', side_effect=[json.dumps(META).encode(), DATA]) as fetch:
            result = pgo.prepare_profile(self.src)
            self.assertEqual(fetch.call_count, 2)
            self.assertEqual(result['revision'], REVISION)
        profile = self.src / 'v8' / pgo.PROFILE
        before = profile.stat().st_mtime_ns
        with patch.object(pgo.subprocess, 'check_output', return_value=REVISION), patch.object(pgo, 'fetch', return_value=json.dumps(META).encode()) as fetch:
            pgo.prepare_profile(self.src)
            self.assertEqual(fetch.call_count, 1)
        self.assertEqual(profile.stat().st_mtime_ns, before)
        self.assertFalse(profile.with_name('meta.json').exists())
        profile.write_bytes(b'corrupt')
        with patch.object(pgo.subprocess, 'check_output', return_value=REVISION), patch.object(pgo, 'fetch', side_effect=[json.dumps(META).encode(), DATA]):
            pgo.prepare_profile(self.src)
        self.assertEqual(profile.read_bytes(), DATA)

    def test_wrong_profile_revision_or_version_rejected(self):
        for meta in [dict(META, revision='0' * 40), dict(META, version='15.4.80.21'), dict(META, profile=['x86'])]:
            with self.subTest(meta=meta), patch.object(pgo.subprocess, 'check_output', return_value=REVISION), patch.object(pgo, 'fetch', return_value=json.dumps(meta).encode()):
                with self.assertRaises(ValueError):
                    pgo.prepare_profile(self.src)
        self.assertFalse((self.src / 'v8' / pgo.PROFILE).exists())

    def test_checkout_mismatch_and_invalid_download_rejected(self):
        with patch.object(pgo.subprocess, 'check_output', return_value='0' * 40), patch.object(pgo, 'fetch') as fetch:
            with self.assertRaises(ValueError):
                pgo.prepare_profile(self.src)
            fetch.assert_not_called()
        with patch.object(pgo.subprocess, 'check_output', return_value=REVISION), patch.object(pgo, 'fetch', side_effect=[json.dumps(META).encode(), b'<Error>missing</Error>']):
            with self.assertRaises(ValueError):
                pgo.prepare_profile(self.src)


if __name__ == '__main__':
    unittest.main()
