# DRAFT (not sent): sync fds created after a first GPU wait signal before the work they cover

Status: draft. Nothing here has been sent to NVIDIA. It describes observed behaviour only, at the
API level and at the documented nvidia-drm ioctl interface; it contains no disassembly and no internal addresses.

## Summary

On 615.71.09, a fence sync or exported sync_file can signal too early. This happens with an EGL native fence
(`EGL_ANDROID_native_fence_sync`), an `EGL_SYNC_FENCE_KHR` fence, or a Vulkan `SYNC_FD` export from a semaphore or
fence. The fence or fd signals as soon as an earlier *imported* fence signals, instead of after the commands
submitted before it. The trigger is a fresh command stream (a thread's first EGL context binding, or a new
`VkDevice` queue) whose first synchronization operation is a GPU wait on an imported sync_file. A second fence
created later on the same stream is correct.

Observed at the nvidia-drm interface, on a fresh stream:

    DRM_IOCTL_NVIDIA_SEMSURF_FENCE_WAIT    ctx 2  fd 32  pre_wait_value 0  post_wait_value 1   <- eglWaitSync / semaphore wait
    DRM_IOCTL_NVIDIA_SEMSURF_FENCE_CREATE  ctx 2  wait_value 1                                  <- the next fence: = post_wait_value

Expected: the created fence's `wait_value` is a semaphore value released by the GPU after the commands it covers.
Here it is the value the kernel writes when the *imported* fence signals (open kernel module
`nvidia-drm-fence.c`, the `SEMSURF_FENCE_WAIT` callback). The same program with one fence created before the wait
gives:

    FENCE_CREATE ctx 2 wait_value 1;  FENCE_WAIT ctx 2 pre 1 post 2;  FENCE_CREATE ctx 2 wait_value 3   (correct)

## Environment

| Item | Value |
|---|---|
| Driver | 615.71.09, open kernel modules (`nvidia-open-dkms 615.71.09-1`), `nvidia-utils 615.71.09-1` |
| GPU | NVIDIA GeForce RTX 5090 (GB202, PCI 0x2B85), VBIOS 98.02.2E.40.61 |
| OS | Arch Linux, kernel 7.2.9-zen1, glibc, `libglvnd 1.7.0-3`, `vulkan-headers 1.4.363` |
| Display | none needed: EGL device platform (`EGL_EXT_platform_device`), surfaceless contexts; Vulkan without WSI |

## Reproducers (self-contained, no code of ours in the driver path)

1. **EGL**: `test/fence-first-wait.c` (stock libglvnd: `-lEGL -lGLESv2`).

       cc -O1 -o fence-first-wait fence-first-wait.c -lEGL -lGLESv2 -lpthread
       ./fence-first-wait --rounds 20                    # base case: 20 of 20 early
       ./fence-first-wait --rounds 20 --mode warm        # same, one fence created first: 0 of 20

   - Thread T makes a new context current and builds a ~4.7 ms full-screen draw, then reports ready.
   - The main thread (context A) then creates a native fence Fa. Fa is still pending when T uses it.
   - T then runs: import Fa (`EGL_SYNC_NATIVE_FENCE_FD_ANDROID`), `eglWaitSyncKHR`, draw, then
     `eglCreateSyncKHR(EGL_SYNC_NATIVE_FENCE_ANDROID)` + `glFlush` + `eglDupNativeFenceFDANDROID` = Fr.
   - It waits for Fr with `poll()`, then calls `glFinish()`. A round is early when `glFinish` still waited more than
     2 ms after Fr had signalled (measured: correct ≤ 1.2 ms, early 4.6–6.3 ms, i.e. the whole draw).
2. **Vulkan**: `<driver checkout>/hal/test/vk-first-wait.c` (loads the ICD directly with `--icd /usr/lib/libEGL_nvidia.so.0`; no
   loader needed).

       cc -O1 -I<android vk_android_native_buffer.h dir> -o vk-first-wait vk-first-wait.c -ldl -lpthread
       ./vk-first-wait --icd /usr/lib/libEGL_nvidia.so.0 --mode sem-export --rounds 20 --fills 60 --timestamps

   - Each round runs on a new thread with a new `VkDevice`.
   - Sequence: import a pending sync_fd (from a second device) into a semaphore (`SYNC_FD`, `TEMPORARY`); submit
     {wait it; 60 × `vkCmdFillBuffer` over 256 MiB; signal an exportable semaphore}; `vkGetSemaphoreFdKHR(SYNC_FD)`.
   - `--timestamps` brackets the work with GPU timestamps, mapped to `CLOCK_MONOTONIC` through
     `VK_KHR_calibrated_timestamps` (deviation ±0.002 ms). Each round reports when the work really ended relative to
     the moment `poll()` saw the exported fd signalled.

## Expected (specification)

- `EGL_ANDROID_native_fence_sync` (= `EGL_KHR_fence_sync`): "The EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR condition is
  satisfied by completion of the fence command corresponding to the sync object and all preceding commands in the
  associated client API context's command stream. The sync object will not be signaled until all effects from these
  commands on the client API's internal and framebuffer state are fully realized." The native fence object "is
  signaled when the EGL native fence sync object is signaled."
- `EGL_KHR_wait_sync`: eglWaitSyncKHR makes the server "block until <sync> is signaled"; it does not change what a
  later fence covers.
- Vulkan, `vkQueueSubmit`/`vkQueueSubmit2` (Command Buffer Submission): "The first synchronization scope of each
  semaphore signal operation defined by this command includes every command in the same batch that the signal
  operation is defined in and all commands that occur earlier in submission order." The fence is "signaled once all
  submitted command buffers have completed execution." A `SYNC_FD` export carries that signal operation.

## Actual

The fence or exported fd signals together with the imported fence it waited on. The GPU work submitted between the
wait and the fence is still running at that point.

- **EGL:** Fr is reported signalled at creation, or as soon as Fa signals, and `glFinish` then runs for the whole draw.
- **Vulkan:** with timestamps, the work ended **10.5–12.6 ms after** `poll()` had already seen the exported fd
  signalled, in 20 of 20 rounds. In the correct cases it ended 0.016–0.109 ms before.

## Measured conditions (20 rounds each; host above; 2026-10-11)

EGL (`fence-first-wait --mode M`). Fa covered a clear (~0 ms of GPU work) or, with `--a-iters 4000000`, an 88 ms
draw. Both series give identical counts.

| Thread T's sequence before Fr | Early rounds |
|---|---|
| wait(Fa); draw; Fr (`first-wait`) | 20 / 20 |
| `eglReleaseThread` after earlier fences, make-current again; wait; draw; Fr (`release-thread`) | 20 / 20 |
| context already used for fences on another thread, first used on T: wait; draw; Fr (`main-warm-ctx`) | 20 / 20 |
| two waits; draw; Fr (`two-waits`) | 20 / 20 |
| `eglClientWaitSyncKHR(Fa, timeout 0)` → `EGL_TIMEOUT_EXPIRED_KHR` (20/20); then wait; draw; Fr (`client-wait-first`) | 20 / 20 |
| wait; draw; Fr = **`EGL_SYNC_FENCE_KHR`** (non-native), polled with `eglClientWaitSyncKHR` (`khr-fence`) | 20 / 20 |
| one native fence (create + flush + dup) first (`warm`) | 0 / 20 |
| `eglCreateSyncKHR(NATIVE)` + `eglDestroySyncKHR` first, no flush, no dup (`warm-nodup`) | 0 / 20 |
| one `EGL_SYNC_FENCE_KHR` first (`khr-warm`) | 0 / 20 |
| wait; then a native fence; draw; Fr (`warm-after-wait`): only the fence right after the wait is affected | 0 / 20 |
| make W current, then switch to B; wait; draw; Fr (`switch-ctx`) | 0 / 20 |
| W: wait; glFlush; switch to B; draw; Fr (`switch-after-wait`) | 0 / 20 |
| make-current B, release, make-current B; wait; draw; Fr (`rebind`) | 0 / 20 |
| the wait in the main thread, which created fences before (`same-thread`) | 0 / 20 |
| T1: wait; draw; unbind. T2 binds the same context and reads pixels, no user fence (`migrate`, `migrate-warm`) | 0 / 20 each |

Vulkan (`vk-first-wait`), via the raw ICD (and, identically, via an Android Vulkan HAL wrapper that passes these
calls through):

| Fresh device/queue; sequence | Early rounds |
|---|---|
| import → semaphore; submit {wait; work; signal exportable semaphore}; `vkGetSemaphoreFdKHR` | 20 / 20 |
| import → semaphore; submit {wait; work} + exportable fence; `vkGetFenceFdKHR` | 20 / 20 |
| import → semaphore + fence (`TEMPORARY`); submit {wait; work; signal S}; submit {wait S} + exportable fence; `vkGetFenceFdKHR` (the Android `vkAcquireImageANDROID` / `vkQueueSignalReleaseImageANDROID` pattern) | 20 / 20 |
| the same, preceded by one frame whose acquire fd is −1 | 0 / 20 |
| the three cases above, each preceded by one empty submit + fence export | 0 / 20 each |
| import → fence; `vkWaitForFences` (CPU wait); submit work + exportable fence; export | 0 / 20 |
| no import; submit work + exportable fence; export | 0 / 20 |

## What the behaviour looks like from outside

This is a description of the observed behaviour, for triage.

1. Each EGL thread gets its own semaphore fence context the first time a context is made current on it
   (`SEMSURF_FENCE_CTX_CREATE`). Every context bound on that thread shares it. `eglReleaseThread` and thread exit drop
   it, and the next make-current creates a new one. Each `VkDevice` creates its own contexts at `vkCreateDevice`.
2. On a fresh context, the first fence creation first submits a semaphore release covering the preceding work, then
   creates the fence on that value (`wait_value 1`). Later submissions release increasing values and fences use
   them.
   - Fence creation includes the driver's own: `eglMakeCurrent` to another context or to `EGL_NO_CONTEXT`,
     `eglReleaseThread` and thread exit each create one.
3. A GPU wait processed before any fence creation advances the context's value (`FENCE_WAIT pre 0 post 1`), but the
   submissions that follow do not release values. The next fence creation then uses the wait's `post_wait_value`.
   The CPU writes that value as soon as the imported fence signals. The fence covers nothing submitted after the
   wait, and, since `pre_wait_value` is 0, nothing the stream submitted before the wait either.
4. From that fence on, the context behaves correctly.

## Impact

- **Android.** SurfaceFlinger's RenderEngine (GLES) waits the layers' acquire fences with
  `eglCreateSyncKHR(NATIVE, fd)` + `eglWaitSyncKHR`, renders, then creates its flush fence. On a fresh RenderEngine
  thread this is exactly the failing sequence. The same holds for any app thread whose first native-fence operation
  is a wait on a dequeued buffer's release fence.
- **Vulkan.** A `VkDevice` whose first queue operation waits on an imported sync_fd exports a release fence that
  does not cover its rendering. This would affect, for example, a new device presenting into buffers that still carry
  release fences, or a compositor whose first operation on a new device waits on an imported client sync_file
  (inferred from the reproducer, not observed in those programs).
- The consumer then reads, or the producer overwrites, a buffer that is still being written. A single frame is
  affected, the first one on each new thread or device.

## Side observation (separate, not a correctness bug)

The first `eglMakeCurrent` on a new thread blocks until previously submitted GPU work from other threads has
completed: 889–892 ms with an 879 ms draw outstanding on another thread. Whether `vkCreateDevice` does the same was
not measured. Because of this, a naive reproducer always finds the imported fence already signalled;
`fence-first-wait.c` sets up its thread before the fence it imports exists.
