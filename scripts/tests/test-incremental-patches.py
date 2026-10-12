#!/usr/bin/env python3
"""Exercise the CI patch helper against isolated Chromium and DEPS repositories."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[2]
BUILD_SCRIPT = (PROJECT / "scripts/ci-incremental-build.sh").read_text(encoding="utf-8")
HELPER = BUILD_SCRIPT.split("apply_incremental_patch() {", 1)[1].split(
    "# The counterpart to the above:", 1
)[0]
HELPER = "apply_incremental_patch() {" + HELPER
BASH = str(Path(shutil.which("git")).parents[1] / "bin/bash.exe") if os.name == "nt" else "bash"


class IncrementalPatchTest(unittest.TestCase):
    def run_command(self, *args, cwd):
        return subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True).stdout

    def test_default_and_deps_roots_first_repeat_and_upgrade(self):
        for deps in (False, True):
            with self.subTest(deps=deps), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                adapter = root / "adapter"
                chromium = root / "chromium"
                target = chromium / "third_party/angle" if deps else chromium
                for repo in (adapter, chromium, *([target] if deps else [])):
                    repo.mkdir(parents=True, exist_ok=True)
                    self.run_command("git", "init", "-q", cwd=repo)
                    self.run_command("git", "config", "core.autocrlf", "false", cwd=repo)
                (adapter / "patches").mkdir()
                fixture = target / "fixture.txt"
                original = "before\noriginal\nafter\n"
                fixture.write_text(original, encoding="utf-8")
                self.run_command("git", "add", "fixture.txt", cwd=target)
                # A same-named Chromium file must not be touched by a DEPS patch.
                if deps:
                    (chromium / "fixture.txt").write_text("chromium sentinel\n", encoding="utf-8")
                for revision in ("old", "new"):
                    fixture.write_text(f"before\n{revision}\nafter\n", encoding="utf-8")
                    diff = self.run_command("git", "diff", "--", "fixture.txt", cwd=target)
                    (adapter / "patches" / f"{revision}.txt").write_text(diff, encoding="utf-8")
                fixture.write_text(original, encoding="utf-8")
                patch = adapter / "patches/change.patch"
                for revision in ("old", "new"):
                    patch.write_bytes((adapter / "patches" / f"{revision}.txt").read_bytes())
                    self.run_command("git", "add", "patches/change.patch", cwd=adapter)
                    self.run_command(
                        "git", "-c", "user.name=Patch Test", "-c", "user.email=test@example.invalid",
                        "commit", "-qm", revision, cwd=adapter,
                    )
                    invocation = 'apply_incremental_patch "$repo_root/patches/change.patch"'
                    if deps:
                        invocation += ' "$src/third_party/angle"'
                    script = "\n".join([
                        "set -euo pipefail",
                        'readonly repo_root="$(pwd)/adapter"',
                        'readonly src="$(pwd)/chromium"',
                        'say() { printf "%s\\n" "$*"; }',
                        'die() { printf "%s\\n" "$*" >&2; exit 1; }',
                        HELPER, invocation, invocation,
                    ])
                    (root / "exercise.sh").write_text(script, encoding="utf-8", newline="\n")
                    output = self.run_command(BASH, "exercise.sh", cwd=root)
                    self.assertIn("already applied", output)
                    if revision == "new":
                        self.assertIn("backed out incremental patch", output)
                    self.assertEqual(fixture.read_text(encoding="utf-8"), f"before\n{revision}\nafter\n")
                    if deps:
                        self.assertEqual((chromium / "fixture.txt").read_text(), "chromium sentinel\n")


if __name__ == "__main__":
    unittest.main()
