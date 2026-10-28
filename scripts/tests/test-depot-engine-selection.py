#!/usr/bin/env python3
"""Exercise published branch selection and checkout isolation with real Git refs."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('depot_selection', ROOT / 'scripts/select-depot-engine.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class DepotEngineSelectionTest(unittest.TestCase):
    def test_release_and_trial_branches_share_only_their_major_workspace(self):
        for major in (154, 155):
            original = module.selection.select(f'hmos-{major}-adapter')
            for branch in (f'hmos-{major}-adapter', f'ci/depot-{major}'):
                with self.subTest(branch=branch):
                    config = module.select(branch)
                    for key in ('CHROMIUM_MAJOR', 'CHROMIUM_VERSION', 'CHROMIUM_REVISION'):
                        self.assertEqual(config[key], original[key])
                    self.assertEqual(config['CHROMIUM_SRC'], f'/mnt/chromium-{major}/source/src')
                    self.assertEqual(config['DEPOT_WORKSPACE'], f'/mnt/chromium-{major}')

    def test_published_head_and_queued_ancestor(self):
        for branch in ('hmos-154-adapter', 'hmos-155-adapter', 'ci/depot-154', 'ci/depot-155'):
            with self.subTest(branch=branch):
                self.assertEqual(module.resolve_branch('a' * 40, branch, 'a' * 40), branch)
                self.assertEqual(module.resolve_branch('a' * 40, branch, 'b' * 40, True), branch)
                with self.assertRaises(ValueError):
                    module.resolve_branch('a' * 40, branch, 'b' * 40)

    def test_invalid_refs_and_shas_are_rejected(self):
        for ref in ('', 'main', 'build-results', 'ci/depot-156', 'hmos-156-adapter',
                    'ci/depot-154/extra', '../ci/depot-154', 'refs/heads/hmos-154-adapter'):
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                module.select(ref)
        for sha in ('', 'main', 'a' * 8):
            with self.subTest(sha=sha), self.assertRaises(ValueError):
                module.resolve_branch(sha, 'hmos-154-adapter', sha, True)

    def test_real_checkout_rejects_wrong_sha_and_other_branch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
            git('init', '-q')
            def commit(text):
                (root / 'input').write_text(text)
                git('add', 'input')
                git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', text)
                return git('rev-parse', 'HEAD')
            base = commit('base')
            first = commit('154-first')
            second = commit('154-second')
            git('update-ref', 'refs/remotes/origin/hmos-154-adapter', second)
            git('checkout', '-q', '--detach', base)
            other = commit('155-only')
            git('update-ref', 'refs/remotes/origin/hmos-155-adapter', other)
            git('checkout', '-q', '--detach', first)
            self.assertEqual(module.verify_checkout('hmos-154-adapter', first, root), 'hmos-154-adapter')
            with self.assertRaises(ValueError):
                module.verify_checkout('hmos-154-adapter', second, root)
            with self.assertRaises(ValueError):
                module.verify_checkout('hmos-155-adapter', first, root)

    def test_legacy_runner_selector_stays_restricted(self):
        for ref in ('ci/depot-154', 'ci/depot-155'):
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                module.selection.select(ref)


if __name__ == '__main__':
    unittest.main()
