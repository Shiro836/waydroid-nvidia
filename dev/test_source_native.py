#!/usr/bin/env python3
# Unit tests for dev/source-native's pure parts (no root, no container, no GPU).
#   python3 -m unittest -v dev/test_source_native.py
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import glob
import hashlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
loader = importlib.machinery.SourceFileLoader('source_native_under_test', str(HERE / 'source-native'))
spec = importlib.util.spec_from_loader(loader.name, loader)
sn = importlib.util.module_from_spec(spec)
loader.exec_module(sn)
nn = sn.native
V = '615.71.09'
ELF = {64: b'\x7fELF\x02', 32: b'\x7fELF\x01'}
PROC = 'NVRM version: NVIDIA UNIX Open Kernel Module for x86_64  {}  Release Build  (root@host)\n'


def touch(path, data=b''):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


@contextlib.contextmanager
def fake_nvidia_drm(top):
    """A sysfs fixture with one NVIDIA render node, renderD129. nvidia_native.nvidia_render_node() globs
    /sys/class/drm; restored_cfg_text() runs it in dev/driver-mode's own copy of the module, which a
    mock of this test's module cannot reach, so the glob itself is redirected (only that pattern)."""
    sysfs = Path(top) / 'sys-class-drm'
    touch(sysfs / 'renderD128/device/vendor', b'0x8086\n')
    touch(sysfs / 'renderD129/device/vendor', b'0x10de\n')
    real = glob.glob

    def fake(pattern, *args, **kwargs):
        if pattern.startswith('/sys/class/drm/'):
            pattern = str(sysfs) + pattern[len('/sys/class/drm'):]
        return real(pattern, *args, **kwargs)
    with mock.patch('glob.glob', fake):
        yield


def hook_sections(text):
    """[(section, [(key, value), ...]), ...] of an alpm hook file (repeated sections allowed)."""
    sections = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('['):
            sections.append((line.strip('[]'), []))
        else:
            key, value = (x.strip() for x in line.split('=', 1))
            sections[-1][1].append((key, value))
    return sections


def triggers(text):
    out = set()
    for name, pairs in hook_sections(text):
        if name != 'Trigger':
            continue
        ops = tuple(sorted(v for k, v in pairs if k == 'Operation'))
        for target in (v for k, v in pairs if k == 'Target'):
            for op in ops:
                out.add((dict(pairs)['Type'], target, op))
    return out


class HookRenderTest(unittest.TestCase):
    # Triggers of the legacy waydroid-nvidia-prolapse package's hook (90-waydroid-nvidia-prolapse.hook,
    # not part of this repo); the override must keep all of them.
    packaged = ('[Trigger]\nOperation = Install\nOperation = Upgrade\nType = Package\nTarget = nvidia-utils\n\n'
                '[Trigger]\nOperation = Upgrade\nType = Package\nTarget = waydroid-nvidia-prolapse\n\n'
                '[Action]\nWhen = PostTransaction\nExec = /usr/bin/waydroid-nvidia-prolapse-setup --hook\n')

    def test_superset_of_packaged_triggers_plus_module_package(self):
        text = sn.render_hook('nvidia-open-dkms')
        have = triggers(text)
        self.assertLessEqual(triggers(self.packaged), have)
        for op in ('Install', 'Upgrade', 'Remove'):
            self.assertIn(('Package', 'nvidia-open-dkms', op), have)
        self.assertEqual(have - triggers(self.packaged),
                         {('Package', 'nvidia-open-dkms', op) for op in ('Install', 'Upgrade', 'Remove')})

    def test_action_runs_source_native_hook(self):
        action = dict(hook_sections(sn.render_hook('nvidia-open'))[-1][1])
        self.assertEqual(action['When'], 'PostTransaction')
        self.assertEqual(action['Exec'], '{} {}/dev/source-native hook'.format(sn.PYTHON, REPO))
        self.assertNotIn('waydroid-nvidia-prolapse-setup', action['Exec'])

    def test_module_package_equal_to_nvidia_utils_not_duplicated(self):
        self.assertEqual(sn.render_hook('nvidia-utils').count('Target = nvidia-utils'), 1)

    def test_same_file_name_as_packaged(self):
        self.assertEqual(sn.HOOK.name, sn.PACKAGED_HOOK.name)
        self.assertEqual(sn.HOOK.parent, Path('/etc/pacman.d/hooks'))

    def test_override_is_mode_independent(self):
        # one drop-in for every driver mode: verify-active checks whatever waydroid.cfg selects
        text = sn.render_override(Path('/var/lib/waydroid/x'))
        self.assertIn('dev/driver-mode verify-active\n', text)
        self.assertNotIn('/var/lib/waydroid/x', text)
        self.assertIn('build/waydroid/waydroid.py container start\n', text)

    def test_override_waits_for_the_nvidia_render_node(self):
        # boot race 2026-10-17: the container started before the NVIDIA modules were up
        text = sn.render_override(None)
        unit = text.split('\n\n[Service]')[0].splitlines()
        self.assertEqual(unit, ['[Unit]', f'Wants={sn.RENDER_UNIT}', f'After={sn.RENDER_UNIT}'])
        self.assertEqual(sn.RENDER_UNIT, nn.RENDER_DEVICE_UNIT)

    def test_render_unit_is_the_rule_alias(self):
        rules = (REPO / 'packaging/aur/waydroid-nvidia-bin/waydroid-nvidia-gpu.rules').read_text()
        rule = [line for line in rules.replace('\\\n', ' ').splitlines() if 'SYSTEMD_ALIAS' in line and not line.startswith('#')]
        self.assertEqual(len(rule), 1, rule)
        for key in ('ACTION!="remove"', 'SUBSYSTEM=="drm"', 'KERNEL=="renderD[0-9]*"', 'ATTRS{vendor}=="0x10de"',
                    'DRIVERS=="nvidia"', 'TAG+="systemd"', f'ENV{{SYSTEMD_ALIAS}}+="{nn.RENDER_DEVICE_ALIAS}"'):
            self.assertIn(key, rule[0])
        # systemd-escape --path --suffix=device: '/' -> '-', leading '/' dropped; [A-Za-z0-9:_.] kept as is
        alias = nn.RENDER_DEVICE_ALIAS.strip('/')
        self.assertRegex(alias, r'^[A-Za-z0-9:_./]+$')
        self.assertEqual(alias.replace('/', '-') + '.device', nn.RENDER_DEVICE_UNIT)
        if shutil.which('systemd-escape'):
            out = subprocess.run(['systemd-escape', '--path', '--suffix=device', nn.RENDER_DEVICE_ALIAS],
                                 capture_output=True, text=True, check=True).stdout.strip()
            self.assertEqual(out, nn.RENDER_DEVICE_UNIT)
        setup = (REPO / 'packaging/aur/waydroid-nvidia-bin/waydroid-nvidia-setup').read_text()
        self.assertIn(f'RENDER_UNIT={nn.RENDER_DEVICE_UNIT}\n', setup)


class HookModeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.top = Path(self.tmp.name)
        self.cfg = self.top / 'waydroid.cfg'
        self.source = self.top / 'source'
        touch(self.source / 'source-rm.json', b'{}')
        self.legacy = self.top / 'legacy'
        self.legacy.mkdir()

    def write(self, text):
        self.cfg.write_text(text)

    def test_source_root(self):
        self.write('[waydroid]\nnvidia_native_root = {}\n'.format(self.source))
        self.assertEqual(sn.hook_mode(self.cfg), ('source', self.source))

    def test_legacy_root(self):
        self.write('[waydroid]\nnvidia_native_root = {}\n'.format(self.legacy))
        self.assertEqual(sn.hook_mode(self.cfg), ('legacy', None))

    def test_not_native_or_no_cfg(self):
        self.write('[waydroid]\narch = x86_64\n')
        self.assertEqual(sn.hook_mode(self.cfg), ('legacy', None))
        self.assertEqual(sn.hook_mode(self.top / 'absent.cfg'), ('legacy', None))

    def links(self, rust=None, shim=None):
        """Patch the root conventions to links in the test dir pointing at rust/shim (None: no link)."""
        links = {'nvidia-prolapse': str(self.top / 'nvidia-prolapse'), 'glibc-shim': str(self.top / 'nvidia-glibc-shim')}
        for mode, target in (('nvidia-prolapse', rust), ('glibc-shim', shim)):
            if os.path.lexists(links[mode]):
                os.unlink(links[mode])
            if target:
                os.symlink(target, links[mode])
        p = mock.patch.object(nn, 'ROOT_LINKS', links)
        p.start()
        self.addCleanup(p.stop)
        return links

    def test_driver_mode_and_links(self):
        self.links(rust=self.source, shim=self.legacy)
        self.write('[waydroid]\nnvidia_driver_mode = nvidia-prolapse\n')
        self.assertEqual(sn.hook_mode(self.cfg), ('source', self.source))
        self.write('[waydroid]\nnvidia_driver_mode = glibc-shim\n')
        self.assertEqual(sn.hook_mode(self.cfg), ('glibc-shim', self.legacy))
        self.write('[waydroid]\nnvidia_driver_mode = venus\n')
        self.assertEqual(sn.hook_mode(self.cfg), ('venus', None))
        self.write('[waydroid]\nnvidia_driver_mode = native\n')
        self.assertEqual(sn.hook_mode(self.cfg)[0], 'unknown')
        self.links(rust=self.legacy)                             # the rust link names a non-source root
        self.write('[waydroid]\nnvidia_driver_mode = nvidia-prolapse\n')
        self.assertEqual(sn.hook_mode(self.cfg)[0], 'unknown')
        self.links()                                             # no link at all
        self.assertEqual(sn.hook_mode(self.cfg)[0], 'unknown')

    def test_configured_root(self):
        self.links(rust=self.source)
        with mock.patch.object(sn, 'CFG', self.cfg):
            self.write('[waydroid]\nnvidia_driver_mode = nvidia-prolapse\n')
            self.assertEqual(sn.configured_root(self.cfg), self.source)
            # a checkpoint: its recorded link target, or the path key it was saved with
            backup = self.top / 'before-source-native-x'
            backup.mkdir()
            (backup / 'waydroid.cfg').write_text('[waydroid]\nnvidia_driver_mode = nvidia-prolapse\n')
            (backup / sn.LINK_TARGET).write_text(str(self.legacy) + '\n')
            self.assertEqual(sn.configured_root(backup / 'waydroid.cfg'), self.legacy)
            (backup / 'waydroid.cfg').write_text('[waydroid]\nnvidia_native_root = {}\n'.format(self.source))
            self.assertEqual(sn.configured_root(backup / 'waydroid.cfg'), self.source)

    def test_restored_cfg_text_converts_path_keys_to_links(self):
        self.links(rust=self.source)
        saved = self.top / 'saved.cfg'
        for text, mode, links in (
                ('[waydroid]\nnvidia_native_root = {}\n'.format(self.source), 'nvidia-prolapse', {'nvidia-prolapse': str(self.source)}),
                ('[waydroid]\nnvidia_native_root = {}\n'.format(self.legacy), 'glibc-shim', {'glibc-shim': str(self.legacy)}),
                ('[waydroid]\nnvidia_driver_mode = venus\nnvidia_rust_driver_root = {}\nnvidia_venus_socket = /x/v.sock\n'
                 .format(self.source), 'venus', {'nvidia-prolapse': str(self.source)}),
                ('[waydroid]\nnvidia_venus_socket = /run/waydroid-venus/venus.sock\n', 'venus', {})):
            with self.subTest(text=text):
                saved.write_text(text + '\n[properties]\nro.hardware.egl = angle\n')
                with fake_nvidia_drm(self.top), contextlib.redirect_stdout(io.StringIO()):
                    out, got = sn.restored_cfg_text(saved)
                self.assertEqual(got, links)
                self.assertIn('nvidia_driver_mode = ' + mode, out)
                for key in nn.RETIRED_KEYS + ('ro.hardware.egl',):
                    self.assertNotIn(key, out)
        saved.write_text('[waydroid]\nnvidia_driver_mode = nvidia-prolapse\nnvidia_native_gl = off\n')
        self.assertEqual(sn.restored_cfg_text(saved), (saved.read_text(), {}))    # already in the convention

    def test_pre_rename_checkpoint_restores_under_the_new_name(self):
        self.links(rust=self.source)
        saved = self.top / 'saved-old-name.cfg'
        saved.write_text('[waydroid]\nnvidia_driver_mode = rust-driver\nnvidia_native_gl = off\n\n[properties]\n')
        out_io = io.StringIO()
        with mock.patch.object(nn, 'nvidia_render_node', lambda *a: '/dev/dri/renderD129'), \
                contextlib.redirect_stdout(out_io):
            out, links = sn.restored_cfg_text(saved)
        self.assertIn('nvidia_driver_mode = nvidia-prolapse', out)
        self.assertNotIn('rust-driver', out)
        self.assertIn('old mode name', out_io.getvalue())
        backup = self.top / 'before-source-native-old'
        backup.mkdir()
        (backup / sn.LEGACY_LINK_TARGET).write_text(str(self.legacy) + '\n')   # pre-rename link record
        self.assertEqual(sn._link_record(backup), backup / sn.LEGACY_LINK_TARGET)
        (backup / sn.LINK_TARGET).write_text(str(self.source) + '\n')
        self.assertEqual(sn._link_record(backup), backup / sn.LINK_TARGET)

    def test_glibc_shim_and_venus_never_run_legacy_setup(self):
        for mode in (('glibc-shim', self.legacy), ('venus', None)):
            with self.subTest(mode=mode[0]):
                out = io.StringIO()
                with mock.patch.object(sn, 'hook_mode', return_value=mode), \
                        mock.patch.object(sn.os, 'execv') as execv, contextlib.redirect_stdout(out):
                    sn.hook()
                execv.assert_not_called()
                self.assertIn('NOT run' if mode[0] == 'glibc-shim' else 'not run', out.getvalue())

    def test_unparseable_is_unknown_not_legacy(self):
        self.write('nvidia_native_root = {}\n'.format(self.source))   # no section header
        mode, reason = sn.hook_mode(self.cfg)
        self.assertEqual(mode, 'unknown')
        self.assertIn(str(self.cfg), reason)


class HookCommandTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'root'
        touch(self.root / 'source-rm.json', json.dumps({'format': 2, 'kernel_module': V}).encode())
        self.execv = mock.patch.object(sn.os, 'execv', side_effect=AssertionError('legacy setup ran'))
        self.execv.start()
        self.addCleanup(self.execv.stop)

    def run_hook(self, mode, loaded=V, installed='615.78', backups=None):
        out = io.StringIO()
        with mock.patch.object(sn, 'BACKUPS', backups or str(Path(self.tmp.name) / 'before-source-native-*')), \
             mock.patch.object(sn, 'hook_mode', return_value=mode), \
             mock.patch.object(sn.native, 'driver_version', return_value=loaded), \
             mock.patch.object(sn, 'installed_module_version', return_value=installed), \
             contextlib.redirect_stdout(out):
            sn.hook()
        return out.getvalue()

    def test_source_root_warns_and_does_not_run_legacy(self):
        backup = Path(self.tmp.name) / 'before-source-native-20261016-120000'
        touch(backup / 'waydroid.cfg', '[waydroid]\nnvidia_native_root = {}\n'.format(self.root).encode())
        text = self.run_hook(('source', self.root))
        for needle in ('verified on NVIDIA kernel module ' + V, 'loaded module is ' + V,
                       'is 615.78', 'refuse to start', 'stage /var/lib/waydroid/nvidia-source-native-',
                       'activate /var/lib/waydroid/nvidia-source-native-', ' restore ', 'was NOT run'):
            self.assertIn(needle, text)

    def test_no_checkpoint_is_said(self):
        text = self.run_hook(('source', self.root))
        self.assertIn('(no saved configuration matches', text)
        self.assertNotIn(' restore ', text)

    def test_unreadable_facts_are_reported_not_raised(self):
        (self.root / 'source-rm.json').write_text('not json')
        with mock.patch.object(sn, 'installed_module_version', side_effect=OSError('modinfo gone')):
            out = io.StringIO()
            with mock.patch.object(sn, 'hook_mode', return_value=('source', self.root)), \
                 contextlib.redirect_stdout(out):
                sn.hook()
        self.assertIn('verified on NVIDIA kernel module unknown (', out.getvalue())
        self.assertIn('unknown (modinfo gone)', out.getvalue())

    def test_unknown_mode_does_not_run_legacy(self):
        self.assertIn('Not running', self.run_hook(('unknown', 'cannot parse x')))

    def test_legacy_mode_execs_setup_unchanged(self):
        self.execv.stop()
        with mock.patch.object(sn, 'hook_mode', return_value=('legacy', None)), \
             mock.patch.object(sn.os, 'execv') as execv:
            sn.hook()
        execv.assert_called_once_with(sn.LEGACY_SETUP, [sn.LEGACY_SETUP, '--hook'])
        self.execv.start()


class AdoptTest(unittest.TestCase):
    """Format-1 tree whose sources equal the deployed bytes; adopt gates and rewrites it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        top = Path(self.tmp.name)
        self.root, self.build = top / 'root', top / 'build'
        proc = top / 'proc-version'
        proc.write_text(PROC.format(V))
        p = mock.patch.object(nn, 'PROC_VERSION', str(proc))
        p.start()
        self.addCleanup(p.stop)
        artifacts, sources = {}, {}
        for sub, bits in (('lib64', 64), ('lib', 32)):
            files = ['vendor_extra/{}/{}'.format(sub, n) for n in (nn.GBM_WRAPPER, nn.MINIGBM_CORE, nn.HAL,
                     'libvulkan_nouveau.so', 'libnv_bionic_native.so', nn.LIBDRM)]
            files += ['vendor/{}/egl/{}'.format(sub, n) for n in nn.EGL_ANGLE]
            for rel in files:
                data = ELF[bits] + rel.encode()
                touch(self.root / rel, data)
                touch(self.build / rel, data)
                artifacts[rel] = hashlib.sha256(data).hexdigest()
                sources[rel] = str(self.build / rel)
        self.manifest = {'format': 1, 'driver': 'mesa-nvk-rust-rm', 'graphics_abis': ['x86_64', 'x86'],
                         'artifacts': artifacts, 'sources': sources}
        (self.root / 'source-rm.json').write_text(json.dumps(self.manifest))
        self.gate = {'runner': 'tools/native-vulkan-regression.py --run', 'exit': 0, 'kernel_module': V,
                     'finished': 'now', 'log_dir': str(top / 'logs'),
                     'exercised': ['vendor_extra/lib64/libvulkan_nouveau.so'], 'cases': []}

    def adopt(self, **kw):
        with mock.patch.object(sn.os, 'geteuid', return_value=0), \
             mock.patch.object(sn, 'run_gate', return_value=self.gate) as gate, \
             contextlib.redirect_stdout(io.StringIO()):
            sn.adopt(self.root, **kw)
        return gate

    def test_matching_sources_have_no_mismatch(self):
        self.assertEqual(sn.source_mismatches(self.manifest), [])

    def test_mismatches_listed(self):
        touch(self.build / 'vendor_extra/lib64/libnv_bionic_native.so', b'rebuilt')
        os.unlink(self.build / 'vendor_extra/lib/libdrm.so.2')
        manifest = dict(self.manifest, sources=dict(self.manifest['sources']))
        del manifest['sources']['vendor/lib/egl/libEGL_angle.so']
        problems = sn.source_mismatches(manifest)
        self.assertEqual(len(problems), 3)
        joined = '\n'.join(problems)
        for needle in ('libnv_bionic_native.so', 'libdrm.so.2', 'libEGL_angle.so: no source recorded'):
            self.assertIn(needle, joined)
        self.assertEqual(sn.source_mismatches({'artifacts': {}}), ['manifest has no sources map'])

    def test_adopt_refuses_changed_build(self):
        touch(self.build / 'vendor_extra/lib64/libnv_bionic_native.so', b'rebuilt')
        with self.assertRaisesRegex(RuntimeError, 'differ from their recorded build sources'):
            self.adopt()
        self.assertEqual(json.loads((self.root / 'source-rm.json').read_text())['format'], 1)

    def test_adopt_requires_root_unless_dry_run(self):
        with mock.patch.object(sn.os, 'geteuid', return_value=1000):
            self.assertRaisesRegex(RuntimeError, 'root-owned manifest', sn.adopt, self.root)

    def test_dry_run_writes_nothing(self):
        before = (self.root / 'source-rm.json').read_bytes()
        with mock.patch.object(sn, 'gate_coverage', return_value=(['a'], ['b'])):
            gate = self.adopt(dry_run=True)
        gate.assert_not_called()
        self.assertEqual((self.root / 'source-rm.json').read_bytes(), before)
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ['source-rm.json', 'vendor', 'vendor_extra'])

    def test_adopt_writes_format2_and_keeps_old_manifest(self):
        before = (self.root / 'source-rm.json').read_bytes()
        self.adopt()
        manifest = nn.verify_source_rm(str(self.root))       # passes the generator's format-3 checks
        self.assertEqual((manifest['format'], manifest['kernel_module']), (nn.SOURCE_RM_FORMAT, V))
        self.assertEqual(manifest['media']['enabled'], False)
        self.assertIn('predates the media component', manifest['media']['reason'])
        self.assertEqual(manifest['gate'], self.gate)
        kept = [p for p in self.root.iterdir() if p.name.startswith('source-rm.json.format1-')]
        self.assertEqual(len(kept), 1)
        self.assertEqual(kept[0].read_bytes(), before)
        self.assertFalse([p for p in self.root.iterdir() if '.tmp-' in p.name])

    def test_adopt_passes_explicit_release_selection_to_gate(self):
        gate = self.adopt(rust_profile='release')
        self.assertEqual(gate.call_args.args[1], 'release')
        self.assertEqual(json.loads((self.root / 'source-rm.json').read_text())['rust_profile'], 'release')

    def test_adopt_refuses_formats_above_1(self):
        self.adopt()
        self.assertRaisesRegex(RuntimeError, 'already format {}'.format(nn.SOURCE_RM_FORMAT), self.adopt)


class GateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def gate(self, script, rust_profile="dev"):
        runner = Path(self.tmp.name) / 'runner.py'
        runner.write_text(script)
        with mock.patch.object(sn, 'RUNNER', runner), \
             mock.patch.object(sn, 'REPO', Path(self.tmp.name)), \
             mock.patch.object(sn, 'gate_coverage', return_value=(['x'], ['y'])), \
             mock.patch.object(sn.native, 'driver_version', return_value=V), \
             mock.patch.object(sn, '_runner_user', return_value=None), \
             contextlib.redirect_stdout(io.StringIO()):
            return sn.run_gate({}, rust_profile)

    def test_passing_run_recorded(self):
        record = self.gate('print("[host/compute]\\nenv x\\nexit=0 log=a\\n[android32/anb]\\nexit=0 log=b")\n')
        self.assertEqual([c['case'] for c in record['cases']], ['host/compute', 'android32/anb'])
        self.assertEqual((record['exit'], record['kernel_module'], record['exercised']), (0, V, ['x']))
        self.assertIn('gate-', record['log_dir'])

    def test_failing_case_refused(self):
        with self.assertRaisesRegex(RuntimeError, 'gate FAILED'):
            self.gate('import sys\nprint("[host/compute]\\nexit=1 log=a")\nsys.exit(1)\n')

    def test_release_gate_selection_reaches_runner_and_record(self):
        script = ('import sys\nassert sys.argv[sys.argv.index("--rust-profile") + 1] == "release"\n'
                  'print("[host/compute]\\nexit=0 log=a")\n')
        record = self.gate(script, 'release')
        self.assertEqual(record['rust_profile'], 'release')
        self.assertIn('--rust-profile release', record['runner'])

    def test_no_cases_refused(self):
        with self.assertRaisesRegex(RuntimeError, 'gate FAILED'):
            self.gate('print("nothing")\n')


class RustProfileTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.profiles = sn.profiles
        for name in ('src/lib.rs', 'Cargo.toml', 'Cargo.lock', 'mesa/nv_rm.h',
                     'mesa/nvkmd_rm.h', 'mesa/nvkmd_rm.c', 'mesa/build.py',
                     'mesa/build-compat.patch', 'mesa/rm-integration.patch'):
            touch(self.root / 'nvidia-prolapse/driver-source/native' / name, b'fixture source')
        touch(self.root / 'tools/native_rust_profile.py', b'fixture helper')
        self.runner = sn._load(REPO / 'tools/native-vulkan-regression.py', 'profile_regression_test')

    def record(self, target, profile):
        rust = self.profiles.rust_library(self.root, target, profile)
        nvk = self.root / f'build/nvk-{target}/src/nouveau/vulkan/libvulkan_nouveau.so'
        touch(rust, (target + profile).encode())
        touch(nvk, b'matching Mesa')
        return self.profiles.write_metadata(self.root, target, profile,
                                            ['cargo', 'build'], 'fixture compiler', {},
                                            self.profiles.source_snapshot(self.root))

    def test_profile_selects_same_target_path_for_stage_and_launch(self):
        for profile, directory in [('dev', 'debug'), ('release', 'release')]:
            with mock.patch.object(sn, 'REPO', self.root), mock.patch.object(self.runner, 'ROOT', self.root):
                sources = sn.stage_sources(profile)
                for target, sub, triple in [('android', 'lib64', 'x86_64-linux-android'),
                                            ('android32', 'lib', 'i686-linux-android')]:
                    expected = self.root / f'build/nv-rm-rust/{triple}/{directory}/libnv_bionic_native.so'
                    self.assertEqual(sources[f'vendor_extra/{sub}/libnv_bionic_native.so'], expected)
                    env, _ = self.runner.launch(target, ['fixture'], profile)
                    self.assertEqual(str(expected.parent), env['LD_LIBRARY_PATH'].split(':')[0])
                    other = 'release' if directory == 'debug' else 'debug'
                    self.assertNotIn(str(expected.parent.with_name(other)), env['LD_LIBRARY_PATH'].split(':'))
                env, _ = self.runner.launch('host', ['fixture'], profile)
                self.assertEqual(str(self.root / f'build/nv-rm-rust/{directory}'), env['LD_LIBRARY_PATH'].split(':')[0])
        with mock.patch.object(sn, 'REPO', self.root):
            self.assertEqual(sn.stage_sources(), sn.stage_sources('dev'))

    def test_metadata_rejects_profile_mix_stale_rust_and_stale_mesa(self):
        for target in self.profiles.TARGETS:
            self.record(target, 'release')
            self.assertEqual(self.profiles.verify_metadata(self.root, target, 'release')['rust_profile'], 'release')
            with self.assertRaisesRegex(RuntimeError, 'target/profile'):
                self.profiles.verify_metadata(self.root, target, 'dev')
            touch(self.profiles.rust_library(self.root, target, 'release'), b'rebuilt Rust')
            with self.assertRaisesRegex(RuntimeError, 'rust artifact path/hash'):
                self.profiles.verify_metadata(self.root, target, 'release')
            self.record(target, 'release')
            touch(self.root / f'build/nvk-{target}/src/nouveau/vulkan/libvulkan_nouveau.so', b'rebuilt Mesa')
            with self.assertRaisesRegex(RuntimeError, 'nvk artifact path/hash'):
                self.profiles.verify_metadata(self.root, target, 'release')

    def test_source_changes_refuse_staging_and_in_build_changes_refuse_metadata(self):
        self.record('host', 'release')
        old = self.profiles.source_snapshot(self.root)
        touch(self.root / 'nvidia-prolapse/driver-source/native/src/lib.rs', b'new source')
        with self.assertRaisesRegex(RuntimeError, 'source inputs changed'):
            self.profiles.verify_metadata(self.root, 'host', 'release')
        before = self.profiles.metadata_path(self.root, 'host').read_bytes()
        with self.assertRaisesRegex(RuntimeError, 'changed while building'):
            self.profiles.write_metadata(self.root, 'host', 'release', [], 'fixture', {}, old)
        self.assertEqual(self.profiles.metadata_path(self.root, 'host').read_bytes(), before)

    def test_changed_build_metadata_cannot_stamp_successfully_gated_new_bytes(self):
        source = self.root / 'fixture.so'
        touch(source, ELF[64] + b'old')
        destination = self.root / 'stage'
        changed = False
        def inspect(*args, **kwargs):
            nonlocal changed
            # Model rebuild after the initial metadata capture but before the
            # artifact hash used by the runtime gate is captured.
            touch(source, ELF[64] + b'new')
            changed = True
            return ['libc.so']
        with mock.patch.object(sn, 'stage_sources', return_value={'vendor_extra/lib64/fixture.so': source}), \
             mock.patch.object(sn.profiles, 'verify_metadata', side_effect=lambda *args: {'build': 'new' if changed else 'old'}), \
             mock.patch.object(sn, '_needed', side_effect=inspect), \
             mock.patch.object(sn, 'run_gate', return_value={'kernel_module': V}):
            with self.assertRaisesRegex(RuntimeError, 'metadata changed'):
                # graphics-only fixture: opt out of media and native GL explicitly (the real build/ may hold nvrm builds)
                sn.stage(destination, 'release', 'fixture: graphics only', 'fixture: graphics only')
        self.assertFalse(destination.exists(), 'failed staging must remove its partial tree')

    def test_missing_metadata_has_explicit_rebuild_remedy(self):
        with self.assertRaisesRegex(RuntimeError, '--target android --rust-profile release'):
            self.profiles.verify_metadata(self.root, 'android', 'release')
        with self.assertRaises(ValueError):
            self.profiles.rust_library(self.root, 'host', 'fast')



def graphics_files():
    out = {}
    for sub, bits in (('lib64', 64), ('lib', 32)):
        for n in (nn.GBM_WRAPPER, nn.MINIGBM_CORE, nn.HAL, 'libvulkan_nouveau.so', 'libnv_bionic_native.so', nn.LIBDRM):
            out['vendor_extra/{}/{}'.format(sub, n)] = ELF[bits]
        for n in nn.EGL_ANGLE:
            out['vendor/{}/egl/{}'.format(sub, n)] = ELF[bits]
    return out


ORIGINALS = {'build.prop': b'ro.odm.x=1\n', 'group': b'', 'passwd': b''}


def media_files():
    out = {'vendor_extra/lib64/' + n: ELF[64] for n in nn.MEDIA_LIBS}
    for rel in nn.MEDIA_ODM_FILES:
        out[rel] = ELF[64] if rel == nn.MEDIA_SERVICE else b'<text/>'
    for n, data in ORIGINALS.items():
        out['odm/etc/' + n] = data
    return out


MEDIA_GATE = {'exit': 0, 'kernel_module': V, 'finished': 'now', 'steps': []}
MEDIA_ON = {'enabled': True, 'vendor_img': '/images/vendor.img', 'system_img': '/images/system.img',
            'codecs': ['c2.nvprolapse.vp9.decoder'], 'gate': MEDIA_GATE}


class SourceRootFixture(unittest.TestCase):
    """A complete format-3 root on disk (fake ELF headers, real hashes) + fake /proc version and vendor.img."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        top = Path(self.tmp.name)
        self.root = top / 'root'
        proc = top / 'proc-version'
        proc.write_text(PROC.format(V))
        for patch in (mock.patch.object(nn, 'PROC_VERSION', str(proc)),
                      mock.patch.object(nn, 'vendor_odm_files', side_effect=self.image)):
            patch.start()
            self.addCleanup(patch.stop)
        self.image_files = {n: (0o644, d) for n, d in ORIGINALS.items()}

    def image(self, vendor_img):
        self.assertEqual(vendor_img, MEDIA_ON['vendor_img'])
        return dict(self.image_files)

    def build(self, media=MEDIA_ON, fmt=nn.SOURCE_RM_FORMAT, files=None):
        shutil.rmtree(self.root, ignore_errors=True)
        files = dict(graphics_files(), **(media_files() if files is None and media and media.get('enabled') else {}),
                     **(files or {}))
        artifacts = {}
        for rel, data in files.items():
            data = data + rel.encode() if data.startswith(b'\x7fELF') else data
            touch(self.root / rel, data)
            artifacts[rel] = hashlib.sha256(data).hexdigest()
        if (self.root / nn.MEDIA_SERVICE).exists():
            (self.root / nn.MEDIA_SERVICE).chmod(0o755)
        manifest = {'format': fmt, 'driver': 'mesa-nvk-rust-rm', 'graphics_abis': ['x86_64', 'x86'],
                    'artifacts': artifacts, 'kernel_module': V,
                    'gate': {'runner': 'r', 'exit': 0, 'kernel_module': V, 'finished': 'now', 'log_dir': '/l',
                             'exercised': ['vendor_extra/lib64/libvulkan_nouveau.so']}}
        if media is not None:
            manifest['media'] = media
        (self.root / 'source-rm.json').write_text(json.dumps(manifest))
        return manifest


class MediaManifestTest(SourceRootFixture):
    def nodes(self):
        return nn._source_rm_nodes(str(self.root))

    def test_media_root_mounts_odm_tree(self):
        nodes = self.nodes()
        self.assertEqual(nodes[-1], 'lxc.mount.entry = {} vendor/odm none rbind 0 0'.format(self.root / 'odm'))
        self.assertEqual(sum('vendor/odm' in n for n in nodes), 1)

    def test_explicit_opt_out_valid_without_odm(self):
        self.build(media={'enabled': False, 'reason': 'no NVDEC on this GPU'})
        self.assertFalse(any('odm' in n for n in self.nodes()))

    def test_format3_needs_media_record_and_reason(self):
        for media, needle in ((None, 'media'), ({'enabled': False}, 'reason'), ({'enabled': False, 'reason': ' '}, 'reason'),
                              ({'enabled': 'yes'}, 'boolean')):
            self.build(media=media)
            with self.assertRaisesRegex(OSError, needle):
                self.nodes()

    def test_format2_root_without_media_still_valid(self):
        self.build(media=None, fmt=2)
        self.assertFalse(any('odm' in n for n in self.nodes()))

    def test_media_files_in_root_without_media_refused(self):
        self.build(media=None, fmt=2)
        touch(self.root / 'odm/etc/media_codecs_c2.xml', b'x')
        self.assertRaisesRegex(OSError, 'predates the media component, but media files are present', self.nodes)
        self.build(media={'enabled': False, 'reason': 'r'})
        touch(self.root / 'vendor_extra/lib64/libcodec2_vndk.so', ELF[64])
        self.assertRaisesRegex(OSError, 'explicitly disabled', self.nodes)

    def test_media_gate_must_pass_on_pinned_module(self):
        self.build(media=dict(MEDIA_ON, gate=dict(MEDIA_GATE, kernel_module='600.1')))
        self.assertRaisesRegex(OSError, 'media gate', self.nodes)
        self.build(media=dict(MEDIA_ON, gate=dict(MEDIA_GATE, exit=1)))
        self.assertRaisesRegex(OSError, 'media gate', self.nodes)
        self.build(media=dict(MEDIA_ON, codecs=['c2.android.vp9.decoder']))
        self.assertRaisesRegex(OSError, 'codecs', self.nodes)

    def test_missing_media_lib_refused(self):
        self.build()
        os.unlink(self.root / 'vendor_extra/lib64/libcodec2_vndk.so')
        self.assertRaisesRegex(OSError, 'libcodec2_vndk.so', self.nodes)

    def test_unrecorded_file_in_odm_refused(self):
        self.build()
        touch(self.root / 'odm/etc/init/extra.rc', b'service x /x')
        self.assertRaisesRegex(OSError, 'must hold exactly', self.nodes)

    def test_service_must_be_executable(self):
        self.build()
        (self.root / nn.MEDIA_SERVICE).chmod(0o644)
        self.assertRaisesRegex(OSError, 'not executable', self.nodes)

    def test_image_odm_drift_refused(self):
        self.build()
        self.image_files['build.prop'] = (0o600, b'ro.odm.x=2\n')
        self.assertRaisesRegex(OSError, 'build.prop differs', self.nodes)
        self.image_files = {n: (0o644, d) for n, d in ORIGINALS.items()}
        self.image_files['vendor_overlay.prop'] = (0o644, b'')
        self.assertRaisesRegex(OSError, 'now holds', self.nodes)

    def test_lxc_nodes_checks_mediacodec_device_access(self):
        self.build()
        with mock.patch.object(nn, '_device_nodes', return_value=['dev']), \
             mock.patch.object(nn, 'nvidia_render_node', return_value='/dev/dri/renderD129'), \
             mock.patch.object(nn, 'check_media_device_access') as access:
            nodes = nn.lxc_nodes(str(self.root), dev='/fake')
        self.assertEqual(nodes[-1], 'dev')
        self.assertEqual(access.call_args.args[0][0], '/fake/nvidiactl')
        self.assertEqual(access.call_args.args[0][-1], '/dev/dri/renderD129')
        self.build(media={'enabled': False, 'reason': 'r'})
        with mock.patch.object(nn, '_device_nodes', return_value=['dev']), \
             mock.patch.object(nn, 'check_media_device_access') as access:
            nn.lxc_nodes(str(self.root), dev='/fake')
        access.assert_not_called()

    def setUp(self):
        super().setUp()
        self.build()


class MediaBasePropertiesTest(SourceRootFixture):
    """waydroid_base.prop gets the Codec2 listing only from a root that stages the decoder."""

    def props(self):
        return dict(nn.base_properties(str(self.root)))

    def test_media_root_lists_codec2_with_omx_software_first(self):
        self.build()
        props = self.props()
        self.assertEqual(props['debug.stagefright.ccodec'], '4')
        # c2.nvprolapse 0x10 < OMX.google.* < c2.android.* (audio 8, other 0x200)
        self.assertLess(int(props['debug.stagefright.omx_default_rank.sw-audio']), 8)
        self.assertGreater(int(props['debug.stagefright.omx_default_rank.sw-other']), 0x10)
        self.assertLess(int(props['debug.stagefright.omx_default_rank.sw-other']), 0x200)

    def test_root_without_media_keeps_waydroid_default(self):
        self.build(media={'enabled': False, 'reason': 'no NVDEC on this GPU'})
        self.assertEqual(self.props(), {})
        self.build(media=None, fmt=2)
        self.assertEqual(self.props(), {})

    def test_blob_root_has_none(self):
        self.build()
        os.unlink(self.root / 'source-rm.json')
        self.assertEqual(self.props(), {})

    def test_invalid_manifest_is_loud(self):
        self.build(media={'enabled': False})
        self.assertRaisesRegex(OSError, 'reason', self.props)
        self.build(fmt=1)
        self.assertRaisesRegex(OSError, 'format 1', self.props)


class DeviceAccessTest(unittest.TestCase):
    def test_world_rw_char_device_passes(self):
        nn.check_media_device_access(['/dev/null'])

    def test_regular_file_or_group_only_refused(self):
        with tempfile.NamedTemporaryFile() as f:
            os.chmod(f.name, 0o666)
            self.assertRaisesRegex(OSError, 'o\\+rw', nn.check_media_device_access, [f.name])
        real = os.stat('/dev/null')
        fake = os.stat_result((real.st_mode & ~0o007,) + tuple(real)[1:])
        with mock.patch.object(nn.os, 'stat', return_value=fake):
            self.assertRaisesRegex(OSError, 'NVreg_DeviceFileMode', nn.check_media_device_access, ['/dev/nvidiactl'])
        self.assertRaisesRegex(OSError, 'missing', nn.check_media_device_access, ['/nonexistent/nvidiactl'])
        with tempfile.NamedTemporaryFile() as f:
            self.assertRaisesRegex(OSError, 'o\\+rw. Remedy: restart it', nn.check_media_device_access, [f.name],
                                   remedy='restart it')


class MediaClosureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        top = Path(self.tmp.name)
        self.service, self.rust = top / 'svc', top / 'libnv_bionic_native.so'
        self.libs = [top / n for n in nn.MEDIA_LIBS]
        for p in [self.service, self.rust, *self.libs]:
            touch(p, ELF[64])
        self.needed = {'svc': ['libcodec2_vndk.so', 'libnv_bionic_native.so', 'libhwminijail.so', 'libc.so', 'libc++.so']}
        self.images = {('sys', sn.VNDK_APEX): {'libc++.so': 0, 'libcodec2.so': 0},
                       ('sys', '/system/lib64'): {'libc.so': 0, 'libfoo.so': 0},
                       ('ven', '/lib64'): {'libhwminijail.so': 0}}
        self.exports = {'svc': {'nv_video_decode', 'nv_video_query_caps'},
                        'libnv_bionic_native.so': {'nv_video_decode', 'nv_video_query_caps', 'nv_rm_x'}}

    def closure(self):
        nm = lambda path, flag: self.exports.get(Path(path).name, set())
        with mock.patch.object(nn, 'image_ls', side_effect=lambda img, path: self.images[(img, path)]), \
             mock.patch.object(sn, '_needed', side_effect=lambda p: self.needed.get(Path(p).name, ['libc.so'])), \
             mock.patch.object(sn, '_nm', side_effect=nm):
            return sn.media_closure(self.service, self.libs, self.rust, 'sys', 'ven')

    def test_resolves_each_needed_to_its_provider(self):
        closure, symbols = self.closure()
        self.assertEqual(closure['svc'], {'libcodec2_vndk.so': 'root vendor_extra/lib64',
                                          'libnv_bionic_native.so': 'root vendor_extra/lib64',
                                          'libhwminijail.so': 'vendor.img /lib64', 'libc.so': 'llndk /system/lib64',
                                          'libc++.so': 'vndk apex'})
        self.assertEqual(symbols, ['nv_video_decode', 'nv_video_query_caps'])

    def test_unresolved_needed_refused(self):
        self.needed['svc'].append('libfoo.so')        # in /system/lib64 but not LLNDK: not reachable
        self.assertRaisesRegex(RuntimeError, 'svc NEEDS libfoo.so', self.closure)

    def test_glibc_or_nvidia_dependency_refused(self):
        self.needed['svc'] = self.needed['svc'] + ['libnvidia-glcore.so.580.0']
        with self.assertRaisesRegex(RuntimeError, 'Non-bionic/proprietary dependency in media artifact'):
            self.closure()

    def test_shadowed_library_refused(self):
        self.images[('ven', '/lib64')]['libcodec2_vndk.so'] = 0
        self.assertRaisesRegex(RuntimeError, 'would load those instead', self.closure)

    def test_backend_without_nv_video_refused(self):
        self.exports['libnv_bionic_native.so'] = {'nv_rm_x'}
        self.assertRaisesRegex(RuntimeError, 'lacks nv_video symbols', self.closure)


class StageMediaTest(SourceRootFixture):
    """stage() with both gates mocked: what lands in the root and the manifest."""

    def stage(self, no_media=None, gl=None):
        build = Path(self.tmp.name) / 'build'
        sources = {}
        for rel, data in graphics_files().items():
            touch(build / rel, data + rel.encode())
            sources[rel] = build / rel
        media_srcs, odm = {}, {}
        for rel, data in media_files().items():
            path = build / 'media' / rel
            touch(path, data + rel.encode() if data.startswith(b'\x7fELF') else data)
            (odm if rel.startswith('odm/') else media_srcs)[rel] = path
        (build / 'media' / nn.MEDIA_SERVICE).chmod(0o755)
        (build / 'media/odm/etc/build.prop').chmod(0o600)
        service = build / 'media' / nn.MEDIA_SERVICE
        gate = {'runner': 'r', 'exit': 0, 'kernel_module': V, 'finished': 'now', 'log_dir': '/l',
                'exercised': ['vendor_extra/lib64/libvulkan_nouveau.so']}
        dest = Path(self.tmp.name) / 'staged'
        with mock.patch.object(sn, 'stage_sources', return_value=sources), \
             mock.patch.object(sn, 'media_sources', return_value=media_srcs), \
             mock.patch.object(sn, 'MEDIA_SERVICE_BIN', service), \
             mock.patch.object(sn, 'run_media_gate', return_value=(MEDIA_ON, odm)) as media_gate, \
             mock.patch.object(sn, 'run_gate', return_value=gate) as vk_gate, \
             mock.patch.object(sn, 'run_native_gl_gate', return_value=gl or (
                 {'enabled': False, 'reason': 'GL driver not built'}, {})), \
             mock.patch.object(sn.profiles, 'verify_metadata', return_value={}), \
             mock.patch.object(sn, '_needed', return_value=['libc.so', 'libnv_bionic_native.so']), \
             contextlib.redirect_stdout(io.StringIO()):
            sn.stage(dest, 'release', no_media)
        self.assertEqual(set(vk_gate.call_args.args[0]), set(graphics_files()), 'Vulkan gate gets graphics only')
        return dest, media_gate

    def test_default_stages_media_with_modes(self):
        dest, media_gate = self.stage()
        media_gate.assert_called_once()
        manifest = nn.verify_source_rm(str(dest))
        self.assertEqual(manifest['format'], 3)
        self.assertTrue(nn.media_enabled(manifest))
        self.assertEqual((dest / nn.MEDIA_SERVICE).stat().st_mode & 0o777, 0o755)
        self.assertEqual((dest / 'odm/etc/build.prop').stat().st_mode & 0o777, 0o600)
        self.assertEqual((dest / 'vendor_extra/lib64/libcodec2_vndk.so').stat().st_mode & 0o777, 0o644)
        self.assertIn('vendor/odm', nn._source_rm_nodes(str(dest))[-1])

    def test_native_gl_record_lands_in_the_manifest(self):
        dest, _ = self.stage()
        manifest = nn.verify_source_rm(str(dest))
        self.assertEqual(manifest['native_gl'], {'enabled': False, 'reason': 'GL driver not built'})
        gl_src = Path(self.tmp.name) / 'gl'
        files = {}
        for rel, bits in nn.native_gl_files({'graphics_abis': ['x86_64', 'x86']}).items():
            touch(gl_src / rel, ELF[bits] + rel.encode())
            files[rel] = gl_src / rel
        rust = {abi: hashlib.sha256(graphics_files()[f'vendor_extra/{sub}/libnv_bionic_native.so'] +
                                    f'vendor_extra/{sub}/libnv_bionic_native.so'.encode()).hexdigest()
                for abi, (sub, _) in nn.ABI_DIRS.items()}
        record = {'enabled': True, 'egl': 'nvrm', 'files': {r: sn.sha256(p) for r, p in files.items()},
                  'gate': {'runner': 'smoke', 'exit': 0, 'kernel_module': V, 'finished': 'now', 'log_dir': '/l',
                           'rust_sha256': rust}}
        shutil.rmtree(Path(self.tmp.name) / 'staged')
        dest, _ = self.stage(gl=(record, files))
        manifest = nn.verify_source_rm(str(dest))
        self.assertTrue(nn.native_gl_enabled(manifest))
        self.assertEqual(nn._prolapse_gl(str(dest)), 'nvrm')
        self.assertEqual((dest / 'vendor_extra/lib/libgallium_dri_nvrm.so').stat().st_mode & 0o777, 0o644)

    def test_no_media_is_explicit(self):
        dest, media_gate = self.stage('decoder not wanted on this host')
        media_gate.assert_not_called()
        manifest = nn.verify_source_rm(str(dest))
        self.assertEqual(manifest['media'], {'enabled': False, 'reason': 'decoder not wanted on this host'})
        self.assertFalse((dest / 'odm').exists())
        with self.assertRaisesRegex(RuntimeError, 'non-empty reason'):
            self.stage('  ')


class NativeGlGateTest(unittest.TestCase):
    """run_native_gl_gate(): fake nvrm builds (fake ELF + real hashes), strip and smoke mocked."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name)
        self.sources = {}
        for rel, data in graphics_files().items():
            touch(self.repo / 'staged-src' / rel, data + rel.encode())
            self.sources[rel] = self.repo / 'staged-src' / rel
        touch(self.repo / 'tools/llvm-strip', b'')
        touch(self.repo / 'smoke.py', b'')
        self.smoke_env = None

    def build(self, rust_sha=None, targets=('android', 'android32')):
        for target, sub, abi, triple in sn.NVRM_TARGETS:
            if target not in targets:
                continue
            base = self.repo / f'build/nvrm-{target}'
            arts = {}
            for name, subdir in sn.NVRM_BUILD_ARTIFACTS.items():
                data = ELF[64 if sub == 'lib64' else 32] + name.encode() + b' debug'
                touch(base / subdir / name, data)
                arts[name] = {'path': str(base / subdir / name), 'sha256': hashlib.sha256(data).hexdigest()}
            rust = self.sources[f'vendor_extra/{sub}/libnv_bionic_native.so']
            touch(base / 'nvrm-build.json', json.dumps({
                'target': target, 'egl_suffix': '_nvrm', 'artifacts': arts, 'driver_source_head': 'abc',
                'driver_source_dirty': False, 'rust': {'path': '/r', 'sha256': rust_sha or sn.sha256(rust)}}).encode())

    def strip(self, tool, flag, o, out, built):
        Path(out).write_bytes(Path(built).read_bytes().replace(b' debug', b''))

    def smoke(self, cmd, env=None, **kw):
        self.smoke_env = env
        fake = Path(env['PROLAPSE_ROOT'])
        for target, sub, abi, triple in sn.NVRM_TARGETS:     # the runner sees the stripped staged bytes
            assert not (fake / f'build/nvrm-{target}/src/egl/libEGL_nvrm.so').read_bytes().endswith(b'debug')
            assert (fake / f'build/nv-rm-rust/{triple}/release/libnv_bionic_native.so').read_bytes() == \
                self.sources[f'vendor_extra/{sub}/libnv_bionic_native.so'].read_bytes()
        return mock.Mock(returncode=self.smoke_rc, stdout='ANDROID SMOKE ' + ('PASS' if self.smoke_rc == 0 else 'FAIL'))

    def gate(self, smoke_rc=0):
        self.smoke_rc = smoke_rc
        with mock.patch.object(sn, 'REPO', self.repo), \
             mock.patch.object(sn, 'NDK_BIN', self.repo / 'tools'), \
             mock.patch.object(sn, 'NVRM_SMOKE', self.repo / 'smoke.py'), \
             mock.patch.object(sn, 'run', side_effect=self.strip), \
             mock.patch.object(sn.subprocess, 'run', side_effect=self.smoke), \
             mock.patch.object(sn, '_needed', return_value=['libc.so', 'libnv_bionic_native.so']), \
             mock.patch.object(sn.native, 'driver_version', return_value=V), \
             mock.patch.object(sn, '_runner_user', return_value=None), \
             mock.patch.object(sn.native, '_elf_class', side_effect=lambda p: 64 if Path(p).read_bytes()[4] == 2 else 32), \
             contextlib.redirect_stdout(io.StringIO()):
            return sn.run_native_gl_gate(self.sources, 'release')

    def test_not_built_is_an_explicit_record(self):
        record, files = self.gate()
        self.assertEqual((record['enabled'], files), (False, {}))
        self.assertIn('GL driver not built', record['reason'])
        self.assertIn('build.py --gl', record['reason'])

    def test_built_and_passing_is_staged_stripped(self):
        self.build()
        record, files = self.gate()
        self.assertTrue(record['enabled'])
        self.assertEqual(record['egl'], 'nvrm')
        self.assertEqual(set(files), set(nn.native_gl_files({'graphics_abis': ['x86_64', 'x86']})))
        for rel, path in files.items():
            self.assertEqual(record['files'][rel], sn.sha256(path))
            self.assertNotIn(b'debug', path.read_bytes())
        self.assertEqual(record['gate']['rust_sha256']['x86'],
                         sn.sha256(self.sources['vendor_extra/lib/libnv_bionic_native.so']))
        self.assertEqual((record['gate']['exit'], record['gate']['kernel_module']), (0, V))
        self.assertNotIn('NVRM_GL_DIR_SUFFIX', self.smoke_env)

    def test_rust_library_mismatch_refused_with_rebuild_command(self):
        self.build(rust_sha='f' * 64)
        with self.assertRaisesRegex(RuntimeError, 'linked against libnv_bionic_native.so f{64}.*build.py --gl'):
            self.gate()

    def test_partial_or_stale_build_refused(self):
        self.build(targets=('android',))
        self.assertRaisesRegex(RuntimeError, 'only; both ABIs', self.gate)
        self.build()
        touch(self.repo / 'build/nvrm-android/src/egl/libEGL_nvrm.so', ELF[64] + b'newer')
        self.assertRaisesRegex(RuntimeError, 'differs from its build record', self.gate)

    def test_failing_smoke_refused(self):
        self.build()
        self.assertRaisesRegex(RuntimeError, 'smoke test FAILED', self.gate, 1)


class MediaStatusTest(SourceRootFixture):
    def status(self, probe, debug='', logcat='', media=MEDIA_ON):
        self.build(media=media)
        lines = []
        replies = iter([mock.Mock(returncode=0, stdout=probe, stderr=''),
                        mock.Mock(returncode=0, stdout=debug, stderr=''),
                        mock.Mock(returncode=0, stdout=logcat, stderr='')])
        sn.media_status(self.root, 1.0, lambda s, w, d: lines.append((w, s)), lambda script: next(replies))
        return dict(lines)

    RUNNING = 'selinux=Permissive\nstate=running\npid=1234\nlshal=listed\n'
    DUMP = ('Beginning of dump -- C2ComponentStore: nvprolapse\n  Supported components:\n'
            '    name: c2.nvprolapse.vp9.decoder\n')

    def test_running_and_listed_pass(self):
        got = self.status(self.RUNNING, self.DUMP, 'I CCodec: c2.nvprolapse.vp9.decoder created')
        self.assertEqual(got, {'media manifest': 'PASS', 'guest SELinux': 'PASS', 'c2 service': 'PASS',
                               'c2 store components': 'PASS', 'Codec2 framework list': 'PASS'})

    def test_not_registered_or_component_mismatch_fail(self):
        got = self.status(self.RUNNING.replace('listed', 'absent'))
        self.assertEqual((got['c2 service'], got['c2 store components']), ('FAIL', 'FAIL'))
        got = self.status(self.RUNNING, self.DUMP.replace('vp9', 'av1'))
        self.assertEqual(got['c2 store components'], 'FAIL')
        self.assertEqual(got['Codec2 framework list'], 'UNKNOWN')

    def test_enforcing_selinux_fails_with_media(self):
        self.assertEqual(self.status(self.RUNNING.replace('Permissive', 'Enforcing'), self.DUMP)['guest SELinux'], 'FAIL')

    def test_root_without_media_must_not_run_service(self):
        off = {'enabled': False, 'reason': 'r'}
        self.assertEqual(self.status('selinux=Disabled\nstate=\npid=\nlshal=absent\n', media=off)['c2 service'], 'PASS')
        self.assertEqual(self.status(self.RUNNING, media=off)['c2 service'], 'FAIL')


class ShaderCacheMigrationTest(unittest.TestCase):
    """invalidate_caches(): a fresh `waydroid init` leaves config_session empty (no session ever ran)."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.top = Path(self._tmp.name)
        self.session = self.top / 'config_session'
        self.backup = self.top / 'backup'
        for p in (mock.patch.object(sn.os, 'geteuid', lambda: 0),
                  mock.patch.object(sn, 'container_state', lambda: 'STOPPED')):
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self._tmp.cleanup()

    def run_it(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            sn.invalidate_caches(self.backup, session=self.session)
        return out.getvalue()

    def test_missing_or_empty_session_means_no_caches(self):
        self.assertIn('No previous user session', self.run_it())
        self.session.write_text('')                    # what set_lxc_config leaves after `waydroid init`
        self.assertIn('No previous user session', self.run_it())
        self.assertFalse(self.backup.exists())

    def test_session_caches_are_moved_aside(self):
        data = self.top / 'data'
        cache = data / 'data/com.example/code_cache/com.android.skia.shaders_cache'
        cache.parent.mkdir(parents=True)
        cache.write_bytes(b'x')
        self.session.write_text(f'lxc.mount.entry = {data} data none rbind 0 0\n')
        self.assertIn('Preserved 1 obsolete shader caches', self.run_it())
        self.assertFalse(cache.exists())
        self.assertTrue((self.backup / 'shader-caches' / cache.relative_to(data)).is_file())

    def test_session_without_a_data_mount_is_refused(self):
        self.session.write_text('lxc.mount.entry = tmpfs run/xdg none create=dir 0 0\n')
        with self.assertRaises(RuntimeError):
            self.run_it()


if __name__ == '__main__':
    unittest.main()
