#!/usr/bin/env python3
"""Exercise persistent GN configuration, snapshot staging and child selection."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('pgo', ROOT / 'scripts/configure-v8-builtins-pgo.py')
pgo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pgo)


class ContextSnapshotTest(unittest.TestCase):
    def test_persistent_args_enable_and_disable_without_changing_other_options(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'config').mkdir()
            out = root / 'out/test'
            out.mkdir(parents=True)
            args = out / 'args.gn'
            for enabled in (True, False):
                (root / 'config/args.plan_kirin_pc.gn').write_text(
                    'v8_enable_builtins_optimization = false\n'
                    f'use_v8_context_snapshot = {str(enabled).lower()}\n')
                args.write_text('target_cpu = "arm64"\nuse_v8_context_snapshot = '
                                f'{str(not enabled).lower()}\n')
                with patch.object(pgo, 'ROOT', root), patch.object(sys, 'argv',
                        ['configure', str(root), 'out/test', str(root / 'status')]), \
                        patch.object(pgo.subprocess, 'run') as run, \
                        patch.object(pgo.subprocess, 'check_output', return_value='[1/2] CXX foo\n[2/2] LINK bar\n'):
                    pgo.main()
                self.assertIn('target_cpu = "arm64"', args.read_text())
                self.assertIn(f'use_v8_context_snapshot = {str(enabled).lower()}', args.read_text())
                run.assert_called_once()
                report = json.loads((root / 'status/v8-context-snapshot.json').read_text())
                self.assertEqual(report['enabled'], enabled)
                self.assertEqual(report['pending_tasks'], 2)

    def test_launcher_uses_selected_snapshot_with_no_extra_descriptor(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        self.assertIsNotNone(compiler)
        source = (ROOT / 'overlay/base/process/launch_ohos.cc').read_text()
        functions = source.split('void SetOhosV8ContextSnapshotEnabled(', 1)[1].split(
            'bool IsOhosIsolatedRenderer()', 1)[0]
        self.assertIn('assets.AppendASCII(GetOhosV8SnapshotFileName())', source)
        with tempfile.TemporaryDirectory() as temp:
            code = Path(temp) / 'selection.cc'
            code.write_text('#include <atomic>\n#include <cassert>\n#include <string>\n'
                            'std::atomic<bool> g_v8_context_snapshot_enabled{false};\n'
                            'void SetOhosV8ContextSnapshotEnabled(' + functions + '''
int main() {
  assert(std::string(GetOhosV8SnapshotFileName()) == "snapshot_blob.bin");
  SetOhosV8ContextSnapshotEnabled(true);
  assert(std::string(GetOhosV8SnapshotFileName()) == "v8_context_snapshot.bin");
  SetOhosV8ContextSnapshotEnabled(false);
  assert(std::string(GetOhosV8SnapshotFileName()) == "snapshot_blob.bin");
}
''')
            binary = str(Path(temp) / 'selection')
            subprocess.run([compiler, '-std=c++20', str(code), '-o', binary], check=True)
            subprocess.run([binary], check=True)

    def test_staging_requires_enabled_snapshot_and_excludes_disabled_stale_file(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            out, project = root / 'out', root / 'project'
            out.mkdir()
            (out / 'locales').mkdir()
            for name in ['libweb_engine.so', 'libnweb_render.so', 'liblumie_adblock.so',
                         'libEGL_angle.so', 'libGLESv2_angle.so', 'icudtl.dat', 'snapshot_blob.bin',
                         'chrome_100_percent.pak', 'chrome_200_percent.pak',
                         'headless_command_resources.pak', 'resources.pak', 'locales/en-US.pak']:
                (out / name).write_bytes(b'fixture')
            (out / 'args.gn').write_text('use_v8_context_snapshot = true\n')
            command = ['bash', str(ROOT / 'scripts/stage-runtime-assets.sh'), str(out), str(project)]
            missing = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn('missing V8 context snapshot', missing.stderr)
            (out / 'v8_context_snapshot.bin').write_bytes(b'context snapshot')
            subprocess.run(command, capture_output=True, check=True)
            raw = project / 'engine/src/main/resources/rawfile/chromium'
            self.assertEqual((raw / 'v8_context_snapshot.bin').read_bytes(), b'context snapshot')
            version = (raw / 'runtime.version').read_text()
            (out / 'v8_context_snapshot.bin').write_bytes(b'updated context snapshot')
            subprocess.run(command, capture_output=True, check=True)
            self.assertNotEqual((raw / 'runtime.version').read_text(), version)
            (out / 'args.gn').write_text('use_v8_context_snapshot = false\n')
            subprocess.run(command, capture_output=True, check=True)
            self.assertFalse((raw / 'v8_context_snapshot.bin').exists())
            self.assertTrue((raw / 'snapshot_blob.bin').exists())


if __name__ == '__main__':
    unittest.main()
