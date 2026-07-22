# AUR packaging

The endgame (Roadmap **M8.6**) is a reproducible AUR package. This directory
holds the build recipe that gets there, plus a validator that continuously
proves the stranger's build path.

## Binary distribution (waydroid-nvidia-bin) + CI provenance

`waydroid-nvidia-bin/` is the binary AUR package. Its native binaries come
from GitHub release assets built by `.github/workflows/build.yml`, which
reproduces the stranger's path on a fresh runner (fetch upstream at
`packaging/ci/pins.env` SHAs → apply `patches/` → the same
`build/<comp>/build.sh` recipes) and attaches **SLSA build-provenance
attestations** — anyone can check a downloaded asset was built by that
workflow from this repo:

```sh
gh attestation verify waydroid-nvidia-host-x86_64-<tag>.tar.zst --repo Shiro836/waydroid-nvidia
```

CI-built today: guest Venus driver (mesa), host renderer (virglrenderer),
gralloc backend. Prebuilt until their provisioning is scripted (PREREQS.md):
hwcomposer, ANGLE, surfaceflinger — uploaded as a separate release asset with
sums in SHA256SUMS, explicitly *not* CI-attested yet.

## One recipe, two front-ends

The build is **not** duplicated between "dev" and "AUR". There is one source of
truth (`patches/` + `src/`) and one build recipe per component
(`build/<comp>/build.sh`), consumed by two thin front-ends:

```
build/<comp>/build.sh  SRCDIR [BUILDDIR]      ← the ONE recipe
      ├── dev/build <comp>        persistent tree, incremental (fast iteration)
      └── packaging/aur/reproduce.sh   fresh checkout at BASE, clean-room (reproducible)
                                       └── eventual PKGBUILD build() calls the same
```

Forcing dev to run the full clone→apply→build per edit would kill iteration
speed; shipping dev's persistent-tree assumptions would break reproducibility.
Sharing the *recipe* (not the *pipeline*) gets both.

## Validate the stranger's path

```sh
packaging/aur/reproduce.sh            # all components
packaging/aur/reproduce.sh mesa       # one
SRC_MODE=clone packaging/aur/reproduce.sh mesa   # true from-scratch (re-downloads upstream)
```

For each component it makes a fresh checkout at `patches/<comp>/BASE`, applies
the patch series + net-new `src/`, and builds via `build/<comp>/build.sh`.

**Validated (2026-07-12):** mesa (full cross-compile) ✅, virglrenderer (full
build) ✅, gralloc (NDK compile) ✅, waydroid (patch apply) ✅. hwcomposer
compiles but its final link needs hand-provisioned prereqs + a mounted image
rootfs — see [`PREREQS.md`](PREREQS.md), the honest gap list.

## What a stranger can and can't do today

- **Can:** clone this repo, run `reproduce.sh mesa virgl` on a box with the NDK
  + meson toolchain and get working guest/host binaries — patches apply on the
  pinned BASE and produce byte-identical trees to what we run.
- **Can't yet:** `makepkg` a finished package. Two reasons: (1) hwcomposer's
  prereqs aren't scripted (`PREREQS.md`), and (2) the current stack rides on
  runtime bind-mounts over a stock image — a real installable package needs the
  **M8.6** framework rebuild that folds everything into one image and retires
  the bind-mounts.

`PKGBUILD` lands here once M8.6 does; its `build()` will call the same
`build/<comp>/build.sh` this validator already exercises.
