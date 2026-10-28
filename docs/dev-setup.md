# Developing the native modes

Users install a release. This page is for working from a checkout: build the native stack, try a driver change
on your machine, switch modes, run CI.

## Build

```sh
packaging/native/build-native.sh [--driver-src DIR] [OUT]
```

The driver (NVK over the Rust RM backend, `nvrm`, the Vulkan HAL, gralloc, the Codec2 decoder, bionic libdrm) is
[nvidia-prolapse](https://github.com/Shiro836/nvidia-prolapse), built by its own `build.sh`. The build fetches the
commit `NVIDIA_PROLAPSE_REF` in `vendor-image/pins.env` names into `build/nvidia-prolapse-src` and checks it.
`--driver-src DIR` (or `NVIDIA_PROLAPSE_SRC=DIR` for `dev/source-native` and the other tools) builds your own
checkout instead; `BUILD-RECORD.json` names the driver commit and whether it had local changes.

This builds the whole native stack the way CI does, inside the pinned container `packaging/native/Containerfile`
(needs docker). It ends with the install-time test programs and the release bundle (`payload/`, `gate/`,
`BUILD-RECORD.json`). `NATIVE_BUILDENV=host` uses your own toolchain instead; `NATIVE_PARALLEL=0` builds one
target at a time. Build outputs and pinned third-party clones go under `build/`.

The vendor image is built separately on a LineageOS 20 tree: `vendor-image/build-vendor.sh` with
`VENDOR_PRODUCT=prolapse`, then `packaging/vendor-ota.py` (see `vendor-image/notes.md`).

## Try a driver change without rebuilding the image

The default mode has two install shapes, set by `nvidia_prolapse_install` in `waydroid.cfg`:

- `vendor-image`: what users get, our vendor image installed and tested by `waydroid-nvidia-setup`;
- `dev-root`: a staged driver directory bind-mounted over the official vendor image, for quick iteration.

```sh
sudo waydroid-nvidia-setup --restore-official-vendor          # dev-root needs the official image
dev/source-native stage /var/lib/waydroid/nvidia-source-native-<name>   # runs the tests on your GPU
sudo dev/source-native activate /var/lib/waydroid/nvidia-source-native-<name>
```

To stage a CI-built bundle instead of `build/`, use
`sudo dev/source-native stage-release <bundle> /var/lib/waydroid/nvidia-source-native-<name>`.

## Switch modes

With Waydroid stopped (`waydroid session stop`, `sudo waydroid container stop`):

```sh
sudo python3 dev/driver-mode apply nvidia-prolapse|glibc-shim|venus [--native-gl on|off] [--install vendor-image|dev-root]
dev/driver-mode status
```

`driver-mode` never installs an image; `--install vendor-image` only selects one that setup already installed.
With the root helper installed (`packaging/native/wd-native`, rules in `packaging/native/sudoers.example`),
`dev/driver-mode set MODE ...` does the stop, apply and restart for you. `glibc-shim` needs its staging tree
first (`dev/native-deploy` from a checkout; a package stages it with `sudo waydroid-nvidia-setup --mode glibc-shim`). The mode rules are in [waydroid/README.md](../waydroid/README.md).

A checkout runs the Waydroid it builds (`dev/build-waydroid` writes `build/waydroid`). A packaged install runs the
system Waydroid.

## Install a local release like a user

Build the release tarballs as the CI release job does, make the vendor zip and channel with
`packaging/vendor-ota.py VENDOR_IMG BUILD-RECORD.json OUT --tag TAG`, then run
`sudo packaging/install.sh --tag <tag> --from <dir with the assets>`. Until that tag exists on GitHub, an online
`waydroid upgrade` fails on the vendor channel; `waydroid upgrade -o` works.

## CI

CI runs on `v*` tags (releases, see `packaging/aur/RELEASING.md`) and when dispatched by hand, never on pushes or
pull requests:

```sh
gh workflow run build.yml --ref <branch>                        # everything
gh workflow run build.yml --ref <branch> -f self_hosted=false   # hosted jobs only
gh workflow run native.yml --ref <branch>                       # unit tests + host build
gh workflow run vendor-image.yml --ref <branch>                 # vendor images; run build.yml first
```

ANGLE, SurfaceFlinger, the native stack and the vendor image build on a self-hosted runner (label `lineage`) that
keeps a LineageOS tree and compiler caches between runs. A cached build gives the same bundle files as an uncached
one (compare the `files` maps of two `BUILD-RECORD.json`). `vendor-image.yml` takes its native bundle and ANGLE
from the newest successful `build.yml` run on the same branch.
