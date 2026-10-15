#!/usr/bin/env python3
"""Exercise the CI patch helper against isolated Chromium and DEPS repositories."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
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
                (adapter / "scripts").mkdir()
                shutil.copy2(PROJECT / "scripts/refresh-incremental-patch.py", adapter / "scripts")
                fixture = target / "fixture.txt"
                original = "before\noriginal\nafter\n"
                fixture.write_text(original, encoding="utf-8")
                header = target / "unchanged.h"
                header.write_text("before\n")
                self.run_command("git", "add", "fixture.txt", "unchanged.h", cwd=target)
                # A same-named Chromium file must not be touched by a DEPS patch.
                if deps:
                    (chromium / "fixture.txt").write_text("chromium sentinel\n", encoding="utf-8")
                for revision in ("old", "new"):
                    fixture.write_text(f"before\n{revision}\nafter\n", encoding="utf-8")
                    header.write_text("before\nafter\n")
                    diff = self.run_command("git", "diff", "--", "fixture.txt", "unchanged.h", cwd=target)
                    (adapter / "patches" / f"{revision}.txt").write_text(diff, encoding="utf-8")
                fixture.write_text(original, encoding="utf-8")
                header.write_text("before\n")
                fixed_time = 1700000000000000000
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
                        self.assertEqual(header.stat().st_mtime_ns, fixed_time)
                        self.assertGreater(fixture.stat().st_mtime_ns, fixed_time)
                    else:
                        os.utime(header, ns=(fixed_time, fixed_time))
                        os.utime(fixture, ns=(fixed_time, fixed_time))
                    self.assertEqual(fixture.read_text(encoding="utf-8"), f"before\n{revision}\nafter\n")
                    if deps:
                        self.assertEqual((chromium / "fixture.txt").read_text(), "chromium sentinel\n")

    def test_failed_refresh_restores_bytes_and_mtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.run_command("git", "init", "-q", cwd=root)
            fixture = root / "fixture.txt"
            fixture.write_text("old\n")
            stamp = 1700000000000000000
            os.utime(fixture, ns=(stamp, stamp))
            old = "--- a/fixture.txt\n+++ b/fixture.txt\n@@ -1 +1 @@\n-base\n+old\n"
            new = root / "new.patch"
            new.write_text("--- a/fixture.txt\n+++ b/fixture.txt\n@@ -1 +1 @@\n-wrong\n+new\n")
            result = subprocess.run(
                [sys.executable, str(PROJECT / "scripts/refresh-incremental-patch.py"),
                 str(root), str(new)], input=old, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(fixture.read_text(), "old\n")
            self.assertEqual(fixture.stat().st_mtime_ns, stamp)

    def test_dependent_hdr_patch_replay_preserves_only_unchanged_mtime(self):
        retire = "retire_incremental_patch() {" + BUILD_SCRIPT.split(
            "retire_incremental_patch() {", 1
        )[1].split("# Compiling and packaging", 1)[0]
        save = BUILD_SCRIPT.split('  hdr_patch_backup="$(mktemp -d)"', 1)[1].split(
            '  apply_incremental_patch "${repo_root}/patches/ohos-phone-fork.patch"', 1
        )[0]
        save = 'hdr_patch_backup="$(mktemp -d)"' + save
        restore = BUILD_SCRIPT.split(
            '  apply_incremental_patch "${repo_root}/patches/ohos-vulkan-hdr-output.patch"', 1
        )[1].split('  apply_incremental_patch "${repo_root}/patches/ohos-tab-discarding.patch"', 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            adapter, chromium = root / "adapter", root / "chromium"
            for repo in (adapter, chromium):
                repo.mkdir()
                self.run_command("git", "init", "-q", cwd=repo)
                self.run_command("git", "config", "core.autocrlf", "false", cwd=repo)
            (adapter / "patches").mkdir()
            fixture = chromium / "output.h"
            fixture.write_text("before\nSDR8\nafter\n")
            self.run_command("git", "add", "output.h", cwd=chromium)
            import difflib
            def patch(before, after):
                return "diff --git a/output.h b/output.h\n" + "".join(
                    difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                         fromfile="a/output.h", tofile="b/output.h"))
            (adapter / "patches/base.patch").write_text(
                patch("before\nafter\n", "before\nSDR8\nafter\n"))
            hdr = adapter / "patches/ohos-vulkan-hdr-output.patch"
            script = "\n".join([
                "set -euo pipefail", 'readonly repo_root="$(pwd)/adapter"',
                'readonly src="$(pwd)/chromium"',
                'say() { printf "%s\\n" "$*"; }',
                'die() { printf "%s\\n" "$*" >&2; exit 1; }',
                HELPER, retire, save,
                'apply_incremental_patch "$repo_root/patches/base.patch"',
                'apply_incremental_patch "$repo_root/patches/ohos-vulkan-hdr-output.patch"',
                restore,
            ])
            (root / "exercise.sh").write_text(script)
            fixture.write_text("before\nHDR10\nafter\n")
            fixed_time = 1700000000000000000
            os.utime(fixture, ns=(fixed_time, fixed_time))
            for value in ("HDR10", "HDR10", "HDR12"):
                hdr.write_text(patch("before\nSDR8\nafter\n", f"before\n{value}\nafter\n"))
                self.run_command("git", "add", "patches", cwd=adapter)
                self.run_command("git", "-c", "user.name=Patch Test", "-c",
                                 "user.email=test@example.invalid", "commit", "--allow-empty",
                                 "-qm", value, cwd=adapter)
                self.run_command(BASH, "exercise.sh", cwd=root)
                self.assertEqual(fixture.read_text(), f"before\n{value}\nafter\n")
                if value == "HDR10":
                    self.assertEqual(fixture.stat().st_mtime_ns, fixed_time)
                else:
                    self.assertGreater(fixture.stat().st_mtime_ns, fixed_time)


if __name__ == "__main__":
    unittest.main()
