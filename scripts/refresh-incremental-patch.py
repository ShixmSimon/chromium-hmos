#!/usr/bin/env python3
"""Replace a known applied patch without invalidating unchanged build inputs.

Old patch arrives on stdin; the new patch is argv[2]. Preserve exact bytes/mode
and nanosecond mtimes on unchanged files, and restore the original state if the
replacement fails. No build-output timestamps are modified.
"""
import os
from pathlib import Path
import stat
import subprocess
import sys


def refresh(root, old, new):
    root = Path(root).resolve()
    paths = set()
    for patch in (old, new):
        if any(line.startswith((b'rename from ', b'rename to ', b'copy from ', b'copy to '))
               for line in patch.splitlines()):
            raise ValueError('patch renames/copies require explicit handling')
        names = subprocess.check_output(['git', 'apply', '--numstat', '-z'], input=patch)
        for record in names.split(b'\0'):
            if not record:
                continue
            name = os.fsdecode(record.split(b'\t', 2)[2])
            path = root / name
            if not path.resolve().is_relative_to(root) or path.is_symlink():
                raise ValueError('patch path must be a regular file inside its repository')
            paths.add(path)
    before = {}
    for path in paths:
        if path.exists():
            info = path.stat()
            if not stat.S_ISREG(info.st_mode):
                raise ValueError('patch path is not a regular file')
            before[path] = (path.read_bytes(), info)
        else:
            before[path] = None
    try:
        subprocess.run(['git', '-C', str(root), 'apply', '-C0', '--reverse'],
                       input=old, check=True)
        subprocess.run(['git', '-C', str(root), 'apply'], input=new, check=True)
    except BaseException:
        for path, saved in before.items():
            if saved is None:
                path.unlink(missing_ok=True)
            else:
                data, info = saved
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                os.chmod(path, stat.S_IMODE(info.st_mode))
                os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns))
        raise
    preserved = 0
    for path, saved in before.items():
        if saved is not None and path.is_file():
            data, info = saved
            if path.read_bytes() == data and path.stat().st_mode == info.st_mode:
                os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns))
                preserved += 1
    print(f'preserved timestamps of {preserved} unchanged patch file(s)')


if __name__ == '__main__':
    refresh(sys.argv[1], sys.stdin.buffer.read(), Path(sys.argv[2]).read_bytes())
