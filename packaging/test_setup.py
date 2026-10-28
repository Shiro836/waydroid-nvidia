#!/usr/bin/env python3
"""Unit tests of waydroid-nvidia-setup's `waydroid init` step and -s/--system-type (no root, no Waydroid):
python3 -m pytest -q packaging/test_setup.py

The setup runs from a copy whose two path constants (waydroid.cfg, the package's lib dir) point into a temporary
directory, with stubs for `id` (root), `systemctl` and `waydroid` (records its arguments, then fails, so the setup
stops right after its init step) and no `readelf` (the setup's next check stops it when init is not needed)."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

import pytest

REPO = Path(__file__).resolve().parents[1]
SETUP = REPO / 'packaging/aur/waydroid-nvidia-bin/waydroid-nvidia-setup'
CONSTANTS = {'CFG=/var/lib/waydroid/waydroid.cfg\n': 'CFG={cfg}\n', 'LIB=/usr/lib/waydroid-nvidia\n': 'LIB={lib}\n'}
OTA = 'https://ota.waydro.id/system/lineage/waydroid_x86_64/{}.json'


@pytest.fixture
def env(tmp_path):
    text = SETUP.read_text()
    cfg, lib = tmp_path / 'waydroid.cfg', tmp_path / 'lib'
    for old, new in CONSTANTS.items():
        assert text.count(old) == 1, old
        text = text.replace(old, new.format(cfg=cfg, lib=lib))
    script = tmp_path / 'setup'
    script.write_text(text)
    (lib / 'guest').mkdir(parents=True)
    bin_dir = tmp_path / 'bin'
    bin_dir.mkdir()
    calls = tmp_path / 'waydroid-calls'
    stubs = {'id': 'echo 0', 'systemctl': 'exit 3',
             'waydroid': f'printf "%s\\n" "$*" >> {calls}; exit 1'}
    for name, body in stubs.items():
        (bin_dir / name).write_text(f'#!/bin/sh\n{body}\n')
        (bin_dir / name).chmod(0o755)
    for tool in ('python3', 'basename', 'cat', 'awk'):
        os.symlink(shutil.which(tool), bin_dir / tool)

    def run(*argv, system_ota=None, environ=None, stubs=None):
        if system_ota is not None:
            cfg.write_text(f'[waydroid]\nsystem_ota = {system_ota}\nimages_path = /var/lib/waydroid/images\n')
        for name, body in (stubs or {}).items():
            (bin_dir / name).write_text(f'#!/bin/sh\n{body}\n')
            (bin_dir / name).chmod(0o755)
        out = subprocess.run([shutil.which('bash'), str(script), *argv], capture_output=True, text=True,
                             env={'PATH': str(bin_dir), **(environ or {'LC_ALL': 'C'})})
        made = calls.read_text().splitlines() if calls.exists() else []
        return out.returncode, out.stdout + out.stderr, made
    return run


def test_fresh_install_passes_the_system_type_to_init(env):
    code, out, calls = env('-s', 'GAPPS')
    assert calls == ['init -s GAPPS']
    assert "running 'waydroid init -s GAPPS'" in out
    assert 'waydroid init -s GAPPS failed' in out and code != 0


@pytest.mark.parametrize('flag', ['-s', '--system-type', '--system_type'])
def test_spellings_and_case(env, flag):
    _, _, calls = env(flag, 'gapps')
    assert calls == ['init -s GAPPS']


def test_fresh_install_without_s_runs_plain_init(env):
    _, out, calls = env()
    assert calls == ['init']
    assert 'sudo waydroid-nvidia-setup -s GAPPS' in out          # how to get Google apps instead


def test_unknown_system_type_is_refused(env):
    code, out, calls = env('-s', 'MICROG')
    assert code != 0 and calls == []
    assert 'takes VANILLA, GAPPS (with Google apps) or FOSS' in out


def test_initialized_with_another_type_says_how_to_switch(env):
    code, out, calls = env('-s', 'GAPPS', system_ota=OTA.format('VANILLA'))
    assert code != 0 and calls == []                             # never re-initializes on its own
    assert 'already initialized with the VANILLA system image' in out
    assert 'sudo waydroid init -f -s GAPPS' in out and 'apps and data are kept' in out
    assert 'run this setup without -s' in out


def test_initialized_with_the_same_type_goes_on(env):
    code, out, calls = env('-s', 'GAPPS', system_ota=OTA.format('GAPPS'))
    assert calls == []
    assert 'Waydroid is initialized with the GAPPS system image' in out
    assert 'readelf is required' in out                          # the next check after the init step


def test_preinstalled_images_have_no_system_type_to_pick(env):
    code, out, calls = env('-s', 'GAPPS', system_ota='None')
    assert code != 0 and calls == []
    assert 'images it does not download' in out


def test_initialized_without_s_does_not_check_the_type(env):
    _, out, calls = env(system_ota=OTA.format('VANILLA'))
    assert calls == [] and 'readelf is required' in out


def test_restore_takes_no_system_type(env):
    code, out, calls = env('--restore-official-vendor', '-s', 'GAPPS')
    assert code != 0 and calls == [] and 'takes no other option' in out


def test_help_names_the_option(env):
    code, out, _ = env('--help')
    assert code == 0 and '-s, --system-type VANILLA|GAPPS|FOSS' in out


APPARMOR = ("lxc-info: waydroid: ../src/lxc/confile.c: set_config_apparmor_profile: 1651 Invalid argument - Built "
            "without AppArmor support")


def test_lxc_info_failure_stops_with_lxc_message_and_remedy(env):
    """Issue #27: an LXC that rejects the container config must stop the setup with LXC's own message, not read as
    a stopped container."""
    code, out, calls = env('--restore-official-vendor', system_ota='None',
                           stubs={'lxc-info': f'echo "{APPARMOR}" >&2; exit 1'})
    assert code != 0 and calls == []
    assert 'lxc-info cannot read the Waydroid container: ' + APPARMOR in out
    assert 'Remedy: sudo waydroid upgrade -o' in out
    assert 'stopping the Waydroid container' not in out


def test_tools_run_in_the_c_locale(env):
    """Issue #28: parsed tool output must not depend on the user's locale (LANG=es_ES printed "Biblioteca compartida")."""
    code, out, _ = env('--restore-official-vendor', system_ota='None',
                       environ={'LANG': 'es_ES.UTF-8', 'LANGUAGE': 'es', 'LC_MESSAGES': 'es_ES.UTF-8'},
                       stubs={'lxc-info': 'echo "LC_ALL=$LC_ALL LANGUAGE=${LANGUAGE-unset}" >&2; exit 1'})
    assert code != 0 and 'LC_ALL=C LANGUAGE=unset' in out


@pytest.mark.parametrize('script', ['packaging/install.sh', 'packaging/aur/waydroid-nvidia-bin/waydroid-nvidia-setup',
                                    'packaging/native/wd-native'])
def test_shell_entrypoints_set_the_c_locale(script):
    text = (REPO / script).read_text()
    assert 'set -euo pipefail\n' in text
    head = text.split('set -euo pipefail\n', 1)[1]
    assert '\nexport LC_ALL=C\nunset LANGUAGE\n' in '\n' + head[:400], script


@pytest.mark.parametrize('script', ['packaging/native/vendor_image.py', 'dev/source-native', 'dev/driver-mode'])
def test_python_entrypoints_set_the_c_locale(script, tmp_path):
    """Run the entrypoint's __main__ with a stub main(): the environment its children get has LC_ALL=C."""
    text = (REPO / script).read_text()
    main = text.split("if __name__ == '__main__':\n", 1)[1]
    code = ('import os, sys\nos.environ["LANGUAGE"] = "es"\n'
            'def main():\n    print(os.environ["LC_ALL"], os.environ.get("LANGUAGE", "unset")); return 0\n'
            'import subprocess\nif True:\n' + main)
    out = subprocess.run([sys.executable, '-c', code, 'status'], capture_output=True, text=True,
                         env={'PATH': os.environ['PATH'], 'LANG': 'es_ES.UTF-8'})
    assert out.stdout.split() == ['C', 'unset'], out.stderr


def test_glibc_shim_is_a_mode(env):
    """glibc-shim is provisioned by the package (it was refused as a developer mode before)."""
    code, out, calls = env('--mode', 'glibc-shim', system_ota='None')
    assert code != 0 and '--mode is' not in out and 'developer mode' not in out
    assert 'readelf is required' in out           # the next check after the mode argument was accepted
    code, out, _ = env('--mode', 'bogus', system_ota='None')
    assert code != 0 and '--mode is nvidia-prolapse, glibc-shim or venus' in out
