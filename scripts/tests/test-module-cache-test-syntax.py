"""Guard syntax checks against writes to a shared native build tree."""
import difflib
import importlib.util
import json
from pathlib import Path
import shlex
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'syntax', Path(__file__).resolve().parents[1] / 'module-cache-test-syntax.py')
syntax = importlib.util.module_from_spec(spec)
spec.loader.exec_module(syntax)


class SyntaxValidationTest(unittest.TestCase):
    def test_output_and_dependency_flags_are_removed(self):
        command = 'clang++ -DNAME="two words" -MMD -MF shared.d -MT shared.o -c test.cc -o shared.o'
        result = syntax.syntax_command(command, '/private/overlay.json')
        self.assertEqual(result, ['clang++', '-DNAME=two words', 'test.cc',
            '-fsyntax-only', '-fno-color-diagnostics', '-ivfsoverlay', '/private/overlay.json'])

    def test_rejects_unexpanded_response_files_and_shell_commands(self):
        for command in ['clang++ @shared.rsp', 'clang++ test.cc && touch shared.o',
                        'clang++ test.cc -ojoined.o', 'clang++ test.cc -MF']:
            with self.subTest(command=command), self.assertRaises(ValueError):
                syntax.syntax_command(command, 'overlay.json')

    def validation(self, directory, compiler_exit):
        root = Path(directory)
        source = root / 'src'
        original = source / syntax.TEST_FILE
        original.parent.mkdir(parents=True)
        original.write_text('// before\n')
        build = source / 'out/test'
        build.mkdir(parents=True)
        object_file = build / 'shared.o'
        object_file.write_bytes(b'preserve shared object')
        compiler = root / 'fake-compiler.py'
        compiler.write_text('''import json, pathlib, sys
args = sys.argv[1:]
assert '-c' not in args and '-o' not in args and '-MF' not in args
assert '-fsyntax-only' in args
overlay = json.loads(pathlib.Path(args[args.index('-ivfsoverlay') + 1]).read_text())
entry = overlay['roots'][0]
assert pathlib.Path(entry['name']).read_text() == '// before\\n'
assert pathlib.Path(entry['external-contents']).read_text() == '// after\\n'
print('compiler saw isolated patched source')
sys.exit(''' + str(compiler_exit) + ')\n')
        diff = root / 'test.patch'
        diff.write_text(''.join(difflib.unified_diff(['// before\n'], ['// after\n'],
            fromfile='a/' + syntax.TEST_FILE, tofile='b/' + syntax.TEST_FILE)))
        entry = {'directory': str(build), 'file': str(original),
                 'command': shlex.join([sys.executable, str(compiler), '-c',
                     str(original), '-MMD', '-MF', 'shared.d', '-o', 'shared.o'])}
        output = root / 'evidence'
        with patch.object(syntax.subprocess, 'check_output', return_value=json.dumps([entry])):
            if compiler_exit:
                with self.assertRaisesRegex(RuntimeError, 'Compiler failed'):
                    syntax.validate(source, build, diff, output)
            else:
                syntax.validate(source, build, diff, output)
        self.assertEqual(original.read_text(), '// before\n')
        self.assertEqual(object_file.read_bytes(), b'preserve shared object')
        self.assertFalse((build / 'shared.d').exists())
        result = json.loads((output / 'syntax-result.json').read_text())
        self.assertTrue(result['original_unchanged'])
        self.assertFalse(result['tests_executed'])
        self.assertEqual(result['status'], 'error' if compiler_exit else 'compiled')
        self.assertIn('compiler saw isolated patched source', (output / 'compiler.log').read_text())

    def test_success_keeps_shared_source_and_outputs_intact(self):
        with tempfile.TemporaryDirectory() as directory:
            self.validation(directory, 0)

    def test_failure_retains_evidence_and_keeps_shared_source_intact(self):
        with tempfile.TemporaryDirectory() as directory:
            self.validation(directory, 17)


if __name__ == '__main__':
    unittest.main()
