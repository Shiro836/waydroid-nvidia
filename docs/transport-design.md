# Transport design: extending vtest for Venus-on-NVIDIA

The Venus guest driver talks to the host over Mesa's **vtest** unix-socket
transport (dev-quality, normally used for testing). Stock vtest lacks three
things HWUI/gralloc need. Each was added as a small, symmetric patch pair
(mesa guest side ↔ virglrenderer host side), fd-passing done with `SCM_RIGHTS`.

## 1. sync_fd over vtest (`VK_KHR_external_{fence,semaphore}_fd`)

**Why:** guest Venus gates the extensions on `renderer->info.has_external_sync`;
vtest set it false. HWUI hard-requires `VK_KHR_external_semaphore_fd`
(`VulkanManager` asserts on it) to sync with the compositor.

**Host (virglrenderer):**
- `RENDER_CONTEXT_OP_EXPORT_FENCE {fence_id}` → reply `{found}` + one fd.
- vkr dispatch: lock queues, find the pending sync by `fence_id`,
  `vkGetFenceFdKHR(SYNC_FD)` (copy-transference semantics, safe vs the waiter
  thread), reply. Not found ⇒ already retired ⇒ `found=0` (guest returns fd −1 =
  "already signaled", spec-legal).
- Public API `virgl_renderer_context_export_fence(ctx, fence_id, &fd)` (bypasses
  the DRM/GL-only fence table).
- vtest: `VCMD_SYNC_EXPORT_SYNC_FILE` + `VCMD_PARAM_HAS_VENUS_SYNC_FD`.

**Guest (mesa `vn_renderer_vtest.c`):** query the param → `has_external_sync =
true`; implement `export_syncobj` (fd, or −1 on SIGNALED). `vn_queue.c` needs no
changes (all transport-side). `vn_create_sync_file` accepts fd == −1.

**Gotcha:** exporting a fence SYNC_FD **resets the fence** (copy-transference) —
never wait on the fence after export.

Verified host-side (`tests/`): sync_fd through the socket signals in 0.67 ms,
identical to direct NVIDIA.

## 2. dma_buf import over vtest (`VK_ANDROID_external_memory_android_hardware_buffer`)

**Why:** HWUI's `SkSurface::MakeFromAHardwareBuffer` →
`vn_get_memory_dma_buf_properties` hit a NULL renderer op; vtest had no import path.

- `VCMD_RESOURCE_IMPORT_BLOB`: client sends the dmabuf fd via `SCM_RIGHTS` +
  res_id; server imports and attaches (size-0 → `lseek`). Server plumbing reused
  `RENDER_CONTEXT_OP_IMPORT_RESOURCE` / `virgl_renderer_resource_import_blob`.
- mesa: `vtest_bo_create_from_dma_buf` + `has_dma_buf_import = true` + the
  dma_buf properties query path.

**Two real bugs fixed here:** (1) `virgl_resource_create_from_fd` **takes
ownership** of the fd — closing it forwarded a dead fd (intermittent
`invalid res_id` → CS-fatal context → vkCreateDevice crash-loop). (2) import must
be visible before the client gets the res_id — the ring-0 export_fence became a
wire round-trip barrier.

## 3. host-side GPU allocation (`VCMD_RESOURCE_ALLOC_GPU`)

**Why:** the display finding — guest buffers must be **NVIDIA-native** or KWin
can't bind them. So gralloc allocates *through the host*.

- New host allocator `src/virglrenderer-vtest/vtest_gpu_alloc.c`: GPU buffers as
  exportable `VkImage`s with real NVIDIA **block-linear** DRM modifiers (LINEAR is
  external-only on NVIDIA EGL ⇒ undisplayable; block-linear binds as
  `GL_TEXTURE_2D`). CPU-mappable buffers as NVIDIA linear images (or udmabuf
  fallback).
- Guest gralloc backend `src/minigbm-vtest/vtest_wrapper.c` replaces
  `libgbm_mesa_wrapper.so` — allocations go over the socket instead of guest libgbm.

## AHB memory-type fixes (mesa `0003-wip`)

ANGLE adds `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT` to every sampled AHB image;
NVIDIA rejects the dedicated import (`vkAllocateMemory` → −11) for LINEAR-modifier
images with that usage. Guest-mesa fixes:

- Force `DRM_FORMAT_MODIFIER` tiling in deferred AHB init (older ANGLE passes
  legacy LINEAR).
- Strip `INPUT_ATTACHMENT` for LINEAR-modifier imports.
- Accept LINEAR tiling in AHB format queries.
- UMA-style `DEVICE_LOCAL` on all memory types/heaps.

## Global-priority strip/retry (virglrenderer `vkr_device.c`)

HWUI requests a global queue priority
(`VkDeviceQueueGlobalPriorityCreateInfoKHR`); NVIDIA returns
`VK_ERROR_NOT_PERMITTED_KHR` for unprivileged REALTIME/HIGH, but Android expects
best-effort. On `NOT_PERMITTED`, strip the priority structs from the queue pNext
chains and retry `vkCreateDevice` once.

## Performance characteristics

- The socket transport adds per-op overhead vs direct calls; Vulkan batches by
  design, so few submits per frame keep this acceptable.
- Plain host-visible allocations are steered to a CACHED memory type of the same
  heap (write-combined sysmem through the socket is slow otherwise).
- dma_buf mmap read bandwidth is below native — relevant to readback / lockCanvas
  paths, not the hot render path.
