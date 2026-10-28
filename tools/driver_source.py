#!/usr/bin/env python3
"""The driver this checkout builds: github.com/Shiro836/nvidia-prolapse (NVK over the Rust RM backend, nvrm,
the Vulkan HAL, the gralloc backend, the Codec2 decoder, bionic libdrm), built from source like Mesa and ANGLE.

The pin is one line of vendor-image/pins.env, NVIDIA_PROLAPSE_REF="<ref> <40-hex commit>". The pinned commit
is fetched into build/nvidia-prolapse-src and checked out detached; its commit is verified. A developer builds
from a local checkout instead with NVIDIA_PROLAPSE_SRC=<dir> (packaging/native/build-native.sh --driver-src
<dir>); the commit and dirty flag of whatever was built go into BUILD-RECORD.json (packaging/native/collect.py).

  tools/driver_source.py fetch        fetch and verify the pin (no-op when build/nvidia-prolapse-src already is it)
  tools/driver_source.py path         print the checkout in use (NVIDIA_PROLAPSE_SRC, else the pinned one)
  tools/driver_source.py identity     JSON: url, ref, pinned commit, built commit, dirty, local override or not
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
URL = 'https://github.com/Shiro836/nvidia-prolapse'
PINS = REPO / 'vendor-image/pins.env'
ENV = 'NVIDIA_PROLAPSE_SRC'


def pin(pins=PINS):
    """(ref, commit) from NVIDIA_PROLAPSE_REF="<ref> <commit>"."""
    for line in Path(pins).read_text().splitlines():
        m = re.fullmatch(r'NVIDIA_PROLAPSE_REF="(\S+) ([0-9a-f]{40})"', line.strip())
        if m:
            return m.group(1), m.group(2)
    raise SystemExit(f'driver_source: no NVIDIA_PROLAPSE_REF="<ref> <40-hex commit>" line in {pins}')


def pinned_dir(repo=REPO):
    return Path(repo) / 'build/nvidia-prolapse-src'


def checkout(repo=REPO, environ=os.environ):
    """The driver checkout in use: NVIDIA_PROLAPSE_SRC when set, else the pinned one. A packaged install (the file
    PACKAGED, packaging/native/install-native.sh) has no checkout, only the driver files its install-time gate
    runs, shipped in the release bundle under driver/ (packaging/native/collect.py DRIVER_FILES)."""
    if (Path(repo) / 'PACKAGED').is_file():
        return Path(repo) / 'release/driver'
    if environ.get(ENV):
        path = Path(environ[ENV]).resolve()
        if not (path / 'build.sh').is_file() or not (path / 'rm').is_dir():
            raise SystemExit(f'driver_source: {ENV}={path} is not an nvidia-prolapse checkout (no build.sh, rm/)')
        return path
    return pinned_dir(repo)


def git(path, *args, check=True):
    return subprocess.run(['git', '-C', str(path), *args], capture_output=True, text=True, check=check).stdout.strip()


def head(path):
    return git(path, 'rev-parse', 'HEAD')


def dirty(path):
    """Tracked files differ from HEAD (build outputs are ignored by the driver repository)."""
    return bool(git(path, 'status', '--porcelain', '--untracked-files=no'))


def fetch(repo=REPO):
    """Make build/nvidia-prolapse-src the pinned commit, verified; returns its path. A checkout with local
    changes is refused, never overwritten."""
    _ref, commit = pin(Path(repo) / 'vendor-image/pins.env')
    path = pinned_dir(repo)
    if (path / '.git').exists():
        if dirty(path):
            raise SystemExit(f'driver_source: {path} has local changes; remove it (it is a fetched copy) or build from '
                             f'a checkout of your own with {ENV}')
        if head(path) == commit:
            return path
    else:
        path.mkdir(parents=True, exist_ok=True)
        git(path, 'init', '-q')
    if subprocess.run(['git', '-C', str(path), 'cat-file', '-e', f'{commit}^{{commit}}'],
                      capture_output=True).returncode:
        subprocess.run([sys.executable, str(Path(repo) / 'tools/fetch.py'), 'retry', '--',
                        'git', '-C', str(path), 'fetch', '-q', '--depth', '1', URL, commit], check=True)
    git(path, 'checkout', '-q', '--detach', commit)
    if head(path) != commit:
        raise SystemExit(f'driver_source: {path} is at {head(path)}, not the pinned {commit}')
    return path


def identity(repo=REPO, environ=os.environ):
    ref, commit = pin(Path(repo) / 'vendor-image/pins.env')
    path = checkout(repo, environ)
    built = head(path)
    return {'url': URL, 'ref': ref, 'pinned_commit': commit, 'commit': built, 'dirty': dirty(path),
            'local_checkout': str(path) if environ.get(ENV) else None, 'matches_pin': built == commit}


def main(argv):
    if argv == ['fetch']:
        print(fetch())
    elif argv == ['path']:
        print(checkout())
    elif argv == ['identity']:
        print(json.dumps(identity(), indent=2))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
