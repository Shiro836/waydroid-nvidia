# waydroid-nvidia

**GPU-accelerated Waydroid on the NVIDIA driver — container-native, no VM.
Needs the open kernel modules (`nvidia-open`); the userspace stays NVIDIA's
regular proprietary stack.**

[![build](https://github.com/Shiro836/waydroid-nvidia/actions/workflows/build.yml/badge.svg)](https://github.com/Shiro836/waydroid-nvidia/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/Shiro836/waydroid-nvidia)](https://github.com/Shiro836/waydroid-nvidia/releases)
[![AUR](https://img.shields.io/aur/version/waydroid-nvidia-bin)](https://aur.archlinux.org/packages/waydroid-nvidia-bin)
[![nix](https://img.shields.io/badge/NixOS-community%20flake-5277C3?logo=nixos&logoColor=white)](https://github.com/yigexuanmu/waydroid-nvidia-nix)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

Stock Waydroid can't render on NVIDIA. This project proxies Vulkan (Mesa
Venus) over a unix socket to a host-side renderer that issues the real Vulkan
calls — Android renders on your NVIDIA GPU, CUDA/NVENC and full performance
stay intact, everything remains a container:

```
Android app ── Vulkan ──▶ guest Mesa Venus ── unix socket ──▶ host renderer
                                                                   │
KWin ◀── hwcomposer ◀── gralloc imports ◀── NVIDIA dmabufs ◀── NVIDIA driver
```

Buffers are allocated host-side as NVIDIA block-linear images and travel to
the compositor as native NVIDIA dmabufs — no cross-vendor negotiation, no
copies. GL runs through ANGLE, ASTC textures are emulated in a compute shader
(desktop NVIDIA lacks the hardware Android mandates), and frame sync is fully
GPU-side (timeline syncobjs + imported sync_fd semaphores; zero per-frame
socket roundtrips). The guest runs native high refresh — 500 Hz verified,
with a translated ARM game holding 500 fps at 2 ms present-to-present.

Verified in the field on Turing, Ampere, Ada and Blackwell GPUs. Real games
tested: Minecraft Bedrock, Subway Surfers, Arknights, Honkai: Star Rail —
plus Google Play certification and ARM translation (libhoudini).

## Requirements

- **NVIDIA open kernel modules** (`nvidia-open`/`nvidia-open-dkms`) — the
  closed module has no DMA-BUF support, and every displayed buffer here is
  one. The userspace (`nvidia-utils`) is the same proprietary code either
  way; nouveau/NVK is out of scope (stock Waydroid handles it). Open KM
  means **Turing (RTX 20 / GTX 16) or newer**.
- Driver **595.71+** (610.x recommended) with **`nvidia-drm.modeset=1`**.
- A Wayland session (tested on KWin / Plasma 6) and the usual Waydroid
  kernel bits (binder).
- Unsure about a machine? `tests/run-probe.sh` checks the exact
  buffer-sharing paths in ~30 s and names anything missing — see
  [`docs/troubleshooting.md`](docs/troubleshooting.md).

## Install (Arch / AUR)

```sh
yay -S waydroid-nvidia-bin        # provides/conflicts: waydroid
waydroid init                     # download an Android image, as usual
sudo waydroid-nvidia-setup        # add --refresh <hz> to match your monitor
sudo systemctl enable --now waydroid-container.service
systemctl --user enable --now wd-venus.service
# re-log-in once (udev rule for /dev/udmabuf), then:
waydroid session start            # or launch Waydroid from the app menu
```

`waydroid-nvidia-setup` deploys the guest stack, writes the config, verifies
your environment (modeset, vendor image, render node) and removes stale
config left by older installs. Safe to re-run any time — it's also the first
thing to try when something misbehaves. Verify acceleration:

```sh
sudo waydroid shell dumpsys SurfaceFlinger | grep GLES
# GLES: ... ANGLE (NVIDIA, Vulkan ... Venus (NVIDIA GeForce ...))
```

**NixOS:** community flake —
[yigexuanmu/waydroid-nvidia-nix](https://github.com/yigexuanmu/waydroid-nvidia-nix).
**Other distros:** the same binaries install anywhere — see
[`docs/install-manual.md`](docs/install-manual.md).

**Releases are fully attested**: every asset is CI-built from pinned sources
and carries SLSA provenance —
`gh attestation verify <asset> --repo Shiro836/waydroid-nvidia`.

## Documentation

- [`docs/troubleshooting.md`](docs/troubleshooting.md) — health checks, known
  failure modes, one-command debug capture, GPU probe kit
- [`docs/architecture.md`](docs/architecture.md) — how the stack works
- [`docs/transport-design.md`](docs/transport-design.md) — socket protocol
  extensions (fences, imports, GPU allocation)
- [`docs/building.md`](docs/building.md) — building from source, repo layout,
  CI/attestation
- [`docs/dev-workflow.md`](docs/dev-workflow.md) — dev environment setup and
  the edit → build → deploy → measure loop
- [`docs/install-manual.md`](docs/install-manual.md) — non-Arch installation

## Limitations & roadmap

Not yet supported: ETC2 texture emulation (ASTC is; affected games show
placeholder textures), ASTC readback (uploads/sampling work), RGBA_FP16
gralloc buffers. dma_buf mmap read bandwidth is below native (readback paths
only). Planned: self-contained guest image published as an OTA channel,
shared-memory ring transport, ETC2, input-to-photon measurement.

## Prior art & references

Anbox Cloud on NVIDIA (commercial existence proof of this shape) ·
waydroid#1883 / #564 / #1402 ·
[Mesa Venus](https://gitlab.freedesktop.org/mesa/mesa) ·
[virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) · ANGLE.

## License

Original code in `src/`, `build/`, `dev/`, `tests/`, `docs/` is MIT (see
[`LICENSE`](LICENSE)). Files under `patches/` are derivative works of their
respective upstreams and carry those upstreams' licenses.
