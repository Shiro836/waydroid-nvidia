# How it works

Waydroid runs Android in an LXC container that shares the host kernel. Each driver mode gives Android a
different way to reach the NVIDIA GPU. All modes share the same display path: our hwcomposer hands each frame as
a dma-buf to your Wayland compositor, which shows it without a copy.

## nvidia-prolapse (default)

```
app ── GLES ──▶ ANGLE ──▶ Mesa NVK (Vulkan) ──▶ our Rust RM backend ──▶ /dev/nvidia* ──▶ GPU
app ── GLES ──▶ nvrm (with --native-gl on) ──┘
video ── Codec2 ──▶ our NVDEC decoder ──────────┘
```

- Mesa's NVK and its NAK shader compiler are built for Android (bionic). Instead of the nouveau kernel driver,
  NVK talks to NVIDIA's kernel module through our backend, which uses the module's public RM interface (the same
  headers as NVIDIA's open-gpu-kernel-modules).
- `nvrm` is our own gallium GLES 3.1 driver on the same backend. It is optional; by default GLES goes through
  ANGLE on NVK.
- gralloc, hwcomposer, ANGLE, the Codec2 decoder and the drivers are vendor modules of our own Waydroid vendor
  image, built from source on Waydroid's LineageOS 20 tree. The system image stays the official one.
- No NVIDIA userspace library runs in the container. The host provides only the device nodes.

Code: the driver repository [nvidia-prolapse](https://github.com/Shiro836/nvidia-prolapse) (NVK and the Rust RM
backend, `nvrm`, the Vulkan HAL, gralloc, the Codec2 decoder), built at the commit `vendor-image/pins.env` pins;
`vendor-image/` here.

## glibc-shim

```
app ──▶ ANGLE or NVIDIA GLES ──▶ Android Vulkan HAL ──▶ host's NVIDIA libraries (glibc) via our shims ──▶ GPU
```

NVIDIA's userspace driver is built for glibc, Android uses bionic. Our shim libraries carry the glibc names and
translate the calls the NVIDIA libraries make. The host's own driver files are bind-mounted into the container
unmodified, found where the system's dynamic loader finds them (any distribution layout) at the exact version of the
loaded kernel module. One shim build covers driver 535 to 615 (glibc-shim runs on 550 to 615): it exports every glibc function those branches import
(the import lists in `nvidia-prolapse/shim/imports/`), and the setup checks the installed driver's own imports
against it before switching. Drivers 470 and older are refused: their Vulkan driver cannot share buffers with
Android's compositor (no `VK_EXT_external_memory_dma_buf` / `VK_EXT_image_drm_format_modifier`). Driver 535 is
refused too: its Vulkan driver cannot import or export a fence as a sync_file, which Android's buffer queue hands
every buffer over with. Two driver defects with imported sync_files make the Vulkan HAL's `vkAcquireImageANDROID`
wait for the buffer's fence on the CPU and import "already signalled" instead: on 550 to 560 when the app asks
for a fence (waiting on a fence whose sync_file has not signalled yet crashes the driver), on 580 when it asks
for a semaphore (the driver keeps an already signalled sync_file open forever). `make -C <driver checkout>/hal
syncfd-check` measures both on the installed driver.

Code: `nvidia-prolapse/shim/`, `egl/`; the HAL, gralloc backend and libdrm are the driver repository's `hal/`,
`gralloc/` and `libdrm/` (their glibc-shim build targets).

## venus

```
app ──▶ ANGLE ──▶ Mesa Venus (guest) ── Vulkan over a unix socket ──▶ virglrenderer (host) ──▶ NVIDIA driver
```

The guest Vulkan driver serializes calls and the host replays them on the real NVIDIA driver. gralloc allocates
buffers through the host as well, so the compositor gets block-linear buffers it can display (NVIDIA's EGL only
imports LINEAR buffers as external textures). Fences are shared through a DRM timeline syncobj, so a frame needs
no socket round trip for them. ASTC textures, which desktop NVIDIA GPUs lack, are decoded on the GPU by a compute
shader. Details: [transport-design.md](transport-design.md), [fence-timeline-design.md](fence-timeline-design.md).

## Display refresh

hwcomposer reports the fastest refresh rate among your monitors when the container starts, so Android renders at
that rate. Android's stock SurfaceFlinger treats vsyncs closer than 3 ms as one, which can stall above about
333 Hz. The default mode uses the official system image's SurfaceFlinger, so `--refresh` caps the rate there.
The `venus` and `glibc-shim` modes bind in a patched SurfaceFlinger whose window shrinks with the refresh rate.

## Waydroid integration

`waydroid/nvidia_native.py` generates the container's mounts and Android properties for the chosen mode. It
refuses to start with a clear message when something is missing; no mode falls back to another silently. See
[waydroid/README.md](../waydroid/README.md).
