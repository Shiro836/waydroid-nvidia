#!/usr/bin/env python3
"""Collect the nvidia-prolapse release payload from a finished build (packaging/native/build-native.sh).

  packaging/native/collect.py OUT_DIR [GATE_DIR]      GATE_DIR default build/native-gate (build-gate.py)

Writes OUT_DIR/payload/ in the staged-root layout dev/source-native stages (vendor_extra/lib{,64}/...,
vendor/lib{,64}/egl/*_nvrm.so, the Codec2 vendor libraries and service), OUT_DIR/gate/ (the CI-built
install-time gate programs, references and SPIR-V) and OUT_DIR/BUILD-RECORD.json (format 2): sha256 of
every payload/ and gate/ file (and, under "glibc_shim_files", of glibc-shim mode's glibc-shim/ tree:
collect_glibc_shim(); under "driver_files", of driver/: the driver's install-time media gate files), the
commit, the driver commit and dirty flag (tools/driver_source.py), the Mesa pin, the Rust build metadata
and the Rust RM library each nvrm ABI was linked against. The source list is dev/source-native's own (stage_sources,
media_sources, the nvrm build artifacts), release profile; nvrm is debug-stripped exactly as a local
stage strips it (llvm-strip --strip-debug). ANGLE comes from the angle job, not from here. Every input
must exist and have the expected ELF class; nothing is skipped.

On the user's machine `waydroid-nvidia-setup` hash-checks this bundle and gates it on the loaded kernel
module with packaging/native/release_gate.py (dev/source-native stage-release) before activating it.
"""
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import driver_source  # noqa: E402
DRIVER = driver_source.checkout()
# Files of the driver an installed package runs (packaging/native/release_gate.py media cases, the Rust build
# record reader dev/source-native imports), shipped in the bundle under driver/ (driver_source.checkout() of a
# packaged install).
DRIVER_FILES = ('media/stage-odm.sh', 'media/test/validate_xml.py', 'tools/native_rust_profile.py')
DRIVER_TREES = ('media/odm',)


def load(path, name):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build_environment():
    """What built this bundle: the build-environment image (packaging/native/Containerfile) and its package
    versions, the Rust toolchain and both NDK revisions. Outside the container: says so."""
    def out(*cmd):
        return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()

    def ndk(path):
        props = Path(path) / 'source.properties'
        lines = props.read_text().splitlines() if props.is_file() else []
        return next((l.split('=', 1)[1].strip() for l in lines if l.startswith('Pkg.Revision')), None)
    image = os.environ.get('NATIVE_BUILDENV')
    env = {'container_image': image if image and image != 'host' else None,
           'containerfile_sha256': os.environ.get('NATIVE_BUILDENV_FILE_SHA256'),
           'rustc': out('rustc', '--version'), 'cargo': out('cargo', '--version'),
           'ndk_r29': ndk(REPO / 'build/hwcomposer-work/ndk/android-ndk-r29'), 'ndk_r30': ndk('/opt/android-ndk')}
    if env['container_image']:
        env['packages'] = dict(l.split(' ', 1) for l in out('pacman', '-Q').splitlines() if ' ' in l)
    else:
        env['note'] = 'built on the host (NATIVE_BUILDENV=host), not in the build container'
    return env


# glibc-shim mode's guest side (packaging/native/glibc_shim.py reads this tree): built by build-native.sh's
# glibc-shim target from nvidia-prolapse/{shim,egl} and the driver's hal, gralloc and libdrm. NVIDIA's libraries are
# never in it.
GLIBC_SHIM_SHIMS = {'lib64': ('out', ('libnvglibc.so', 'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'librt.so.1',
                                      'libdl.so.2', 'ld-linux-x86-64.so.2')),
                    'lib': ('out32', ('libnvglibc.so', 'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'librt.so.1',
                                      'libdl.so.2', 'ld-linux.so.2'))}


def glibc_shim_sources():
    """{path under glibc-shim/: source} for glibc-shim mode."""
    p = REPO / 'nvidia-prolapse'
    src = {}
    for sub, (shim_out, shims) in GLIBC_SHIM_SHIMS.items():
        android = 'android' if sub == 'lib64' else 'android32'
        lib = f'root/vendor_extra/{sub}'
        for name in shims:
            src[f'{lib}/{name}'] = p / 'shim' / shim_out / name
        src[f'{lib}/hw/vulkan.nvidia.so'] = DRIVER / 'hal/out' / android / 'vulkan.nvidia.so'
        src[f'{lib}/libgbm_mesa_wrapper.so'] = DRIVER / 'gralloc/out' / android / 'libgbm_mesa_wrapper.so'
        src[f'{lib}/libminigbm_gralloc_gbm_mesa.so'] = DRIVER / 'gralloc/out' / android / 'libminigbm_gralloc_gbm_mesa.so'
        src[f'{lib}/libdrm.so.2'] = DRIVER / 'libdrm' / ('out' if sub == 'lib64' else 'out32') / 'libdrm.so.2'
        src[f'{lib}/libnvnative-egl-android.so'] = p / 'egl/out' / android / 'libnvnative-egl-android.so'
        src[f'{lib}/nvnative_egl_android.json'] = p / 'egl/nvnative_egl_android.json'
        src[f'{lib}/nvglibc-default-env'] = p / 'egl/nvglibc-default-env'
        for front in ('libEGL_nvnative.so', 'libGLESv1_CM_nvnative.so', 'libGLESv2_nvnative.so'):
            src[f'root/vendor/{sub}/egl/{front}'] = p / 'egl/out' / android / front
    src['check/census-x86_64.tsv'] = p / 'shim/gen/census.tsv'
    src['check/census-x86.tsv'] = p / 'shim/gen32/census.tsv'
    return src


def collect_glibc_shim(out, sn, problems):
    """Copy glibc-shim mode's tree into out/glibc-shim; {relative path: sha256} for BUILD-RECORD.json."""
    sources = glibc_shim_sources()
    for rel, path in sorted(sources.items()):
        want = 32 if '/lib/' in rel else 64
        if not path.is_file():
            problems.append(f'missing {path} (glibc-shim {rel}; build-native.sh target glibc-shim)')
        elif rel.endswith('.so') or '.so.' in rel:
            if sn.native._elf_class(str(path)) != want:
                problems.append(f'{path} is not ELF{want} (glibc-shim {rel})')
    if not (REPO / 'nvidia-prolapse/shim/out/.checked').is_file() or not (REPO / 'nvidia-prolapse/shim/out32/.checked').is_file():
        problems.append('the shims are not a checked build (make -C nvidia-prolapse/shim [ARCH=x86])')
    if problems:
        return None
    files = {}
    for rel, path in sorted(sources.items()):
        dest = out / 'glibc-shim' / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
        dest.chmod(0o644)
        files['glibc-shim/' + rel] = sha256(dest)
    for sub, arch in (('lib64', 'x86_64'), ('lib', 'x86')):
        lib = out / 'glibc-shim/root/vendor_extra' / sub / 'libdrm.so.2'
        nm = subprocess.run([str(sn.NDK_BIN / 'llvm-nm'), '-D', '--defined-only', '--format=just-symbols', str(lib)],
                            capture_output=True, text=True, check=True).stdout.split()
        names = sorted({n.split('@')[0] for n in nm if n.startswith('drm')})
        dest = out / 'glibc-shim/check' / f'libdrm-{arch}.names'
        dest.write_text(''.join(n + '\n' for n in names))
        dest.chmod(0o644)
        files[f'glibc-shim/check/libdrm-{arch}.names'] = sha256(dest)
    return files


def collect_driver_files(out):
    """Copy DRIVER_FILES and DRIVER_TREES into out/driver; {relative path: sha256} for BUILD-RECORD.json."""
    rels = list(DRIVER_FILES)
    for tree in DRIVER_TREES:
        rels += sorted(str(p.relative_to(DRIVER)) for p in (DRIVER / tree).rglob('*') if p.is_file())
    files = {}
    for rel in rels:
        src = DRIVER / rel
        if not src.is_file():
            raise SystemExit(f'collect: {src} missing in the driver checkout')
        dest = out / 'driver' / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dest)
        dest.chmod(0o755 if os.access(src, os.X_OK) else 0o644)
        files['driver/' + rel] = sha256(dest)
    return files


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    out = Path(sys.argv[1]).resolve()
    gate_dir = Path(sys.argv[2] if len(sys.argv) == 3 else REPO / 'build/native-gate').resolve()
    sn = load(REPO / 'dev/source-native', 'source_native')
    native = sn.native
    profile = 'release'
    sources = {rel: Path(p) for rel, p in sn.stage_sources(profile).items() if not rel.startswith('vendor/')}
    sources.update(sn.media_sources())
    sources[native.MEDIA_SERVICE] = sn.MEDIA_SERVICE_BIN
    strip_names, nvrm_rust, nvrm_builds = set(), {}, {}
    problems = []
    for target, sub, abi, _triple in sn.NVRM_TARGETS:
        build = sn.nvrm_build_dirs()[target]
        record = json.loads((build / 'nvrm-build.json').read_text())
        rust = sources[f'vendor_extra/{sub}/libnv_bionic_native.so']
        if record['rust']['sha256'] != sha256(rust):
            problems.append(f'nvrm {target} was linked against libnv_bionic_native.so {record["rust"]["sha256"]}, '
                            f'the payload ships {sha256(rust)} ({rust}); rebuild nvrm after NVK (build-native.sh)')
        nvrm_rust[abi] = record['rust']['sha256']
        nvrm_builds[abi] = {k: record.get(k) for k in ('driver_source_head', 'driver_source_dirty', 'mesa_pin',
                                                       'rust_profile')}
        for name, subdir in sn.NVRM_BUILD_ARTIFACTS.items():
            built = build / subdir / name
            if record['artifacts'].get(name, {}).get('sha256') != (sha256(built) if built.is_file() else None):
                problems.append(f'{built} differs from its build record {build / "nvrm-build.json"}')
            sources[sn.nvrm_staged_path(name, sub)] = built
            strip_names.add(sn.nvrm_staged_path(name, sub))
    gate_files = {f'gate/{p.relative_to(gate_dir)}': p for p in sorted(gate_dir.rglob('*')) if p.is_file()}
    for want in ('gate/bin/android/vk-compute', 'gate/bin/android32/gl-yuv', 'gate/bin/android/nvc2-xmlgen',
                 'gate/ref/ref-flat.rgba', 'gate/data/generate.spv'):
        if want not in gate_files:
            problems.append(f'gate program {want} missing in {gate_dir} (packaging/native/build-gate.py)')
    for rel, path in sorted(sources.items()):
        want = 64 if ('/lib64/' in rel or rel.startswith('odm/')) else 32
        if not path.is_file():
            problems.append(f'missing {path} (for {rel})')
        elif native._elf_class(str(path)) != want:
            problems.append(f'{path} is not ELF{want} (for {rel})')
    if problems:
        raise SystemExit('collect: build incomplete; run packaging/native/build-native.sh:\n  ' + '\n  '.join(problems))
    rust_builds = {t: sn.profiles.verify_metadata(DRIVER, t, profile) for t in sn.profiles.TARGETS}
    if out.exists():
        raise SystemExit(f'collect: {out} exists; use a new directory')
    glibc_shim = collect_glibc_shim(out, sn, problems)
    if problems:
        raise SystemExit('collect: build incomplete; run packaging/native/build-native.sh:\n  ' + '\n  '.join(problems))
    payload = out / 'payload'
    files = {}
    for rel, path in sorted(sources.items()):
        dest = payload / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if rel in strip_names:
            subprocess.run([str(sn.NDK_BIN / 'llvm-strip'), '--strip-debug', '-o', str(dest), str(path)], check=True)
        else:
            shutil.copyfile(path, dest)
        dest.chmod(0o755 if rel == native.MEDIA_SERVICE else 0o644)
        files['payload/' + rel] = sha256(dest)
    for rel, path in gate_files.items():
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
        dest.chmod(0o755 if '/bin/' in rel else 0o644)
        files[rel] = sha256(dest)
    commit = subprocess.run(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    dirty = bool(subprocess.run(['git', '-C', str(REPO), 'status', '--porcelain', '--untracked-files=no'],
                                capture_output=True, text=True).stdout.strip())
    driver = driver_source.identity()
    driver_files = collect_driver_files(out)
    mesa_pin = sn.re.search(r'^PIN = "([0-9a-f]{40})"', (DRIVER / 'mesa/build.py').read_text(), sn.re.M).group(1)
    record = {'format': 2, 'component': 'nvidia-prolapse native stack (mesa-nvk-rust-rm + nvrm + Codec2 NVDEC)',
              'commit': commit, 'dirty': dirty, 'release': os.environ.get('RELEASE_TAG') or None,
              'driver': driver, 'driver_files': driver_files,
              'mesa_commit': mesa_pin, 'rust_profile': profile, 'rust_builds': rust_builds,
              'nvrm_rust_sha256': nvrm_rust, 'nvrm_builds': nvrm_builds,
              'build_environment': build_environment(),
              'files': files, 'install_gate': 'packaging/native/release_gate.py (waydroid-nvidia-setup)',
              'glibc_shim_files': glibc_shim,
              'glibc_shim_check': 'packaging/native/glibc_shim.py (waydroid-nvidia-setup --mode glibc-shim)'}
    (out / 'BUILD-RECORD.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'collected {len(files)} files into {out} (commit {commit[:12]}{" dirty" if dirty else ""}; driver '
          f'{driver["commit"][:12]}{" dirty" if driver["dirty"] else ""}'
          f'{"" if driver["matches_pin"] else ", not the pinned " + driver["pinned_commit"][:12]})')


if __name__ == '__main__':
    main()
