# Zero-roundtrip fence export via shared DRM timeline syncobj

Status: implementing (M8.B). Owner: dev loop. Prereqs researched 2026-07-12
(see STATE.md log: syncobj ioctls are DRM_RENDER_ALLOW, cross-process and
cross-driver by design; reference designs: linux-drm-syncobj-v1, PipeWire 1.2).

## Problem

Android needs a `sync_file` fd per rendered frame (HWUI → SF → hwc → KWin).
Over vtest today each export costs the guest: SYNC_CREATE (roundtrip) +
SUBMIT_CMD2 (write) + SYNC_EXPORT_SYNC_FILE (roundtrip, 79–765 µs server-side
— vtest fork → proxy socket → render worker → GetFenceFdKHR under the
sync-thread mutex → fd back over two sockets) + SYNC_UNREF (write).
Measured (VTEST_STATS): SF pays ~765 µs/frame, HWUI ~66 µs + 4 socket ops.

## Insight

Waydroid is a container: guest and host share one kernel. A DRM timeline
syncobj created by the render worker is directly usable by the guest — no
socket traffic per frame, only kernel ioctls.

## Design

One timeline syncobj per venus context, created lazily by the **render
worker** (it owns the VkFences), shared with the guest once.

Per frame:
- **Guest (mesa vtest)**: on submit with syncs, assigns `point = ++counter`
  (uint64, per context) and sends it in the submit. On
  `export_syncobj(sync_file=true)`:
  `DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT(WAIT_AVAILABLE, point)` (blocks only until
  the host has *materialized* the fence, i.e. done QueueSubmit+import — not
  until it signals), then `SYNCOBJ_TRANSFER(timeline@point → tmp binary)` +
  `SYNCOBJ_EXPORT_SYNC_FILE(tmp)`. Zero socket ops.
- **Render worker (vkr)**: `vkr_queue_sync_submit` already does
  `QueueSubmit(fence)`; when the submit carries a point, immediately
  `GetFenceFdKHR(SYNC_FD)` (copy transference, legal with a pending signal op)
  and import: `SYNCOBJ_FD_TO_HANDLE/IMPORT_SYNC_FILE(tmp binary)` +
  `SYNCOBJ_TRANSFER(tmp → timeline@point)`. In-order per queue, so points
  materialize in order.

## Wire/API additions (all gated, fallback = old export path)

| Layer | Addition |
|---|---|
| vtest protocol | `VCMD_PARAM_HAS_TIMELINE_FENCE`; `VCMD_TIMELINE_GET {ring?}` → status + syncobj fd; `VCMD_SUBMIT_CMD2_TIMELINE {point_lo,point_hi} + submit_cmd2 payload` |
| virglrenderer API | `virgl_renderer_context_create_fence2(..., uint64 point)`, `virgl_renderer_context_get_timeline_fd(ctx_id, &fd)` |
| proxy protocol | `submit_fence` request gains `uint64 timeline_point` (proxy and worker ship from the same build — safe); new `TIMELINE_GET` request/reply-with-fd |
| vkr | per-context `{drm_fd, timeline_handle}` lazily created (scan `/dev/dri/renderD*` for `DRM_CAP_SYNCOBJ_TIMELINE`); import-at-point in `vkr_queue_sync_submit` |
| guest mesa | vtest: param probe, own render-node fd (any node; syncobjs are device-agnostic), timeline import at init, per-submit points, local export; `vn_queue.c` unchanged |
| waydroid | lxc.py generator: bind-mount a render node into the container |

## Correctness notes

- dma_fence in a syncobj is driver-agnostic; the guest node (e.g. amdgpu) can
  hold NVIDIA fences.
- `WAIT_AVAILABLE` waits for materialization only; the real GPU wait stays on
  the consumer (KWin) exactly as today.
- Timeline points materialize in submission order per queue (single vkr queue
  per ring); cross-ring order is irrelevant — each point is only consumed
  individually.
- Guest wait timeout (1 s): on timeout or any ioctl failure, fall back to the
  old socket export; on device-lost the old path also reports it.
- sync_file export requires a *materialized* point — hence WAIT_AVAILABLE
  before TRANSFER, two-step via tmp binary syncobj (portable to kernels < 6.11).

## Expected effect

SF: −~0.7 ms/frame critical path; HWUI: −4 socket ops −~0.1 ms; removes
SYNC_CREATE/UNREF/EXPORT from the per-frame VCMD profile entirely (verify with
VTEST_STATS after).
