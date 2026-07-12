# Zero-roundtrip fence export via shared DRM timeline syncobj

Status: **working** (M8.B done 2026-07-12: 0.00% janky, flat 5 ms @250 Hz with
the path engaged). Prereqs researched 2026-07-12 (see STATE.md log: syncobj
ioctls are DRM_RENDER_ALLOW, cross-process and cross-driver by design;
reference designs: linux-drm-syncobj-v1, PipeWire 1.2). Kill switch:
`VTEST_NO_TIMELINE=1` on the server disables the capability param.

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
  and hands `{fd, point}` to a per-context **timeline worker thread** (FIFO,
  keeps chain seqnos monotonic). The worker imports via the one-ioctl
  chain-add `SYNCOBJ_FD_TO_HANDLE(IMPORT_SYNC_FILE|TIMELINE, point)`
  (kernel ≥ 6.16; on EINVAL falls back to scratch-binary
  IMPORT_SYNC_FILE + TRANSFER) and closes the fd. The worker exists because
  fence-release/GC work (`drm_syncobj_add_point` ends with a chain GC walk
  that runs driver release callbacks) and `close()` task_work must not bill
  to the dispatch thread.

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

## The drm_file pid-handover trap (root cause of the initial 38 ms regression)

The first implementation passed the guest a `dup()` of the render worker's
render-node fd. A dup shares the `struct file` → one `drm_file` ioctl'd
alternately by two processes (host worker imports, guest exports). DRM core's
`drm_file_update_pid()` (kernel ≥ 6.6, for accurate fdinfo) runs
`mutex_lock(filelist) + synchronize_rcu()` — ~10 ms under load — on **every
ioctl whose caller tgid differs from the previous one**; its model is "a
single handover followed by exclusive repeated use". Result: every host
import and every guest export ate a synchronize_rcu, 38 ms frames, 94% janky.
The stall lives in `drm_ioctl()` *before* the syncobj handler, bills to
whichever process alternates in, and never reproduces single-process — which
made it look like an NVIDIA driver pathology. (Diagnosed via bpftrace
off-CPU stacks: `synchronize_rcu ← drm_file_update_pid ← drm_ioctl`.)

Fix: `VCMD_TIMELINE_GET` passes the guest a **freshly opened** node fd
(`open()` of the same `/dev/dri/renderD*`), so each `drm_file` has exactly
one user process after a single handover. Rule for any future fd passing:
never share a long-lived drm_file between processes that both ioctl it.

## Measured effect

App-drawer fling @250 Hz, engaged path (2026-07-12): 644–658 frames/run,
0.00–0.15% janky, 5 ms at p50/p90/p95/p99 — equal to the best socket-export
baseline, with SYNC_CREATE/UNREF/EXPORT gone from the per-frame VCMD profile
and zero slow-import diagnostics over full runs. Remaining gap to the 4 ms
DoD median is elsewhere (ring notify / hwc), not in fence export.
