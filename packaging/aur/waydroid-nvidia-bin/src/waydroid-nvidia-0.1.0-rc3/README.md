# waydroid-nvidia

**GPU-accelerated Waydroid on the NVIDIA driver — container-native, no VM.
Needs the open kernel modules (`nvidia-open`); the userspace stays NVIDIA's
regular proprietary stack.**

[![build](https://github.com/Shiro836/waydroid-nvidia/actions/workflows/build.yml/badge.svg)](https://github.com/Shiro836/waydroid-nvidia/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/Shiro836/waydroid-nvidia?include_prereleases)](https://github.com/Shiro836/waydroid-nvidia/releases)
[![AUR](https://img.shields.io/aur/version/waydroid-nvidia-bin)](https://aur.archlinux.org/packages/waydroid-nvidia-bin)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

Waydroid runs Android in an LXC container. On a machine whose displays hang off
an NVIDIA GPU, stock Waydroid can't use it for rendering. This project makes
Android render on the NVIDIA GPU by **proxying Vulkan (Mesa Venus) over a unix
socket** to a host-side renderer that issues the real Vulkan calls — keeping the
host's proprietary NVIDIA userspace (CUDA / NVENC / full performance) intact and
the whole thing inside a container.

- [How it works](#how-it-works)
- [Capabilities](#capabilities)
- [Installation](#installation)
  - [Requirements](#requirements)
  - [Arch Linux (AUR)](#arch-linux-aur)
  - [Other distros (manual)](#other-distros-manual)
  - [Verifying downloads](#verifying-downloads)
- [Building from source](#building-from-source)
- [Repository layout](#repository-layout)
- [Roadmap](#roadmap)
- [Limitations](#limitations)
- [Prior art & references](#prior-art--references)
- [License](#license)

## How it works

```
Android app ── Vulkan ──▶ guest Mesa Venus (bionic)
                              │ serialized Venus protocol, unix socket
                              ▼
                     host daemon (virglrenderer render server)
                              │ real Vulkan calls
                              ▼
      NVIDIA proprietary userspace (open kernel modules) ──▶ GPU
                              │ rendered buffer as dmabuf (zero-copy)
                              ▼
              guest gralloc imports ──▶ hwcomposer ──▶ KWin
```

The container speaks the **Venus** wire protocol (Mesa's virtio-gpu Vulkan
driver) over the vtest unix-socket transport. A patched **virglrenderer** on the
host decodes it and replays the calls on the real NVIDIA driver. Buffers are
allocated **host-side** as NVIDIA block-linear `VkImage`s, exported as dmabufs,
and imported by a custom **gralloc** backend in the guest — so every buffer the
compositor sees is NVIDIA-native and binds without cross-vendor negotiation.

See [`docs/architecture.md`](docs/architecture.md) for the design and
[`docs/transport-design.md`](docs/transport-design.md) for the socket protocol
extensions.

## Capabilities

- **Full NVIDIA acceleration, zero VM.** Every pixel Android draws — Vulkan,
  GL (via ANGLE), UI, games — renders on the host NVIDIA GPU and displays
  through KWin as native NVIDIA dmabufs.
- **High refresh, low latency.** The guest runs a native high-refresh display
  (500 Hz supported; SurfaceFlinger's stock scheduler caps at ~333 Hz — this
  ships a one-line-tunable fix). All frame synchronization is GPU-side:
  timeline-syncobj fences shared between guest and host (zero per-frame
  socket roundtrips) and imported `sync_fd` semaphores (no CPU waits in
  SurfaceFlinger). Measured: a translated ARM game runs 500 fps flat at
  2 ms present-to-present on a 500 Hz monitor.
- **The compositor path is direct.** SurfaceFlinger composites nothing in
  steady state: app buffers attach straight to Wayland surfaces and KWin
  displays them — a fullscreen game's buffer travels app → KWin untouched.
  Layers the compositor can't take directly (software-rendered, solid-color)
  fall back to SurfaceFlinger transparently.
- **Vulkan-native Android games work on desktop GPUs.** Desktop NVIDIA has no
  ASTC texture hardware (Android mandates it); the guest Venus driver
  transparently decodes ASTC uploads with a compute shader, so Unity/Unreal
  Vulkan titles render correctly instead of magenta placeholders.
  (Opt-out: `VN_NO_ASTC_EMU=1`.)
- **Real games, verified:** Minecraft Bedrock (native x86_64), Subway
  Surfers, Arknights, Honkai: Star Rail — plus Google Play certification and
  ARM translation via libhoudini.
- **Survives updates.** Mounts and props are emitted by the integration
  generators, so `waydroid upgrade` keeps everything working. A desktop
  launcher entry health-checks and auto-recovers the whole stack on click.

## Installation

### Requirements

- **NVIDIA's open kernel modules + the regular proprietary userspace.**
  NVIDIA's driver has two halves. The kernel module comes in two flavors:
  `nvidia-open` / `nvidia-open-dkms` (open-source, NVIDIA's default for
  Turing and newer) and `nvidia` / `nvidia-dkms` (legacy closed). The
  userspace (`nvidia-utils`: Vulkan, GL, CUDA, NVENC) is the same
  proprietary code with either one. This project **requires the open
  kernel modules**: every buffer Android displays is exported from the
  driver as a DMA-BUF, and NVIDIA supports DMA-BUF only on the open flavor
  ([NVIDIA docs](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/kernel-modules.html),
  [open-gpu-kernel-modules#243](https://github.com/NVIDIA/open-gpu-kernel-modules/discussions/243)).
  So: `nvidia-open` users are exactly who this is for — "proprietary driver"
  in these docs refers to the userspace, not the kernel module. The fully
  open Mesa stack (nouveau/NVK) is a different world; stock Waydroid
  already supports it on its own.
- Driver version **610.x recommended** (tested on 610.172) with
  **`nvidia-drm.modeset=1`**; needs `VK_EXT_image_drm_format_modifier` and
  SYNC_FD fence support. 595.71+ has been reported working in the field, on
  `nvidia-open-dkms`
  ([waydroid#1883](https://github.com/waydroid/waydroid/issues/1883#issuecomment-5037146083));
  our allocator never relies on implied dmabuf strides, so the 595.45 EGL
  stride bug ([NVIDIA forum #364360](https://forums.developer.nvidia.com/t/364360),
  verified fixed in 610.x by `tests/eglstride.c`) should not affect this stack.
- A Wayland session (tested on KWin / Plasma 6).
- The usual Waydroid kernel bits: binder (in-kernel binder or binderfs —
  default on Arch/zen and most modern kernels).
- For the manual path: host binaries are CI-built on Ubuntu 24.04
  (glibc ≥ 2.39) and link `libepoxy`, `libdrm`, `libgbm`, `libX11`, `expat`,
  plus the Vulkan loader at runtime.

### Arch Linux (AUR)

```sh
yay -S waydroid-nvidia-bin        # provides/conflicts: waydroid
waydroid init                     # download an Android image, as usual
sudo waydroid-nvidia-setup        # add --refresh <hz> to match your monitor
sudo systemctl enable --now waydroid-container.service
systemctl --user enable --now wd-venus.service
waydroid session start            # or launch Waydroid from the app menu
```

`waydroid-nvidia-setup` copies the guest driver stack into
`/var/lib/waydroid`, writes the required properties into `waydroid.cfg`, and
regenerates the container config. It is safe to re-run (e.g. after a package
upgrade or `waydroid upgrade`). `--refresh` above 240 Hz also enables the
patched SurfaceFlinger that lifts the stock ~333 Hz scheduler ceiling.

Verify it worked:

```sh
sudo waydroid shell dumpsys SurfaceFlinger | grep GLES
# GLES: ... ANGLE (NVIDIA, Vulkan ... Venus (NVIDIA GeForce ...))
```

### Other distros (manual)

The same binaries install anywhere systemd + Waydroid's dependencies exist.
From a checkout of this repo:

1. **Patched waydroid** (two small patches: the config generator emits this
   stack's bind-mounts so `waydroid upgrade` keeps working, and
   `suspend_action = none` support):

   ```sh
   git clone https://github.com/waydroid/waydroid.git && cd waydroid
   git checkout a33a5c0b31d89d6ce687381104b30aff4dd2d330   # patches/waydroid/BASE
   git apply ../patches/waydroid/0001-nvidia-integration.patch
   sudo make install USE_NFTABLES=1     # =0 if your firewall is iptables
   cd ..
   ```

2. **Download and verify the release binaries** (see
   [Verifying downloads](#verifying-downloads)):

   ```sh
   V=v0.1.0-rc1
   B=https://github.com/Shiro836/waydroid-nvidia/releases/download/$V
   curl -LO $B/waydroid-nvidia-host-x86_64-$V.tar.zst
   curl -LO $B/waydroid-nvidia-guest-android-x86_64-$V.tar.zst
   curl -LO $B/waydroid-nvidia-guest-prebuilts-$V.tar.zst
   curl -LO $B/SHA256SUMS
   sha256sum -c --ignore-missing SHA256SUMS
   ```

3. **Install them** (paths must match the unit/setup script):

   ```sh
   sudo mkdir -p /usr/lib/waydroid-nvidia/guest
   sudo tar --zstd -xf waydroid-nvidia-host-x86_64-$V.tar.zst          -C /usr/lib/waydroid-nvidia
   sudo tar --zstd -xf waydroid-nvidia-guest-android-x86_64-$V.tar.zst -C /usr/lib/waydroid-nvidia/guest
   sudo tar --zstd -xf waydroid-nvidia-guest-prebuilts-$V.tar.zst      -C /usr/lib/waydroid-nvidia/guest
   ```

4. **Units, tmpfiles, setup helper:**

   ```sh
   P=packaging/aur/waydroid-nvidia-bin
   sudo install -Dm644 $P/wd-venus.service        /etc/systemd/user/wd-venus.service
   sudo install -Dm644 $P/waydroid-venus.tmpfiles /etc/tmpfiles.d/waydroid-venus.conf
   sudo install -Dm755 $P/waydroid-nvidia-setup   /usr/local/bin/waydroid-nvidia-setup
   sudo systemd-tmpfiles --create /etc/tmpfiles.d/waydroid-venus.conf
   ```

5. **Initialize and start** — same as the AUR steps from `waydroid init`
   onward (the container unit is `waydroid-container.service`, installed by
   `make install` in step 1).

### Verifying downloads

`SHA256SUMS` covers integrity. For **provenance** — proof an asset was built
by this repo's CI workflow from this source — the CI-built tarballs
(`host-x86_64`, `guest-android-x86_64`) carry SLSA attestations:

```sh
gh attestation verify waydroid-nvidia-host-x86_64-<tag>.tar.zst --repo Shiro836/waydroid-nvidia
```

The `guest-prebuilts` tarball (hwcomposer, ANGLE, patched SurfaceFlinger) is
**not CI-attested yet** — those components' build provisioning isn't scripted
in CI (an ANGLE build needs a ~16 GB checkout, SurfaceFlinger a ~154 GB AOSP
tree). Their recipes live in `build/` and their sums are in the release; see
[`packaging/aur/PREREQS.md`](packaging/aur/PREREQS.md) for the honest gap
list and plan.

## Building from source

Each `patches/<component>/BASE` pins the upstream commit and apply order; one
build recipe per component lives in `build/<component>/build.sh`, and
[`packaging/aur/reproduce.sh`](packaging/aur/reproduce.sh) validates the full
clone → apply → build path (it's what CI runs). In outline:

1. **Host renderer** — clone virglrenderer at `patches/virglrenderer/BASE`, apply
   the series, copy `src/virglrenderer-vtest/*` into `vtest/`, `meson` + `ninja`.
   Runs as a systemd user unit serving the venus socket.
2. **Guest Mesa Venus** — clone mesa at `patches/mesa/BASE`, apply, cross-build
   for `android-x86_64` (NDK; cross file in `build/mesa/`).
3. **gralloc backend** — build `src/minigbm-vtest/vtest_wrapper.c` against
   minigbm; deploy as the `libgbm_mesa_wrapper` replacement.
4. **hwcomposer** — apply `patches/hwcomposer`, build standalone with
   `build/hwcomposer/build.sh` (no AOSP tree needed).
5. **waydroid** — apply `patches/waydroid`; the generators emit the bind-mounts
   and props so they survive `waydroid upgrade`.

## Repository layout

This repo is **patches + net-new source + build glue**, not vendored upstream
trees. A build clones pinned upstream and applies the patches.

```
patches/            per-component patch series; each dir has a BASE (pinned commit)
  mesa/               guest Venus driver
  virglrenderer/      host renderer
  minigbm/            gralloc (base pin; the change is the net-new backend below)
  hwcomposer/         display / refresh / windowing
  waydroid/           lxc.py mount generator + suspend handling
src/                net-new source dropped into the patched trees
  virglrenderer-vtest/  vtest_gpu_alloc.{c,h}   host NVIDIA allocator
  minigbm-vtest/        vtest_wrapper.c         gralloc backend over the socket
build/              standalone build glue (hwcomposer NDK, hidl-gen, mesa cross, ANGLE args)
build/lineage-20/   guest image / SurfaceFlinger build recipes (500 Hz patch)
patches/lineage-20/ frameworks/native patch series (vsync snap window prop)
dev/                dev-loop scripts (restart, status, logs)
tests/              C probes used to de-risk each step
docs/               architecture, transport design, dev workflow
packaging/host/     host helper binaries (wd-deploy, wd-launch desktop launcher)
packaging/aur/      PKGBUILD (waydroid-nvidia-bin), CI/release docs, reproduce.sh
packaging/ci/       pinned upstream SHAs consumed by .github/workflows/build.yml
```

## Roadmap

- Self-contained guest image (all components folded in, bind-mounts retired)
  published as an OTA channel.
- CI provisioning for hwcomposer (then ANGLE/SurfaceFlinger) so every shipped
  binary is attested.
- ETC2 texture emulation (same mechanism as ASTC; needed by some GLES3
  Vulkan ports).
- Shared-memory ring transport for Venus commands.
- Input-to-photon measurement and tuning.

## Limitations

- ETC2-compressed textures are not yet emulated (ASTC is); affected games
  show placeholder textures.
- Reading back ASTC texture data from the GPU (rare; some tools) is not
  supported — uploads and sampling are.
- RGBA_FP16 buffer combination not yet supported by the gralloc format table.
- dma_buf mmap read bandwidth is below native (affects readback paths, not the
  hot render path).

## Prior art & references

- Anbox Cloud on NVIDIA — commercial existence proof of this shape.
- Tracking issues: waydroid#1883 (NVIDIA accel), waydroid#564 (socket-proxy
  proposal), waydroid#1402 (Venus/virgl discussion).
- [Mesa Venus](https://gitlab.freedesktop.org/mesa/mesa) ·
  [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) · ANGLE.

## License

Original code in `src/`, `build/`, `dev/`, `tests/`, `docs/` is MIT (see
[`LICENSE`](LICENSE)). Files under `patches/` are derivative works of their
respective upstreams and carry those upstreams' licenses.
