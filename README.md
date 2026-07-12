# waydroid-nvidia

**GPU-accelerated Waydroid on the NVIDIA proprietary driver — container-native, no VM.**

Waydroid runs Android in an LXC container. On a machine whose displays hang off
an NVIDIA GPU, stock Waydroid can't use it for rendering. This project makes
Android render on the NVIDIA GPU by **proxying Vulkan (Mesa Venus) over a unix
socket** to a host-side renderer that issues the real Vulkan calls — keeping the
host's proprietary NVIDIA userspace (CUDA / NVENC / full performance) intact and
the whole thing inside a container.

## How it works

```
Android app ── Vulkan ──▶ guest Mesa Venus (bionic)
                              │ serialized Venus protocol, unix socket
                              ▼
                     host daemon (virglrenderer render server)
                              │ real Vulkan calls
                              ▼
                NVIDIA proprietary driver (Open KM) ──▶ GPU
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

- Vulkan executes on the NVIDIA GPU through the socket (compute round-trip
  verified identical to direct NVIDIA).
- `sync_fd` fences and semaphores over the socket (`VK_KHR_external_*_fd`).
- dma_buf import **and** host-side GPU allocation over the socket.
- HWUI on `skiavk` (Vulkan), SurfaceFlinger RenderEngine and GL apps on **ANGLE**
  (GL-on-Vulkan) — all reaching NVIDIA via Venus.
- hwcomposer presents NVIDIA dmabufs through Wayland to KWin.
- Google Play certified image; ARM app translation (libndk); real games run.
- Configuration survives `waydroid upgrade` (mounts and props emitted by the
  integration generators).

## Repository layout

This repo is **patches + net-new source + build glue**, not vendored upstream
trees. A build clones pinned upstream and applies the patches (AUR-friendly).

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
dev/                dev-loop scripts (restart, status, logs)
tests/              C probes used to de-risk each step
docs/               architecture, transport design, dev workflow
packaging/aur/      PKGBUILD (planned)
```

## Building & deploying

Each `patches/<component>/BASE` pins the upstream commit and apply order. In outline:

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

## Roadmap

Focus is smoothness and latency, then a self-contained image for packaging:

- Combined submit + fence-export socket command (fewer roundtrips per frame).
- Threaded RenderEngine on ANGLE.
- Direct-overlay path for fullscreen game layers.
- Shared-memory ring transport for Venus commands (the big latency win).
- Native high-refresh: guest framework rebuild folding all pieces into one
  image and retiring the runtime bind-mounts.
- Input-to-photon measurement and tuning.
- AUR package.

## Limitations

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
