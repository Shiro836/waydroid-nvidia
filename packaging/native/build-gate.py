#!/usr/bin/env python3
"""Build the install-time gate programs of the nvidia-prolapse release (CI, after build-native.sh's builds).

  packaging/native/build-gate.py OUT_DIR          default OUT_DIR=build/native-gate

The developer stage (dev/source-native stage) compiles its test programs while it gates; a user's
machine has no compiler. This builds the same programs once, from the same sources, with the pinned
NDK r29 (API 33) for both Android ABIs, so packaging/native/release_gate.py can run them on the user's
GPU and kernel module against the shipped payload:

  OUT/bin/<target>/   target = android (x86_64) | android32 (i686)
    vk-compute      tools/native-vulkan-probe.c        (compute + copy, --negative)
    vk-graphics     tools/native-vulkan-offscreen.c    (triangle -> tiled image -> readback)
    vk-interop      tools/native-vulkan-interop.c      (dma-buf + sync-fd between two devices)
    anb-roundtrip, astc-roundtrip, etc2-roundtrip   the driver's Vulkan HAL tests (hal/build-nvk-tests.py output)
    gl-triangle     <driver>/mesa/tests/gl-g0b-triangle.c   (ES 3.0 clear/triangle/quad + readback)
    gl-dmabuf       <driver>/mesa/tests/gl-dmabuf.c         (-DNVRM_NO_VULKAN; EGL dma-buf + native fences)
    gl-skia-shaders <driver>/mesa/tests/gl-skia-shaders.c   (SurfaceFlinger RenderEngine shader features)
    gl-yuv          <driver>/mesa/tests/gl-yuv.c            (NV12/P010 EGLImage import + external sampling)
    nvc2-xmlgen     <driver>/media (android only)           (Codec2 caps xml from the GPU, bionic build)
  <driver> = the driver checkout build-native.sh built (tools/driver_source.py).
  OUT/data/         generate.spv transform.spv vertex.spv fragment.spv (glslangValidator, spirv-val)
  OUT/ref/          ref-flat.rgba ref-grad.rgba skia/   llvmpipe references (host Mesa, surfaceless),
                    exactly what gl-android-smoke.py renders on the developer host before comparing

Every output is checked for its ELF class/machine; GL programs link nvrm's EGL/GLES by SONAME and
resolve them from the payload at gate time. Nothing here touches a GPU.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import driver_source  # noqa: E402
DRIVER = driver_source.checkout()
TESTS = DRIVER / 'mesa/tests'
NDK = REPO / 'build/hwcomposer-work/ndk/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin'
TARGETS = {'android': ('x86_64-linux-android', 'lib64', 64), 'android32': ('i686-linux-android', 'lib', 32)}
WARN = ['-std=c11', '-Wall', '-Wextra', '-Werror']
sys.path.insert(0, str(DRIVER / 'tools'))
import native_rust_profile as profiles  # noqa: E402  (the driver's own Rust build records)


def run(*cmd, **kw):
    print('+ ' + ' '.join(map(str, cmd)), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def must(path):
    path = Path(path)
    if not path.exists():
        raise SystemExit(f'build-gate: missing {path} (run packaging/native/build-native.sh first)')
    return path


def elf_check(path, bits):
    header = subprocess.check_output(['readelf', '-h', str(path)], text=True)
    machine = 'Advanced Micro Devices X86-64' if bits == 64 else 'Intel 80386'
    if f'ELF{bits}' not in header or machine not in header:
        raise SystemExit(f'build-gate: {path} is not an ELF{bits} {machine} object')


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else REPO / 'build/native-gate').resolve()
    if out.exists():
        shutil.rmtree(out)
    data, ref = out / 'data', out / 'ref'
    data.mkdir(parents=True)
    ref.mkdir()
    shaders = REPO / 'tools/native-vulkan-shaders'
    for name in ('generate', 'transform'):
        run('glslangValidator', '-V', '--target-env', 'vulkan1.0', shaders / f'{name}.comp', '-o', data / f'{name}.spv')
        run('spirv-val', '--target-env', 'vulkan1.0', data / f'{name}.spv')
    for stage, name in (('vert', 'vertex'), ('frag', 'fragment')):
        run('glslangValidator', '-V', shaders / f'offscreen.{stage}', '-o', data / f'{name}.spv')
        run('spirv-val', data / f'{name}.spv')
    mesa_include = must(DRIVER / 'build/mesa-gl-source/include')
    for target, (triple, sub, bits) in TARGETS.items():
        cc = must(NDK / f'{triple}33-clang')
        cxx = must(NDK / f'{triple}33-clang++')
        bindir = out / 'bin' / target
        bindir.mkdir(parents=True)
        for exe, source in (('vk-compute', 'native-vulkan-probe.c'), ('vk-graphics', 'native-vulkan-offscreen.c'),
                            ('vk-interop', 'native-vulkan-interop.c')):
            run(cc, *WARN, REPO / 'tools' / source, '-ldl', '-o', bindir / exe)
        for exe in ('anb-roundtrip', 'astc-roundtrip', 'etc2-roundtrip'):   # the driver's hal/build-nvk-tests.py
            shutil.copyfile(must(DRIVER / 'build/source-native-integration' / target / exe), bindir / exe)
        nvrm = DRIVER / f'build/nvrm-{target}'
        link = [f'-L{must(nvrm / "src/egl/libEGL_nvrm.so").parent}',
                f'-L{must(nvrm / "src/mesa/glapi/es2api/libGLESv2_nvrm.so").parent}', '-lEGL_nvrm', '-lGLESv2_nvrm']
        run(cc, *WARN, TESTS / 'gl-g0b-triangle.c', *link, '-o', bindir / 'gl-triangle')
        run(cc, *WARN, '-DNVRM_NO_VULKAN', f'-I{mesa_include}', f'-I{mesa_include / "drm-uapi"}',
            TESTS / 'gl-dmabuf.c', *link, '-o', bindir / 'gl-dmabuf')
        run(cc, *WARN, TESTS / 'gl-skia-shaders.c', *link, '-lm', '-o', bindir / 'gl-skia-shaders')
        run(cc, *WARN, f'-I{mesa_include}', f'-I{mesa_include / "drm-uapi"}', TESTS / 'gl-yuv.c', *link,
            '-lm', '-ldl', '-o', bindir / 'gl-yuv')
        if target == 'android':
            rust = profiles.rust_library(DRIVER, 'android', 'release')
            run('make', '-B', '-C', DRIVER / 'media', 'xmlgen-android',
                f'NDK_CXX={cxx}', f'NV_VIDEO_ANDROID_LIB=-L{must(rust).parent} -lnv_bionic_native')
            shutil.copyfile(DRIVER / 'media/out/android/nvc2-xmlgen', bindir / 'nvc2-xmlgen')
        for exe in bindir.iterdir():
            elf_check(exe, bits)
            exe.chmod(0o755)
    # llvmpipe references, as gl-android-smoke.py makes them (host Mesa through glvnd, surfaceless)
    host = out / 'host'
    host.mkdir()
    env = {**os.environ, 'LIBGL_ALWAYS_SOFTWARE': '1',
           '__EGL_VENDOR_LIBRARY_FILENAMES': str(must('/usr/share/glvnd/egl_vendor.d/50_mesa.json'))}
    run('cc', *WARN, TESTS / 'gl-g0b-triangle.c', '-lEGL', '-lGLESv2', '-o', host / 'gl-triangle')
    for mode, name in ((['--flat'], 'flat'), ([], 'grad')):
        run(host / 'gl-triangle', '--es3', *mode, 'llvmpipe', ref / f'ref-{name}.rgba', env=env)
    run('cc', *WARN, TESTS / 'gl-skia-shaders.c', '-lEGL', '-lGLESv2', '-lm', '-o', host / 'gl-skia-shaders')
    (ref / 'skia').mkdir()
    run(host / 'gl-skia-shaders', 'llvmpipe', ref / 'skia', env=env, stdout=subprocess.DEVNULL)
    shutil.rmtree(host)
    count = sum(1 for p in out.rglob('*') if p.is_file())
    print(f'build-gate: {count} files in {out}')


if __name__ == '__main__':
    main()
