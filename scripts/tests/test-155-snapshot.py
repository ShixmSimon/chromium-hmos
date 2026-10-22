#!/usr/bin/env python3
"""An isolated Git fixture protects completed objects during migration retries."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import os

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('snapshot', Path(__file__).resolve().parents[1] / 'apply-155-snapshot.py')
snapshot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snapshot)


class SnapshotRetryTest(unittest.TestCase):
    def test_only_changed_or_retired_inputs_get_new_timestamps(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            src = root / 'src'
            src.mkdir()
            def git(*args):
                return subprocess.check_output(['git', '-C', str(src), *args])
            git('init', '-q')
            for name in ('shared.h', 'feature.cc', 'retired.h'):
                (src / name).write_text('upstream\n')
            git('add', '.')
            git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'base')
            for name in ('shared.h', 'feature.cc', 'retired.h'):
                (src / name).write_text('adapted\n')
            first = root / 'first.patch'
            first.write_bytes(git('diff', '--binary', '--full-index', 'HEAD'))
            (src / 'feature.cc').write_text('fixed\n')
            (src / 'retired.h').write_text('upstream\n')
            second = root / 'second.patch'
            second.write_bytes(git('diff', '--binary', '--full-index', 'HEAD'))
            git('reset', '--hard', 'HEAD')
            snapshot.apply_snapshot(src, first)
            stamp = 1700000000000000000
            for name in ('shared.h', 'feature.cc', 'retired.h'):
                os.utime(src / name, ns=(stamp, stamp))
            snapshot.apply_snapshot(src, second)
            self.assertEqual((src / 'shared.h').stat().st_mtime_ns, stamp)
            self.assertNotEqual((src / 'feature.cc').stat().st_mtime_ns, stamp)
            self.assertNotEqual((src / 'retired.h').stat().st_mtime_ns, stamp)
            self.assertEqual((src / 'feature.cc').read_text(), 'fixed\n')
            self.assertEqual((src / 'retired.h').read_text(), 'upstream\n')


if __name__ == '__main__':
    unittest.main()
