#!/usr/bin/env python3
"""Unit tests of tools/driver_source.py (no network: a local git repository stands for the driver):
python3 -m pytest -q tools/test_driver_source.py"""
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import driver_source  # noqa: E402

COMMIT = 'a' * 40


def repo(tmp_path, line=f'NVIDIA_PROLAPSE_REF="main {COMMIT}"'):
    (tmp_path / 'vendor-image').mkdir(parents=True)
    (tmp_path / 'vendor-image/pins.env').write_text(f'# comment\nL20_LUNCH=x\n{line}\n')
    return tmp_path


def driver(path):
    """A minimal driver checkout with one commit."""
    path.mkdir(parents=True)
    (path / 'build.sh').write_text('#!/bin/sh\n')
    (path / 'rm').mkdir()
    (path / 'rm/lib.rs').write_text('// x\n')
    run = lambda *a: subprocess.run(['git', '-C', str(path), *a], check=True, capture_output=True)
    run('init', '-q')
    run('add', '-A')
    run('-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-qm', 'x')
    return path


def test_pin_reads_the_one_line(tmp_path):
    assert driver_source.pin(repo(tmp_path) / 'vendor-image/pins.env') == ('main', COMMIT)


@pytest.mark.parametrize('line', ['NVIDIA_PROLAPSE_REF=main ' + COMMIT, 'NVIDIA_PROLAPSE_REF="main abc"', ''])
def test_pin_refuses_anything_but_ref_and_full_commit(tmp_path, line):
    with pytest.raises(SystemExit, match='NVIDIA_PROLAPSE_REF'):
        driver_source.pin(repo(tmp_path, line) / 'vendor-image/pins.env')


def test_checkout_default_override_and_packaged(tmp_path):
    root = repo(tmp_path / 'r')
    assert driver_source.checkout(root, {}) == root / 'build/nvidia-prolapse-src'
    local = driver(tmp_path / 'local')
    assert driver_source.checkout(root, {driver_source.ENV: str(local)}) == local
    with pytest.raises(SystemExit, match='not an nvidia-prolapse checkout'):
        driver_source.checkout(root, {driver_source.ENV: str(tmp_path)})
    (root / 'PACKAGED').write_text('packaged\n')
    assert driver_source.checkout(root, {driver_source.ENV: str(local)}) == root / 'release/driver'


def test_identity_names_commit_dirty_and_override(tmp_path):
    root = repo(tmp_path / 'r')
    local = driver(tmp_path / 'local')
    env = {driver_source.ENV: str(local)}
    ident = driver_source.identity(root, env)
    assert ident['commit'] == driver_source.head(local) and not ident['dirty']
    assert ident['local_checkout'] == str(local) and not ident['matches_pin']
    (local / 'rm/lib.rs').write_text('// changed\n')
    assert driver_source.identity(root, env)['dirty']
    (local / 'untracked').write_text('build output\n')
    (local / 'rm/lib.rs').write_text('// x\n')
    assert not driver_source.identity(root, env)['dirty']


def test_fetch_refuses_a_changed_fetched_copy(tmp_path):
    root = repo(tmp_path / 'r')
    copy = driver(root / 'build/nvidia-prolapse-src')
    (copy / 'rm/lib.rs').write_text('// edited in place\n')
    with pytest.raises(SystemExit, match='local changes'):
        driver_source.fetch(root)
