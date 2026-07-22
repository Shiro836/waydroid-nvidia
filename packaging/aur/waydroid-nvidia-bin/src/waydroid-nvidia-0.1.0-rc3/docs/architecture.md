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
- SurfaceFlinger RenderEngine: `skiaglthreaded` on **ANGLE**
  (`ro.hardware.egl=angle`) — GL-on-Vulkan-on-Venus, composition on a
  dedicated RenderEngine thread (the old prebuilt-ANGLE crash that forced
  unthreaded `skiagl` is gone on the source ANGLE build). In steady state
  SF composites nothing: `persist.waydroid.use_subsurface=true` puts hwc
  in compositing mode — layers are marked DEVICE and their dmabufs attach
  directly to wayland (sub)surfaces, so a fullscreen game's buffer goes
  straight to KWin with no SF GLES pass in between.
- GL apps: ANGLE.
- `ro.hardware.vulkan=virtio` selects Venus; `mesa.vn.debug=vtest` +
  `mesa.vtest.socket.name=/dev/venus.sock` select the socket transport.

## Display: native high refresh

The guest display runs at the monitor's real refresh rate (tested at 500 Hz):

- hwcomposer reports the rate from `persist.waydroid.refresh_rate` (up to
  1000 Hz).
- Stock SurfaceFlinger parks its vsync timer forever at periods ≤ 3 ms
  (`kSnapToSameVsyncWithin` collapses all timeslots). The patched
  SurfaceFlinger (`patches/lineage-20/`) makes the window tunable via
  `debug.sf.snap_to_same_vsync_within_ns`; the config sets 1 ms, enabling
  any refresh rate the display offers. Default behavior is unchanged when
  the prop is unset.

## Frame synchronization: all GPU-side

- **Fences**: a per-context DRM timeline syncobj is shared between guest and
  host once (the container shares the host kernel), so per-frame fence
  export costs kernel ioctls only — zero socket roundtrips.
- **Semaphores**: imported `sync_fd` wait semaphores (BufferQueue acquire
  fences) are forwarded to the host driver as real semaphore imports instead
  of being CPU-waited before submit.

## ASTC texture emulation (Vulkan-native games)

Android mandates `textureCompressionASTC_LDR`; desktop NVIDIA GPUs don't
have it, so Vulkan-native games would sample garbage. The guest Venus driver
emulates it: ASTC images are backed by RGBA host images, and
`vkCmdCopyBufferToImage` uploads are transparently decoded by a compute
shader (mesa's common `vk_texcompress_astc` decoder) recorded inline into
the app's command buffer. Sampling, mips, arrays and sRGB all behave as
native. Opt-out with `VN_NO_ASTC_EMU=1`. ETC2 is not yet emulated.

## Component map

| Component | Repo (upstream) | Change |
|---|---|---|
| Guest Vulkan driver | Mesa `src/virtio/vulkan/` | vtest sync_fd + dma_buf transport; timeline-fence path; semaphore sync_fd import; ASTC LDR emulation; AHB memory steering; UMA memory flags |
| Host renderer | virglrenderer `vtest/`, `src/venus/` | sync_file export, dmabuf-import blob, gpu-alloc command, global-priority strip/retry |
| gralloc | minigbm `gbm_mesa_driver/` | net-new `vtest_wrapper.c` allocating via `VCMD_RESOURCE_ALLOC_GPU` |
| Display | android_hardware_waydroid (hwcomposer) | refresh override; direct (subsurface) composition with compositor-compatibility gate; opaque-layer alpha handling; fence lifecycle fixes; single-window layer selection |
| Guest SurfaceFlinger | LineageOS 20 `frameworks/native` | prop-tunable vsync snap window (enables >333 Hz) |
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
