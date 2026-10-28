# Building from source

Nothing third-party is vendored. Each build clones the pinned upstream (see [THIRD_PARTY.md](../THIRD_PARTY.md)),
applies our patches and runs a recipe script. CI uses the same scripts.

- Native modes (the default `nvidia-prolapse`, and `glibc-shim`): `packaging/native/build-native.sh`, see
  [dev-setup.md](dev-setup.md).
- Venus mode and the parts all modes share: the recipes below. `packaging/aur/reproduce.sh` runs them the way CI
  does.

## Repository layout

```
nvidia-prolapse/    glibc-shim mode's shims and EGL front, probes (see nvidia-prolapse/README.md); the driver
                    itself is github.com/Shiro836/nvidia-prolapse, pinned in vendor-image/pins.env and
                    fetched into build/nvidia-prolapse-src (tools/driver_source.py)
waydroid/           Waydroid integration of the driver modes (see waydroid/README.md)
vendor-image/       our Waydroid vendor image, built on a LineageOS 20 tree
hwcomposer/ angle/  display HAL and ANGLE, shared by all modes
patches/            Venus-mode patch series per upstream (each dir has a BASE commit)
src/                new source files copied into the patched Venus-mode trees
build/              one recipe per component (build outputs go here too)
packaging/          installer, AUR package, native bundle and release tooling
dev/                developer scripts (build, deploy, mode switch, measurements)
tests/              host probes and the portable GPU probe
```

## Shared and Venus-mode components

1. Host renderer: virglrenderer at `patches/virglrenderer/BASE` plus `src/virglrenderer-vtest/`
   (`build/virglrenderer/build.sh`). Runs as the `wd-venus` user service.
2. Guest Venus driver: Mesa at `patches/mesa/BASE`, cross-built with the NDK for x86_64 and x86
   (`build/mesa/build.sh`, see below).
3. Venus gralloc backend: `src/minigbm-vtest/vtest_wrapper.c` against pinned minigbm (`build/gralloc/build.sh`).
4. hwcomposer: `hwcomposer/provision.sh` fetches every prerequisite from pinned public sources (AOSP headers,
   hidl-gen, static wayland/libffi/xkbcommon, link stubs from the pinned images, no root needed), then
   `hwcomposer/build.sh` applies `hwcomposer/patches/` and builds with the NDK. Pins: `hwcomposer/pins.env`.
5. ANGLE: `ANGLE_SHA` in `packaging/ci/pins.env`, `build/angle/args-x86{,_64}.gn`. The checkout needs about 16 GB.
6. SurfaceFlinger: `build/lineage-20/build.sh sf` on a LineageOS 20 tree (about 150 GB). Don't source AOSP's
   `envsetup.sh` from a `set -u` script.
7. Waydroid: Python patches only (`patches/waydroid/`), applied by the package.

Venus driver for both Android ABIs:

```sh
ANDROID_ABI=x86_64 build/mesa/build.sh /path/to/mesa /path/to/mesa/build-android-x86_64
ANDROID_ABI=x86    build/mesa/build.sh /path/to/mesa /path/to/mesa/build-android-x86
```

Set `NDK_PKGCONFIG` only to libraries built for the same ABI. The two results must stay at
`vendor/lib64/hw/vulkan.virtio.so` and `vendor/lib/hw/vulkan.virtio.so`; don't flatten them into one directory.

For an edit, build and deploy loop on Venus mode, see [dev-workflow.md](dev-workflow.md).
