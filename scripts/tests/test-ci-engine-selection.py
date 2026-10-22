#!/usr/bin/env python3
"""Protect the stable checkout from branch mixups and manual dispatches."""
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('selection', ROOT / 'scripts/select-ci-engine.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class EngineSelectionTest(unittest.TestCase):
    def test_release_paths_are_distinct_and_pinned(self):
        for major in (154, 155):
            config = module.select(f'hmos-{major}-adapter')
            self.assertEqual(config['CHROMIUM_SRC'], f'/root/chromium-{major}/src')
            self.assertEqual(config['CHROMIUM_MAJOR'], str(major))
            self.assertEqual(len(config['CHROMIUM_REVISION']), 40)

    def test_other_refs_cannot_fall_back_to_154(self):
        for ref in ('', 'main', 'hmos-154-dev', 'hmos-150-adapter', 'build-results',
                    'codex/phone-fork-renderers', 'build-5834ccdf'):
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                module.select(ref)


if __name__ == '__main__':
    unittest.main()
