#!/usr/bin/env python3
"""Keep Depot experiments separate from the production branch allowlist."""
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('depot_selection', ROOT / 'scripts/select-depot-engine.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class DepotEngineSelectionTest(unittest.TestCase):
    def test_branch_dispatch_and_published_commit_dispatch(self):
        sha = 'a' * 40
        for ref in ('ci/depot-154', 'refs/heads/ci/depot-154', sha):
            with self.subTest(ref=ref):
                self.assertEqual(module.resolve_branch(ref, sha, 'ci/depot-154', sha),
                                 'ci/depot-154')

    def test_commit_dispatch_rejects_wrong_branch_stale_head_and_mismatched_sha(self):
        sha = 'a' * 40
        for ref, commit, remote in (
                ('refs/heads/hmos-154-adapter', sha, sha),
                ('refs/heads/ci/depot-155', sha, sha),
                (sha, sha, 'b' * 40),
                (sha, 'b' * 40, sha),
                ('', sha, sha)):
            with self.subTest(ref=ref, commit=commit, remote=remote), self.assertRaises(ValueError):
                module.resolve_branch(ref, commit, 'ci/depot-154', remote)

    def test_engines_use_distinct_disks_and_existing_pins(self):
        for major in (154, 155):
            with self.subTest(major=major):
                config = module.select(f'ci/depot-{major}')
                original = module.selection.select(f'hmos-{major}-adapter')
                self.assertEqual({key: config[key] for key in original}, original)
                self.assertEqual(config['DEPOT_ENGINE_DISK'],
                                 f'chromium-hmos-depot-{major}-native-v1')

    def test_production_and_other_refs_cannot_run_trial(self):
        for ref in ('', 'main', 'hmos-154-adapter', 'hmos-155-adapter',
                    'ci/depot-156', 'ci/depot-154/extra', '../ci/depot-154',
                    'refs/heads/ci/depot-154'):
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                module.select(ref)

    def test_trial_refs_cannot_select_a_production_runner_tree(self):
        for ref in ('ci/depot-154', 'ci/depot-155'):
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                module.selection.select(ref)


if __name__ == '__main__':
    unittest.main()
