# Architecture

## Overview

Run Waydroid GPU-accelerated on the host's proprietary NVIDIA driver, while
staying container-native (no VM) and keeping the host's proprietary NVIDIA
userspace intact. Android renders Vulkan through Mesa Venus; the calls are
proxied over a unix socket to a host renderer that replays them on the real
NVIDIA driver; the NVIDIA-driven KWin compositor displays the result.

## Buffer allocation: why host-side

On a machine whose displays are on the NVIDIA GPU, KWin composites on NVIDIA, so
every buffer it shows must be NVIDIA-native:

- NVIDIA EGL binds a foreign LINEAR dmabuf **only** as `GL_TEXTURE_EXTERNAL_OES`;
  a compositor using `GL_TEXTURE_2D` for RGB cannot bind it. Cross-vendor
  buffers are therefore undisplayable.
- NVIDIA **exports** dma_buf from `VkDeviceMemory` (optimal and linear).

So the guest allocates its buffers **through the host NVIDIA driver** over the
socket, and never lets guest Mesa pick modifiers blindly. NVIDIA LINEAR is
external-only (undisplayable); **block-linear** binds as `GL_TEXTURE_2D`, so GPU
buffers use block-linear DRM modifiers.

## The pipeline

```
Android app ── Vulkan ──▶ guest Mesa Venus (bionic, vulkan.virtio.so)
                              │ Venus protocol over vtest unix socket
                              ▼
                     virglrenderer render server (host)
                              │ real Vulkan
                              ▼
                     NVIDIA proprietary driver ──▶ GPU
                              │ VkImage (block-linear) ─exported▶ dmabuf
                              ▼
       guest gralloc (minigbm vtest backend) imports the dmabuf
                              │
                     hwcomposer.waydroid ──▶ Wayland ──▶ KWin
```

## Rendering backends in the guest

All end up on NVIDIA via Venus:

- HWUI: `debug.hwui.renderer=skiavk` (Skia Vulkan).
- SurfaceFlinger RenderEngine: `skiagl` on **ANGLE** (`ro.hardware.egl=angle`) —
  GL-on-Vulkan-on-Venus (unthreaded).
- GL apps: ANGLE.
- `ro.hardware.vulkan=virtio` selects Venus; `mesa.vn.debug=vtest` +
  `mesa.vtest.socket.name=/dev/venus.sock` select the socket transport.

## Component map

| Component | Repo (upstream) | Change |
|---|---|---|
| Guest Vulkan driver | Mesa `src/virtio/vulkan/` | vtest sync_fd + dma_buf transport; AHB memory steering; UMA memory flags |
| Host renderer | virglrenderer `vtest/`, `src/venus/` | sync_file export, dmabuf-import blob, gpu-alloc command, global-priority strip/retry |
| gralloc | minigbm `gbm_mesa_driver/` | net-new `vtest_wrapper.c` allocating via `VCMD_RESOURCE_ALLOC_GPU` |
| Display | android_hardware_waydroid (hwcomposer) | refresh override, single-window layer selection, bufferless-SKIP draw index |
| Integration | waydroid `lxc.py`, `hardware_manager.py` | emit mounts/props in generators; `suspend_action=none` |

## Waydroid integration points (`patches/waydroid`)

- `tools/helpers/gpu.py` blacklists `nvidia`, so Waydroid auto-picks the other
  render node. The translator sidesteps this: it uses no DRM render node, it
  talks to the host daemon over a socket.
- `tools/helpers/lxc.py` `generate_nodes_lxc_config` emits the config_nodes
  bind-mounts (venus socket + guest `.so`s) so they survive `waydroid upgrade`;
  props live in `waydroid.cfg [properties]`.
- `tools/services/hardware_manager.py` honors `suspend_action = none` so the
  container isn't `lxc-freeze`d when the Android screen blanks.

## Host capabilities relied upon

- `nvidia-drm.modeset=1` ⇒ `VK_EXTERNAL_*_HANDLE_TYPE_SYNC_FD` (fence + semaphore,
  export + import).
- dmabuf export/import + `VK_EXT_image_drm_format_modifier`,
  `external_memory_dma_buf`, `queue_family_foreign`.
- NVIDIA dma_bufs are mmap-able from every memory type on the current driver.
