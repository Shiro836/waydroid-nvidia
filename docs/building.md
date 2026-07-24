# Building from source & repository layout

This repo is **patches + net-new source + build glue**, not vendored upstream
trees. A build clones the pinned upstream, applies the patch series, and runs
the same per-component recipe the dev loop and CI use.

## Repository layout

```
patches/            per-component patch series; each dir has a BASE (pinned commit)
  mesa/               guest Venus driver (sync_fd, dmabuf import, ASTC emu, steering)
  virglrenderer/      host renderer (GPU allocator command, timeline fences, sem import)
  minigbm/            gralloc base pin (the real change is the net-new backend below)
  hwcomposer/         display / refresh / windowing fixes
  waydroid/           config generators, gpu.py trust, session preflight
  lineage-20/         frameworks/native series (vsync snap window prop)
src/                net-new source dropped into the patched trees
  virglrenderer-vtest/  vtest_gpu_alloc.{c,h}   host NVIDIA allocator
  minigbm-vtest/        vtest_wrapper.c         gralloc backend over the socket
build/              one build recipe per component
  mesa/               NDK cross build (guest driver)
  virglrenderer/      host meson build
  gralloc/            NDK build of the wrapper
  hwcomposer/         build.sh + provision.sh (see below)
  hidl-gen/           standalone CMake build of AOSP hidl-gen (host tool)
  angle/              pinned gn args
  lineage-20/         surfaceflinger / full image build scripts
packaging/ci/       pinned upstream SHAs (pins.env, hwc-pins.env) consumed by CI
packaging/aur/      PKGBUILD (waydroid-nvidia-bin), release docs, reproduce.sh
packaging/host/     host helpers (wd-deploy, wd-launch desktop launcher)
tests/              C probes that de-risked each step + the portable GPU probe kit
docs/               architecture, transport design, install, troubleshooting
```

## Components and recipes

Every binary in a release is CI-built from these exact recipes
(`.github/workflows/build.yml`); `packaging/aur/reproduce.sh` runs the same
clone → apply → build path locally.

1. **Host renderer** — virglrenderer at `patches/virglrenderer/BASE`, apply
   series, copy `src/virglrenderer-vtest/*` into `vtest/`, `meson` + `ninja`
   (`build/virglrenderer/build.sh`). Runs as the `wd-venus` systemd user unit.
2. **Guest Mesa Venus** — mesa at `patches/mesa/BASE`, apply, then NDK
   cross-build both `ANDROID_ABI=x86_64` and `ANDROID_ABI=x86`
   (`build/mesa/build.sh`). The latter uses the NDK's
   `i686-linux-android34-clang` target.
3. **gralloc backend** — `src/minigbm-vtest/vtest_wrapper.c` against pinned
   minigbm (`build/gralloc/build.sh`); deploys as the `libgbm_mesa_wrapper`
   replacement.
4. **hwcomposer** — fully clean-room:
   `build/hwcomposer/provision.sh` assembles every prerequisite from pinned
   public sources (13 AOSP header repos, standalone hidl-gen, generated HIDL
   headers, static wayland/libffi/xkbcommon for bionic, libdrm headers, and
   the link-time `.so` stubs extracted from the pinned OTA images with
   debugfs — no mounts, no root). Then `build/hwcomposer/build.sh` compiles
   and links with the NDK. Pins: `packaging/ci/hwc-pins.env`.
5. **ANGLE** — pinned commit (`ANGLE_SHA` in `packaging/ci/pins.env`),
   `gclient` checkout, `build/angle/args-x86{,_64}.gn`, ninja. CI builds
   `out/AndroidX86` and `out/AndroidX64`; the checkout is heavy (~16 GB), so
   it runs on a self-hosted runner with a persistent work area.
6. **surfaceflinger** — built from a LineageOS 20 tree
   (`build/lineage-20/build.sh sf`); CI runs it on the self-hosted runner
   that carries the ~150 GB synced tree. Note: AOSP's `envsetup.sh` is not
   `set -u` clean — don't source it from strict-mode scripts.
7. **waydroid** — python patches only (`patches/waydroid/`), applied at the
   pinned base by the PKGBUILD / manual install.

## CI notes

- Hosted-runner jobs (mesa, virgl, gralloc, hwcomposer) run on every push.
- Self-hosted jobs (ANGLE, surfaceflinger) run on tags and manual dispatch;
  the runner is registered by the maintainer with the `lineage` label.
- Fork PRs require maintainer approval before any workflow runs
  (all-external-contributors policy) — never approve a run for a PR touching
  `.github/` without reading it.
- On `v*` tags the release job assembles all four tarballs, writes
  `SHA256SUMS`, and attaches SLSA provenance attestations to every asset.
  Non-rc tags publish as full releases, `-rc*` tags as prereleases.
- Please don't use CI as a remote grep/build service — every recipe runs
  locally (see `reproduce.sh`), and hermetic validation in a container
  before pushing is the house style.

Build the two app-facing Vulkan HALs locally with:

```sh
ANDROID_ABI=x86_64 build/mesa/build.sh /path/to/mesa /path/to/mesa/build-android-x86_64
ANDROID_ABI=x86    build/mesa/build.sh /path/to/mesa /path/to/mesa/build-android-x86
```

The default pkg-config search is intentionally empty so Meson uses its pinned
fallbacks. Set `NDK_PKGCONFIG` only to a directory containing libraries built
for the selected Android ABI; the old x86_64-only local tree is invalid for an
`ANDROID_ABI=x86` build.

Release staging preserves their Android paths as
`vendor/lib64/hw/vulkan.virtio.so` and
`vendor/lib/hw/vulkan.virtio.so`; flattening these artifacts would overwrite
one ABI with the other.
