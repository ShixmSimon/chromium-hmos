"""Compile experimental tests through a VFS overlay; never edit the build tree."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


TEST_FILE = 'third_party/blink/renderer/bindings/core/v8/v8_script_runner_test.cc'


def syntax_command(command, overlay):
    """Retain target/toolchain flags while dropping all compile outputs."""
    args = shlex.split(command)
    if any(arg in ('&&', '||', ';', '|', '>', '>>') for arg in args):
        raise ValueError('Expected a single compiler invocation')
    if any(arg.startswith('@') for arg in args):
        raise ValueError('Compilation database must expand response files (-x)')
    result = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
            if index + 1 >= len(args):
                raise ValueError(f'Missing value for {arg}')
            index += 2
            continue
        if arg in ('-c', '-MD', '-MMD', '-MP'):
            index += 1
            continue
        if arg.startswith(('-o', '-MF', '-MT', '-MQ', '-MJ')):
            raise ValueError(f'Unsupported joined output option: {arg}')
        result.append(arg)
        index += 1
    return result + ['-fsyntax-only', '-fno-color-diagnostics', '-ivfsoverlay', str(overlay)]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(source, build, patch, output):
    source, build = source.resolve(), build.resolve()
    patch, output = patch.resolve(), output.resolve()
    original = source / TEST_FILE
    before = digest(original)
    output.mkdir(parents=True, exist_ok=True)
    report = {'status': 'checking', 'source': TEST_FILE, 'original_sha256': before,
              'patch_sha256': digest(patch), 'tests_executed': False,
              'limitation': 'Syntax compilation only; no test binary linked or executed.'}
    try:
        database = json.loads(subprocess.check_output(
            [str(source / 'third_party/ninja/ninja'), '-C', str(build),
             '-t', 'compdb', '-x', 'cxx'], text=True))
        entries = [entry for entry in database
                   if (Path(entry['directory']) / entry['file']).resolve() == original]
        if len(entries) != 1:
            raise ValueError(f'Expected one compile command for {TEST_FILE}; got {len(entries)}')
        entry = entries[0]
        with tempfile.TemporaryDirectory(prefix='module-cache-test-') as temporary:
            work = Path(temporary)
            copied = work / TEST_FILE
            copied.parent.mkdir(parents=True)
            copied.write_bytes(original.read_bytes())
            subprocess.run(['git', 'apply', '--check', str(patch)], cwd=work, check=True)
            subprocess.run(['git', 'apply', str(patch)], cwd=work, check=True)
            report['patched_sha256'] = digest(copied)
            overlay = work / 'overlay.json'
            overlay.write_text(json.dumps({'version': 0, 'use-external-names': False,
                'roots': [{'type': 'file', 'name': str(original),
                           'external-contents': str(copied)}]}))
            args = syntax_command(entry['command'], overlay)
            with (output / 'compiler.log').open('w') as log:
                result = subprocess.run(args, cwd=entry['directory'], stdout=log,
                                        stderr=subprocess.STDOUT, timeout=300)
            report['compiler_exit'] = result.returncode
            if result.returncode:
                raise RuntimeError(f'Compiler failed ({result.returncode}); see compiler.log')
            report['status'] = 'compiled'
    except Exception as error:
        report['status'] = 'error'
        report['error'] = str(error)
        raise
    finally:
        report['original_unchanged'] = digest(original) == before
        if not report['original_unchanged']:
            report['status'] = 'error'
        (output / 'syntax-result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
    if not report['original_unchanged']:
        raise RuntimeError('Shared source changed during validation')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path('/root/chromium-155/src'))
    parser.add_argument('--build', type=Path)
    parser.add_argument('--patch', type=Path,
                        default=Path('patches/ohos-module-cache-consumer-tests.patch'))
    parser.add_argument('--output', type=Path, default=Path('validation/consumer-tests'))
    options = parser.parse_args()
    source = options.source.resolve()
    validate(source, (options.build or source / 'out/ohos_arm64').resolve(),
             options.patch.resolve(), options.output.resolve())
