# Fence contract: NVIDIA native fences, dma-buf implicit sync, and buffer hand-off on Android

Driver 615.71.09 (open KM), RTX 5090, host Arch, kernel 7.2.9-zen1. Measured 2026-10-11. Invariant I (§6a) is
enforced by the front and the HAL since 2026-10-11. Every line carries a
source (file:line at a pinned commit) or a command plus its measured output. "Not established" means nobody looked
it up or measured it, not "probably fine".

Sources:

| Abbreviation | Source |
|---|---|
| **KM** | `build/ogkm-615.71.09` = NVIDIA/open-gpu-kernel-modules tag 615.71.09 (commit 61dcc937), matching `/proc/driver/nvidia/version` |
| **K** | `build/p7/khronos/*.txt`, fetched from registry.khronos.org/EGL/extensions/ on 2026-10-11 (sha256 `EGL_ANDROID_native_fence_sync.txt` 96fb65ae…, `EGL_KHR_fence_sync.txt` 8973f2a9…, `EGL_KHR_wait_sync.txt` 46f5b927…) |
| **W** | egl-wayland2 v1.0.2 (5a2c1cbe) |
| **X** | egl-x11 v1.0.6 (4a89da50) |
| **M** | Mesa 25.2.0 `platform_android.c` (0b97dcdd) |
| **FN** | frameworks/native android-13.0.0_r31 (2fe7eddf); all of these are under `build/p7/src/` |

Tools: `test/fence-first-wait.c` (stock libglvnd, no code of ours; `--front` runs the same sequences through our front),
`test/fence-race.c`, `test/host-window.c`, `<driver checkout>/hal/test/vk-first-wait.c` (raw ICD or our HAL). The
ioctl log comes from a measurement-only `ioctl` interposer (LD_PRELOAD, host only, not shipped) that prints the
`DRM_NVIDIA_SEMSURF_*` arguments.

## 1. What the specification promises

1. **Creation.** A native fence sync created with no fd inserts a fence command into the *current context's*
   stream. The fd exists from the next Flush (K `EGL_ANDROID_native_fence_sync.txt:118-128`). Order: create, then
   flush, then dup.
2. **Signal = completion.** "satisfied by completion of the fence command … and all preceding commands in the
   associated client API context's command stream. The sync object will not be signaled until all effects from these
   commands on the client API's internal and framebuffer state are fully realized" (K
   `EGL_ANDROID_native_fence_sync.txt:135-140` = `EGL_KHR_fence_sync.txt:236-242`).
   - Nothing is said about cross-process or cross-API memory visibility beyond this sentence.
3. **Import.** An imported fd is owned by EGL from `eglCreateSync` on (K `…native_fence_sync.txt:105-110`). An
   imported sync inserts no fence command; its condition is "the native fence object signalled" (`:142-145`).
4. **eglWaitSync.** Blocks only the issuing context's subsequent commands; "other contexts … are not affected"
   (K `EGL_KHR_wait_sync.txt:89-103`, fn2).
   - So a wait must be issued **in the context that will touch the buffer**, before those commands.

## 2. What the kernel does (nvidia-drm, 615.71.09)

1. **Fence fd = semaphore threshold.** `SEMSURF_FENCE_CREATE` makes a sync_file that signals when "the semaphore at
   the specified index reach[es] a value >= wait_value" (KM `kernel-open/nvidia-drm/nv_drm_common_ioctl.h:382-397`;
   `nvidia-drm-fence.c:1466` dma_fence seqno = wait_value).
2. **The kernel only reads; the GPU writes.**
   - The CPU reads the sysmem semaphore with `READ_ONCE` (fence.c:741) and signals when `fenceSeqno <= currentSeqno`
     (fence.c:847).
   - Semaphore surfaces are sysmem-only (KM `src/nvidia/src/kernel/gpu/mem_mgr/sem_surf.c:758-765`).
   - The GPU writes the value through a semaphore release in the closed userspace pushbuffer. The kernel emits no
     WFI, membar or L2 flush on this path; the only barriers are CPU-side `portAtomicMemoryFenceFull()` (sem_surf.c:69,
     122).
   - **Whether a value covers prior GPU work is decided entirely by the userspace driver**, i.e. by which value it puts
     in `wait_value`. Section 3.3 shows where that goes wrong.
3. **Timeout.** A fence also "signals" with `-ETIMEDOUT` after `timeout_value_ms`. A value of 0 or more than 5000 is
   forced to 5000 (fence.c:43, 1447-1449, 895-901).
   - Measured: libEGL_nvidia passes `timeout 0`, i.e. 5 s (ioctl log: `FENCE_CREATE ctx 5 timeout 0 wait 1`).
   - So a signalled fd is not proof of completion when the work ran longer than 5 s; only the fence status (sync_file
     info) tells. The kernel's wait path proceeds on any signal (fence.c:1716-1732); measured 2026-10-11, eglWaitSync
     on an errored fd returns `EGL_TRUE` and the GPU proceeds (§7).
4. **GPU wait on an fd.** `SEMSURF_FENCE_WAIT(fd, pre, post)` registers a dma_fence callback. Once the fd signals
   *and* the context's semaphore has reached `pre`, the CPU writes `post` (fence.c:1585-1597, 1716-1732;
   sem_surf.c:118-124: mfence, then store). The GPU then acquires `post` from the userspace pushbuffer.
   - The ioctl returns 0 even on a bad fd or allocation failure (fence.c:1748-1749).
   - If `dma_fence_add_callback` fails it logs "Signaling early!" and writes `post` immediately (fence.c:1721-1732).
5. **Implicit sync: NVIDIA does not participate.**
   - Fences enter a buffer's reservation object only through the explicit `GEM_PRIME_FENCE_ATTACH`/`SEMSURF_FENCE_ATTACH`
     ioctls (fence.c:494-508, 588, 1817). Nothing is attached at submit; the kernel never sees submits.
   - Our gralloc buffers are nvidia-drm GEM exports. Measured: `/proc/self/fdinfo` shows `exp_name: drm`;
     `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` returns 0 with a sync_file.
   - Measured, `fence-race --iters 200000`, 5/5 runs: the exported implicit fence is **already signalled while a flushed
     NVIDIA GL write into that dma-buf is still running** (the native fence for the same work signalled 4.4 ms later).
   - NVIDIA's own platform says the same: "The NVIDIA driver does not support implicit sync semantics in OpenGL" (X
     `src/x11/x11-platform.c:1013-1016`).
   - So `DMA_BUF_IOCTL_SYNC`, EXPORT_SYNC_FILE and poll() on the dma-buf give **no ordering against NVIDIA GPU work**.
     Every hand-off must carry an explicit fence. The RM exporter `nv_dma_buf_ops` has no begin/end_cpu_access either
     (KM `kernel-open/nvidia/nv-dmabuf.c:1565-1570`).

## 3. What the userspace driver does (measured; ioctl log + reproducers)

1. **Per-thread timeline.** Each thread that first touches native fences gets its own semaphore context
   (`SEMSURF_FENCE_CTX_CREATE`, one per thread id in the log: e.g. `tid …789 CTX_CREATE index 6 -> handle 5`).
   Contexts made current in the same thread share it.
2. **Normal behaviour.**
   - `eglCreateSync(NATIVE)` + flush + dup gives `FENCE_CREATE(wait = V)`, with V a new value released after the
     preceding work.
   - Importing a fence with `eglWaitSync` gives `FENCE_WAIT(pre = last, post = last + 1)`.
   - A fence created after a wait takes a value above `post` (log: `FENCE_WAIT pre 1 post 2` → `FENCE_CREATE wait 4`).
3. **DEFECT (D1 root cause).** **When a thread's first native-fence operation is an `eglWaitSync` on an imported
   native fence, the next native fence created in that thread gets `wait_value = post` of that wait.** Effects:
   - The fence is satisfied by the CPU's write of `post` as soon as the *imported* fence signals.
   - It covers none of the work the thread submitted after the wait.
   - It is reported signalled at creation while that work is still running.
   - This breaks §1.2.

   Evidence:
   - Stock glvnd, no code of ours (`test/fence-first-wait.c`, `-lEGL -lGLESv2`, renderbuffer, 200k-iteration draw):
     **10/10 rounds "Fr SIGNALLED at creation; the draw it covers finished 4.43–4.98 ms later"**. With `--warm` (the
     thread creates one native fence before its first wait): 0/10. With `--same-thread`: 0/10.
   - `fence-race --iters 200000` through glvnd `libEGL.so.1`, 20 runs each: consumer thread 20/20 signalled at
     creation and 20/20 with poison incomplete in memory at signal time; same thread 0/20; with `--c-warmfence` 0/20.
   - Bisection, 20 runs per row, our front:

     | Consumer variant | Early fences |
     |---|---|
     | own thread, eglWaitSync, then draw, then fence | 20 |
     | CPU wait instead of eglWaitSync | 0 |
     | warm fence before the wait | 0 |
     | warm fence with another context current in the same thread | 0 |
     | GPU work (no fence) before the wait | 20 |
     | acquire fence already signalled | 20 |
     | context C warmed in the main thread, then used in a new thread | 20 |

     **The state is per thread, not per context.**
   - The memory read at signal time (dma-buf mmap): 25,208 of 32,768 poison pixels.
   - Spy census, 50 host-window runs × 60 frames: fence created right after a wait whose timeline's first operation was
     that wait: **50/50** have `wait <= post`. First wait after earlier creations: 0/50. Later waits: 0/5,800.
4. **What still holds (measured).**
   - Native fences created on a timeline that started with a creation cover the preceding work: `fence-probe` 0 early;
     `fence-race` same-thread 0/20; host-window steady state 0/5,850.
   - eglWaitSync itself waits correctly (no failure was ever traced to a wait).
5. **The state machine behind §3.3** (2026-10-11; Ghidra on `libnvidia-eglcore.so.615.71.09` plus gdb breakpoints on
   the same code in live runs plus the ioctl log; behaviour only).
   - **Timeline state.** Each stream has one state: semaphore fence context `ctx`, value `V`, flag `started`, one
     pending-wait slot, one cached "latest fence".
     - EGL: one state per thread, created at the first `eglMakeCurrent(ctx)` on a thread that has none
       (`CTX_CREATE`, `V = 0`, `started = 0`) and shared by every context bound on that thread. Destroyed by
       `eglReleaseThread` and thread exit. Context switches, unbinding and `eglDestroyContext` keep it (lifecycle
       probe).
       - Which entry points create one (measured 2026-10-11, 24 rounds per mode through stock glvnd, verdict by GPU
         timestamps, plus the ioctl log): only that make-current. A context with a config and a pbuffer surface
         creates the state exactly like a no-config surfaceless one (`pbuffer-first` 24/24 early;
         `warm-then-pbuffer` 0/24). A second EGLDisplay of the same device (`EGL_TRACK_REFERENCES_KHR` display) shares
         the thread's state (`second-display` 0/24; no second `CTX_CREATE` on that thread). `eglTerminate` +
         `eglInitialize` keep it: of a second display (`terminate-reinit` 0/24, control `reinit-noterm` 0/24) and of
         the process's only display (ioctl log, three terminate/initialize/bind cycles on one thread: the same
         semaphore context, values continuing 1, 2 | 3, 4 | 5, 6, no `CTX_CREATE` after the first).
     - Vulkan: one state per **VkQueue**, created with the device (`vkCreateDevice` + `vkGetDeviceQueue` make
       3 + one `CTX_CREATE` per created queue). A queue's submissions, waits and exports all use its own state; a
       start on one queue does not cover another queue of the same family or of another family; idling, other
       threads and the `vkGetDeviceQueue2` handle change nothing (`<driver checkout>/hal/AHB-CONTRACT.md` §8.1).
   - **Wait** (`eglWaitSync[KHR]` on an imported fd; Vulkan semaphore wait on an imported `SYNC_FD`):
     - The fd is stored, and merged with any already pending wait, so two waits become one `FENCE_WAIT`.
     - At the next submission: `FENCE_WAIT(pre = V, post = V + 1)`, `V := V + 1`, GPU acquire of `V` ahead of the
       submission's commands.
     - **It neither tests nor sets `started`.** An fd of −1 issues no `FENCE_WAIT`.
   - **Submission** (flush/kickoff): only when `started`: `V := V + 1`, GPU release of `V` after the submission's
     commands, cached fence dropped.
   - **Fence creation**
     - Covers `eglCreateSync(NATIVE)`, `eglCreateSync(EGL_SYNC_FENCE_KHR)`, `vkGetFenceFdKHR`,
       `vkGetSemaphoreFdKHR(SYNC_FD)`, and the driver's own fence at every context unbind (`eglMakeCurrent` to
       another context or to NONE, `eglReleaseThread`, thread exit).
     - Steps:
       1. Pending work is submitted first.
       2. A cached fence is returned if there is one.
       3. Otherwise, if `!started`: `started := 1` and a "submit if needed" step runs. That step submits when work is
          pending, or when `started && V == 0` (this clause forces the first release of a fresh state).
       4. `FENCE_CREATE(wait_value = V)`.
     - The EGL native fence is created at `eglCreateSync` time, not at dup time.
   - **D1 precisely.** At the first fence creation on a state, `started == 0` and `V != 0`. Only a processed wait
     can make `V != 0` while `started == 0`, because submissions do not advance `V` before `started`. Then:
     - The work after the wait was already submitted by step 1, with no release, because `started` was 0.
     - The `V == 0` clause does not fire, so nothing is submitted.
     - The fence gets `wait_value = V`, the last wait's `post`, which the CPU writes as soon as the imported fd
       signals.
     - From then on `started == 1`, so only that one fence per state is poisoned. If the driver's unbind fence is
       the first creation, it takes the poisoned value instead; measured `wait_value 1` after `FENCE_WAIT post 1`,
       with no observed effect (`--mode migrate`, 0/20).
   - **Sequences, 20 rounds each** (`test/fence-first-wait.c --mode`; Fa pending, both `--a-iters 4000000` and a
     clear-only Fa):
     - Early in 20/20: `first-wait`, `release-thread`, `main-warm-ctx`, `two-waits`, `client-wait-first` (the
       `eglClientWaitSync` timed out in 20/20), `khr-fence` (`EGL_SYNC_FENCE_KHR` is affected too).
     - Early in 0/20: `warm`, `warm-nodup`, `warm-after-wait`, `khr-warm`, `khr-fence-warm`, `same-thread`,
       `switch-ctx`, `switch-after-wait`, `rebind`, `migrate`, `migrate-warm`.
     - The traces of `warm-nodup`, `switch-ctx`, `release-thread`, `two-waits` and `main-warm-ctx` follow the
       transitions above step by step.
   - **Vulkan, same defect** (`<driver checkout>/hal/test/vk-first-wait.c`, fresh device, queue and thread per round, 20 rounds,
     GPU timestamps calibrated to `CLOCK_MONOTONIC`):
     - The work ends 10.5–12.6 ms *after* `poll()` saw the exported fd signalled in 20/20 for `sem-export`,
       `fence-export` and `hal-seq`. `hal-seq` is the exact `vkAcquireImageANDROID` → submit →
       `vkQueueSignalReleaseImageANDROID` sequence of our HAL.
     - The results are identical through the raw ICD and through `vulkan.nvidia.so`.
     - 0/20, with the work ending 0.016–0.109 ms *before* the fd was seen signalled: the `-warm` variants,
       `hal-seq-m1first` (frame 1 acquires −1, which starts the timeline), `fence-import-cpu` and `no-import`.
   - **Side fact.** The first `eglMakeCurrent` on a new thread blocks until earlier GPU work of other threads has
     completed (889–892 ms with an 879 ms draw outstanding). Reproducers must set threads up before the fence they
     import exists.
6. **Bug report draft:** `NVIDIA-BUG-D1.md` (not sent).

## 4. How NVIDIA's own platforms hand buffers back

1. **Explicit sync, release to new back buffer.**
   - W: a CPU `WAIT_AVAILABLE` first, then the release point is exported as a sync_file and waited on the GPU:
     `CreateSync(NATIVE, fd)` + `WaitSync` (W `src/wayland/wayland-swapchain.c:442-458`, 501-571).
   - **Then** `PlatformSetColorBuffersNVX(GL_BACK, next)` (W `src/wayland/wayland-surface.c:1556-1570`).
   - X does the same: `GetFreeBuffer` → wait, then SetColorBuffers (X `src/x11/x11-window.c:2255`, 2265-2275).
   - **Both wait before SetColorBuffers.** Our platform did the opposite until 2026-10-11 (SetColorBuffers, then
     the wait); it now waits first and attaches second, as they do (`src/window.c`, `nvnSwapBuffers`, step 7 of the
     file header).
2. **Acquire fence.** Created, flushed and duped after the driver's internal swap (W wayland-surface.c:1245-1271; X
   x11-window.c:1564-1582, where X flushes *before* CreateSync).
3. **First use and realloc.** Fresh or reallocated buffers start IDLE and are rendered to with no wait (W
   wayland-swapchain.c:203; X x11-window.c:355). Old buffers are freed without waiting.
4. **Implicit sync.** Used only toward non-NVIDIA servers, by IMPORT/EXPORT_SYNC_FILE of explicit fences (W
   wayland-display.c:1013-1015, wayland-platform.c:328-353).
5. **Fallbacks.** No native_fence_sync → `glFinish` (W wayland-surface.c:1238-1241; X x11-window.c:1555-1560).

## 5. Android and Mesa

1. **What a dequeue fence means.**
   - Never-used slot: NO_FENCE (FN `libs/gui/include/gui/BufferSlot.h:183`).
   - Reallocated slot: `mFence = NO_FENCE` (FN `libs/gui/BufferQueueProducer.cpp:493-505`).
   - Reused slot: the release fence (`:525-527`).
   - "the fence that must signal before the dequeued buffer may be written to. A value of -1 indicates that the caller
     may access the buffer immediately" (FN `libs/nativewindow/include/system/window.h:500-505`).
2. **Release fence.** "the buffer is no longer in use"; the consumer may still be accessing it when it calls
   releaseBuffer (FN `IGraphicBufferConsumer.h:115-117`).
   - SurfaceFlinger's RenderEngine GPU-waits acquire fences: `eglCreateSyncKHR(NATIVE, fd)` + `eglWaitSyncKHR`
     (FN `libs/renderengine/skia/SkiaGLRenderEngine.cpp:434-466`).
   - It then creates its flush fence with create, glFlush, dup (`:402-425`).
   - **That is the exact sequence of §3.3 on a fresh RenderEngine thread.** This is an inference; it was not measured
     in the guest.
3. **Mesa's in-fence.**
   - Mesa stores the dequeue fence and, with native fences, attaches it to the back image (M `platform_android.c:112-127`).
   - It is waited with `fence_server_sync` when the image is bound as the draw buffer, i.e. GPU-side and before any
     rendering into it. -1 is a no-op.

## 6. The platform's obligations, and what it may rely on

| # | Hand-off | Obligation | Relies on |
|---|---|---|---|
| H1 | dequeue to producer render, buffer reused | the release fence gates every command that touches the buffer | §1.4 (wait in the rendering context); §3.4 (the wait is honoured) |
| H2 | dequeue at surface creation (no context current) | CPU wait on the fence (`window.c:250-254`) | the fd's signal; §2.3 caveat |
| H3 | first use or reallocated buffer | fence -1 means write at once (§5.1). The previous owner's GPU work is covered by the fence the window had stored | §5.1 |
| H4 | producer render to consumer (queue) | acquire = native fence after all rendering into the buffer (`window.c:120-135`, 355-359) | **§1.2, which the driver breaks (§3.3) whenever the app thread's first native-fence op was an eglWaitSync** |
| H5 | consumer to producer (release) | the consumer's release fence must cover its accesses | **the consumer's driver honouring §1.2; with NVIDIA consumers (SF RenderEngine, our harness) §3.3 breaks it on a fresh thread** |
| H6 | CPU access | only through explicit fences; implicit sync is absent (§2.5) | — |

**Status (2026-10-11):** H4 and H5 rest on §1.2, which 615.71.09 breaks for one fence per timeline (§3.3, §3.5).
They hold by construction because invariant I (§6a) is enforced outside the driver, on every timeline our code can
be reached through: the EGL front starts each thread's timeline in `eglMakeCurrent`, the Vulkan HAL starts each
queue's timeline in `vkCreateDevice`. The blob is not touched.

The first version of this document rejected such a start ("a warm-up native fence per thread would hide the defect,
rely on unpublished per-thread state, and not cover fences created by app or SurfaceFlinger code"). That was
reversed once the mechanism was understood, because each objection was answered:
- *Hides the defect*: `make driver-check` (here and in `<driver checkout>/hal`) keeps detecting it through stock glvnd and the raw
  ICD and fails on 615.71.09; the bug report draft stays (`NVIDIA-BUG-D1.md`).
- *Unpublished state*: the start relies on one measured fact per API, that a timeline is created only inside
  `eglMakeCurrent` / with a VkQueue at `vkCreateDevice` (§3.5, with the creation census above), not on its internals.
  If a future driver creates timelines elsewhere, `make front-fence-check` / `make hal-fence-check` fail.
- *App and SurfaceFlinger fences*: the start is not "our fences are fine"; it puts every timeline into the state in
  which **every** later fence on it, whoever creates it, covers its work (§6a proof). App and SurfaceFlinger threads
  reach the driver only through the front's `eglMakeCurrent` (Android's loader dlsym()s it; `eglGetProcAddress`
  returns it too).

## 6a. Invariant I: the implemented contract (2026-10-11)

**Invariant I.** On every timeline state, the first fence creation happens while `V == 0`, i.e. before any wait is
processed on that state. Equivalently, `started == 1` before the first `FENCE_WAIT` on it.

**Why I suffices (against §3.5).**
- `V` changes only in two places: a submission when `started`, and a processed wait. So before any wait,
  `V == 0`.
- A fence creation with `started == 0, V == 0` sets `started` and forces a submission that releases `V = 1` after
  all earlier commands of the stream. Its fence covers them.
- From then on, every submission ends with a release of `++V` and drops the cached fence. Every wait takes
  `pre = V`, so the kernel writes `post` only after the GPU released `V` (KM fence.c:1585-1597), and the GPU
  acquires `post` before later commands. Every later fence creation submits pending work first and takes the `V`
  released at the end of that submission. So each fence covers every command submitted before it.
- The one residual assumption is that the driver's "pending work" detection is complete. That is the ordinary
  steady-state path: measured 0 early in host-window 5,850 frames, fence-probe, every warm mode, and the stress and
  check runs below.

**EGL: the front (`src/front.c`, `eglMakeCurrent`).**
- A state is created only inside `eglMakeCurrent(ctx ≠ NONE)` on a thread without one (§3.5 census). After every
  successful bind of a non-NULL context, before returning, the front creates and destroys one `EGL_SYNC_FENCE` on the
  bound display. If the bind created the state, nothing can have waited on it (the state and its wait slot are new),
  so `V == 0` and this creation starts it. If the state existed, the creation is an ordinary one (the cached fence,
  or one `FENCE_CREATE` after pending work) and changes nothing.
- Every bind, not once per thread: then I does not depend on knowing every call that destroys a state
  (`eglReleaseThread`, thread exit and whatever else): the next state can only be created by a bind, and every bind
  ends with the start. Measured equivalent sequences: `khr-warm-nodup`, `warm-nodup` 0/24 through stock glvnd; the
  ioctl log through the front shows `CTX_CREATE` immediately followed by `FENCE_CREATE wait_value 1` on that thread.
- Failure is loud: the driver's error is logged, the previous binding is restored (or NONE), `eglMakeCurrent`
  returns `EGL_FALSE` with that error. GLES 1 (config context), GLES 3 (no-config) and desktop-GL contexts bind
  through it (measured).
- Re-entrancy: the driver's internal binds (its unbind fence, the platform base's `eglSwitchCurrentSurface` through
  `EGLExtDriver`'s `eglMakeCurrent`) never come back through the front's export; a nested call through it would be a
  plain forward (`t_bind_depth`), the outermost call starts the timeline before returning to the app.
- The platform library: its `eglWaitSync` (`src/window.c`, the dequeue fence) calls the driver directly, inside
  `eglSwapBuffers`, on the thread that made the surface current, i.e. through the front's `eglMakeCurrent`. It
  creates no threads and binds no contexts of its own (`grep MakeCurrent/pthread_create` over `src/` and the platform
  base: only `eplSwitchCurrentSurface`, unused, which rebinds the current context of the calling thread).
- Cost (host, idle GPU, `eglMakeCurrent` medians over 2000 calls, stock glvnd → front): re-bind of the current
  context 0.08 → 1.41 µs (after a glClear 0.10 → 3.3 µs: the start flushes the pending work); switching between two
  contexts 5.8 → 10.7 µs; unbind + bind 6.1 → 10.9 µs; the first bind of a new thread 28.8 → 28.9 ms (the driver's).

**Vulkan: the HAL (`<driver checkout>/hal/src/device.c`, `vkCreateDevice`).** One state per VkQueue, created with the device. Before
the device is returned, every queue of every `VkDeviceQueueCreateInfo` gets one empty submit with an exportable fence
(or semaphore) and its sync_fd export; nothing of the app can have waited yet. Details, measurements, cost and the
loud failures: `<driver checkout>/hal/AHB-CONTRACT.md` §8.

**Proof by test (2026-10-11, host; verdicts by GPU timestamps).**
- `make front-fence-check`: all 23 fence-first-wait sequences through the front on the Android platform display, 200
  rounds each: **0 early** in every one. i386 (host glibc build of the same front.c): `first-wait`, `release-thread`,
  `khr-fence` 0/24 (stock lib32 glvnd 24/24).
- `make driver-check` (stock glvnd, 10 rounds): 10/10 early in `first-wait`, `release-thread`, `main-warm-ctx`,
  `two-waits`, `client-wait-first`, `khr-fence`, `pbuffer-first`; 0 in the other 16 (also 24 rounds: 24/24 and 0).
- `<driver checkout>/hal` `make hal-fence-check`: all 23 vk-first-wait modes through the HAL, 200 rounds each, plus the semaphore-only
  start: **0 early**; the raw ICD (`make driver-check`, 10 rounds): 10/10 early in 8 modes. host32 HAL vs lib32 ICD:
  `hal-seq`, `fence-export`, `mq-warm-g0-use-c0` 0/24 vs 24/24.
- Host stress (README "Tests"), `tools/host-window-stress.sh`, every run first frame + steady state + interval 1→0→1 +
  resize, half in-process, half separate-process consumer: amplified platform gl-write 5000, gl-read 5000, vk-write
  5000, vk-read 300, cpu 300 runs; normal platform 200 runs per type: **0 failed runs**. With the pre-fix front/HAL the
  same harness failed 10, 3, 35, 39 of 200 amplified in-process runs (gl-write, gl-read, vk-write, vk-read).
- An errored imported fd (§2.3): see §7.

## 7. Not established

- Whether `eglPlatformSetColorBuffersNVX` or colour-buffer import emits GPU work that a later eglWaitSync would not
  order. We now use NVIDIA's own order (wait, then SetColorBuffers, §4.1), which avoids the question.
- Two further code paths in the driver set `started` without a release. They are conditional on flags not
  identified. Neither fired in any traced run, and neither can break invariant I, since they only set `started`.
- Multi-queue Vulkan devices: **measured** (2026-10-11): one state per VkQueue (`<driver checkout>/hal/AHB-CONTRACT.md` §8.1).
- Errored (-ETIMEDOUT) fences: **measured** (2026-10-11, scratch tool, 20 rounds through stock glvnd and 20 through
  the front): the fd from a Vulkan submission blocked on an unsignalled timeline value signals after 5.37–5.44 s with
  sync_file status −110; `eglWaitSync` on it returns `EGL_TRUE` (error 0x3000) at once and the GPU proceeds; the
  thread's next native fence has status 1 (the error does not propagate). Stock glvnd: 20/20 early (the wait was the
  thread's first fence operation, D1); front: 0/20 early. So an errored fence is treated as signalled: a consumer
  then touches the buffer while the producer's (hung, > 5 s) work may still run. That is the kernel's timeout
  semantics (§2.3), not D1; nothing in the platform checks a dequeue fence's status before waiting on it (not built).
- Driver-internal threads: with `__GL_THREADED_OPTIMIZATIONS` (off by default) the driver submits from its own
  worker thread; whether that thread uses the app thread's timeline state was not measured.
- The guest: not measured. Everything above is host glibc; the guest runs the same driver binaries through the shims.
