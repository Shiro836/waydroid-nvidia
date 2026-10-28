# egl: NVIDIA's own EGL/GLES as an Android vendor EGL driver (P7)

Two pieces, both built from source, both Android-specific but Waydroid-agnostic:

| Library | Role |
|---|---|
| `libEGL_nvnative.so`, `libGLESv1_CM_nvnative.so`, `libGLESv2_nvnative.so` | The **front**: what Android's EGL loader loads as `lib*_<ro.hardware.egl>.so` (Loader.cpp:600-668). It `dlopen`s `libEGL_nvidia.so.0` by soname from its own (sphal) namespace, calls `__egl_Main(0.2)` with its own `__EGLapiExports` (no libglvnd), exports the 71 loader entries NVIDIA provides (generated from Android's `egl_entries.in`; 6 entries NVIDIA lacks are not exported) and answers every GL name through the vendor's `getProcAddress`. `eglGetDisplay(d)` = `eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, d)`. The GLES libraries are empty on purpose (`src/gles_empty.c`). |
| `libnvnative-egl-android.so` + `nvnative_egl_android.json` | The **platform**: an NVIDIA EGL external platform (eglexternalplatform 1.2 + the driver's platform-surface interface "NVX" 0.2) that claims `EGL_PLATFORM_ANDROID_KHR`. Window configs, ANativeWindow surfaces on the window's own gralloc dma-bufs, fences, buffer age, swap interval, resize, `EGL_ANDROID_image_native_buffer`. Structure from NVIDIA's egl-wayland2 v1.0.2, Android semantics from Mesa's `platform_android.c`; display/surface bookkeeping is NVIDIA's platform base (egl-x11 v1.0.6 `src/base`, Apache-2.0) compiled from the pinned clone. |

Discovery: the driver reads `__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES` with plain `getenv` (measured). The shims answer
that `getenv` from the sidecar `nvglibc-default-env` next to `libnvglibc.so`
(`__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES=${ORIGIN}/nvnative_egl_android.json`), only when the variable is absent
from the real environment; the app's environment is never written (`../shim/DESIGN.md`, `getenv` row). The JSON
names the platform by soname, so it resolves in sphal from `/vendor_extra/lib{,64}`.

Contracts: `build/p7/NV-EGL-CONTRACT.md` (driver side), `build/p7/ANDROID-EGL-CONTRACT.md` (Android 13 side).

## Build

    make            # host (ASan/UBSan) + android (x86_64) + android32 (i686) + check
    make check      # the built Android libraries: ELF class, SONAME, NEEDED ⊆ {libc,libdl,liblog,libm}, no GLIBC_
                    # reference, exact export sets (front 62 forwarded + 9 own EGL names, GLES libs nothing, platform loadEGLExternalPlatform)
    make test       # host tests on the real GPU (below)
    make front-fence-check   # every fence-first-wait sequence through the front, 20 rounds each: must be 0 early
    make driver-check        # the same sequences through STOCK libglvnd: detects NVIDIA defect D1, FAILS on 615.71.09
    make stress [STRESS_PLATFORM=amplify] [STRESS_RUNS=N] [STRESS_CONSUMERS="gl-write ..."]   # host stress, 0 failures
    make guest      # test/eglwindow for x86_64 and x86 (links the guest's own libgui; see "ABI-fragile")

Third-party sources are read from the git object store of pinned clones in `build/p7/src/` (egl-x11 328a5e1b = v1.0.6-5,
eglexternalplatform cf9f1058, libglvnd faa23f21, frameworks/native 2fe7eddf); nothing third-party is checked in.
The gralloc handle layout and its validity check are shared with the Vulkan HAL
(`<driver checkout>/hal/include/minigbm/cros_gralloc_handle.h`, `cros_gralloc_handle_mismatch()`).

Guest layout: `/vendor_extra/lib{,64}/libnvnative-egl-android.so`, `nvnative_egl_android.json`, `nvglibc-default-env`
(staging tree, rbind-mounted at `/vendor_extra`), and `/vendor/lib{,64}/egl/lib{EGL,GLESv1_CM,GLESv2}_nvnative.so`
(the only directories Android's loader searches; the Waydroid integration stages that directory with ANGLE next to it
and `ro.hardware.egl` selects one, `../../waydroid/nvidia_native.py`).

**System driver (G7d, 2026-10-11).** NVIDIA's EGL imports a LINEAR dma-buf external-only
(`eglQueryDmaBufModifiersEXT`: external_only=1; GL_TEXTURE_2D and renderbuffer binds give GL_INVALID_OPERATION, stock
driver on the host). SurfaceFlinger renders screenshots into `SW_READ_OFTEN|SW_WRITE_OFTEN|HW_RENDER|HW_COMPOSER|
HW_TEXTURE` buffers (usage 0xb33, SurfaceFlinger.cpp:6611-6616), which the gralloc backend used to allocate LINEAR,
so SurfaceFlinger aborted ("Unable to generate SkSurface") at its first screenshot. Fixed in gralloc
(`<driver checkout>/gralloc/README.md` "Layout rule"): CPU-mapped GPU render targets are block-linear and CPU locks go through a GPU
staging copy. `test/sfimage.c` (`make guest`) reproduces RenderEngine's sequence through Android's libEGL;
`--usage b33 --cpu-check` adds the gralloc lock contract (native fence as acquire fence, every pixel through the CPU
view, a CPU write seen by `glReadPixels` and a second lock) and the lock access flags (a GL write during a READ lock
survives its unlock, a nested READ→WRITE lock is written back, a WRITE-only rect lock keeps the rest): 0 failures on
nvnative, x86_64 and x86, 2560x1412 and 1001x37 (2026-10-11).

Diagnosis switch: `debug.nvnative.gldebug=1` (property; host `NVNATIVE_GL_DEBUG=1`), read once per process: the front
enables KHR_debug output after every bind and logs the driver's GL_DEBUG_TYPE_ERROR messages ("nvnative-egl"), and the
platform logs every `EGL_ANDROID_image_native_buffer` import (ANWB size/format/usage, fourcc, pitch, modifier). Off by
default (it changes app-visible GL debug state).

## Design facts (each measured; host 615.71.09 unless noted)

- **Client APIs: OpenGL ES only** (2026-10-11, dEQP-EGL `negative_api.bind_api`, `create_context_ext.gl_*`). NVIDIA's EGL
  offers `EGL_OPENGL_API` and sets `EGL_OPENGL_BIT` on all 69 configs; Android has no desktop GL (its loader reports
  `EGL_CLIENT_APIS` "OpenGL_ES", frameworks/native `egl_display.cpp:46`, keeps only GLES dispatch tables and calls
  `glGetString(GL_EXTENSIONS)` at a context's first make-current, `egl_object.cpp:319`, which is GL_INVALID_ENUM in a
  desktop core profile). The front does what Mesa does on Android (`eglcurrent.h` `_eglIsApiValid`, `egl_dri2.c`
  ClientAPIs): `eglBindAPI(EGL_OPENGL_API)` → EGL_BAD_PARAMETER, `EGL_RENDERABLE_TYPE`/`EGL_CONFORMANT` without
  `EGL_OPENGL_BIT`, a config request for it matches nothing (the driver still validates the list), `EGL_CLIENT_APIS`
  "OpenGL_ES". No config is GL-only, so none disappears. Test: `test/front-api.c` (`make test`).
- **Platform base pin** egl-x11 `328a5e1b` (v1.0.6-5, main 2026-08-25) instead of v1.0.6: upstream `ec2d477` loads
  the driver's `eglSetDamageRegionKHR` so a pbuffer gets EGL_BAD_MATCH, and `c83c2e8` passes `eglSwapInterval`
  without a current context to the driver (EGL_BAD_CONTEXT) instead of EGL_BAD_SURFACE; both fix dEQP-EGL cases
  (`negative_partial_update.not_postable_surface`, `negative_api.swap_interval`). Measured, not asserted:
  `eglSwapInterval` with a surfaceless context (or a context of another display) returns the driver's EGL_TRUE
  (EGL 1.5 §3.10.3 says EGL_BAD_SURFACE; no dEQP case covers it).

- **Window configs**: 49 of the device display's 69 configs: stream-capable, NVX fourcc with a HAL format, and a
  non-external render modifier for that fourcc. NVIDIA reports `AR24` for its RGBA8 configs, so
  `EGL_NATIVE_VISUAL_ID = HAL_PIXEL_FORMAT_BGRA_8888 (5)`; the Android loader still sets `RGBA_8888` buffers (`AB24`)
  from the sizes, and the NVX import takes the *buffer's* fourcc: `AB24` window buffers render pixel-exact with an
  `AR24` config (R/B-distinct colours checked). `EGL_RECORDABLE_ANDROID`/`EGL_FRAMEBUFFER_TARGET_ANDROID` are TRUE on
  window configs, answered by the platform (the driver rejects RECORDABLE in eglChooseConfig);
  `EGL_COLOR_COMPONENT_TYPE_EXT` comes from the driver (0x333a).
- **Buffers**: usage `GRALLOC_USAGE_HW_RENDER`, buffer count left to the window (Mesa does the same); each window
  buffer is imported once (`eglPlatformImportColorBufferNVX`, single plane, explicit modifier; minigbm handle decoded),
  cached by `ANativeWindowBuffer*` + handle with a reference held; a buffer of a new size drops the old ones.
  Block-linear `0x0300000000606014` (gralloc's choice for 256x128 and 640x400 window buffers) imports as a render
  target; multi-planar buffers, `DRM_FORMAT_MOD_INVALID` and non-minigbm handles fail with a named error.
- **Fences**: swap = driver's internal `eglSwapBuffers` on the NVX surface (NVX ≥ 0.2) → `eglCreateSync(NATIVE_FENCE)`
  + `glFlush` + `eglDupNativeFenceFDANDROID` → `queueBuffer(fd)` → `dequeueBuffer` → the dequeue fence imported as
  `EGL_SYNC_NATIVE_FENCE_ANDROID` and `eglWaitSync`ed in the rendering context → **then**
  `eglPlatformSetColorBuffersNVX(GL_BACK)`, NVIDIA's own order (egl-wayland2 wayland-surface.c:1556-1570, egl-x11
  x11-window.c:2255-2275; until 2026-10-11 we attached first and waited second). A failure after the dequeue cancels
  the buffer with the consumer's release fence still attached. The first buffer (dequeued in `eglCreateWindowSurface`,
  no context current) waits on the CPU. Destroy cancels the dequeued buffer, with a native fence when the surface is
  current. No glFinish/CPU fallback: a missing `EGL_ANDROID_native_fence_sync` (libdrm.so.2 absent) refuses the
  display with that remedy.
- **Thread timelines (NVIDIA defect D1, FENCE-CONTRACT.md §6a)**: the front's `eglMakeCurrent` creates and destroys
  one `EGL_SYNC_FENCE` after every successful bind of a non-NULL context, before returning. NVIDIA 615.71.09 creates a
  thread's fence timeline inside that call; this first fence creation, while nothing can have waited on it, makes every
  later fence of the thread cover its work. `eglGetProcAddress("eglMakeCurrent")` returns the front's function, never
  the driver's. A failed start fails `eglMakeCurrent` loudly (previous binding restored).
- **Y orientation**: `EGL_SURFACE_Y_INVERTED_NVX = EGL_TRUE`. Measured: with TRUE the window's top quarter lands in the
  buffer's first rows (8192 of 8192 marker pixels, read through an independent dma-buf import); the `EGL_FALSE` build
  puts it in the last rows. On SurfaceFlinger the TRUE build's band is at the window's top (screencap).
- **`EGL_ANDROID_image_native_buffer`**: the platform's `eglCreateImageKHR`/`eglCreateImage` hooks are reached for
  `EGL_NATIVE_BUFFER_ANDROID` (target 0x3140) on the platform's display; translated to `EGL_LINUX_DMA_BUF_EXT` with
  modifiers. Context must be `EGL_NO_CONTEXT` (EGL_BAD_CONTEXT otherwise). LINEAR (CPU) buffers sample as
  `GL_TEXTURE_EXTERNAL_OES`; binding them as `GL_TEXTURE_2D` gives GL_INVALID_OPERATION (the driver imports LINEAR
  external-only); block-linear buffers work as `GL_TEXTURE_2D` render targets.
- `eglSurfaceAttrib(EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED)` on a window surface → EGL_TRUE (HWUI needs it).
- Version checks at load (loud, logcat "nvnative-egl"): platform interface major 1 with driver minor ≥ 2 (615 asks 1.3),
  NVX major 0 minor ≥ 2 (615: 0x2), every driver entry point the platform uses.

## D1: NVIDIA's fence defect, and the invariant that makes our hand-offs correct

`host-window` used to fail rarely on frame 4, the first reuse of the first-rendered buffer: the buffer held the
consumer's earlier poison. **Root cause: NVIDIA 615.71.09 keeps one fence timeline per EGL thread (and per Vulkan
queue); a GPU wait on an imported fence before the timeline's first fence creation makes that first fence signal
together with the imported one, covering none of the work before it** (`FENCE-CONTRACT.md` §3.3, §3.5; bug report
draft `NVIDIA-BUG-D1.md`, not sent). The harness consumer did exactly that in its first frame, as SurfaceFlinger's
RenderEngine does on a fresh thread.

**Fix: enforce invariant I from outside the driver** (§6a has the proof): the first
fence creation on every timeline happens before any wait. The front starts each thread's timeline in `eglMakeCurrent`
(above); the Vulkan HAL starts each queue's timeline in `vkCreateDevice` (`<driver checkout>/hal/AHB-CONTRACT.md` §8). The blob is
untouched; `make driver-check` (here and in `<driver checkout>/hal`) still detects the defect through stock libglvnd / the raw ICD and
fails on 615.71.09 by design.

## Tests

- `test/host-window.c` (host, glibc, ASan/UBSan): fake `ANativeWindow` per `system/window.h` acting as a 3-buffer
  BufferQueue over real dma-bufs from `<driver checkout>/gralloc`'s host build (`cros_gralloc_handle` as minigbm lays it out). The
  producer follows the Android loader's steps and loads the front with dlopen + dlsym + eglGetProcAddress. Checks pixels
  (per-frame colour + top marker band), sync_file acquire fences, buffer age, swap interval 0/1, resize 256x128→192x96,
  cancel on destroy, references returned, image_native_buffer (LINEAR external, block-linear 2D); `--y-measure`,
  `--cycles N`. `--consumer TYPE`:
  - `gl-write` (default): EGL through the front; GPU-waits the acquire fence, reads every pixel through its own
    dma-buf import, overwrites the buffer with a slow "poison" pass, releases with a native fence.
  - `gl-read`: GPU-waits, samples the buffer in a slow pass whose texel fetch happens at its end, releases, and only
    then reads its copy back and checks it (an early release lets the next frame land before the late fetch).
  - `vk-write` / `vk-read`: Vulkan through our HAL (`--vk-hal`), the dma-buf as a DRM-modifier VkImage, the acquire
    fence as a SYNC_FD semaphore wait, queue-family-foreign ownership transfers; write = copy out (checked), slow fills,
    poison clear, release by `vkGetFenceFdKHR`; read = slow fills, then the copy out, release by `vkGetSemaphoreFdKHR`.
  - `cpu`: mmap + `DMA_BUF_IOCTL_SYNC`. **NVIDIA takes no part in dma-buf implicit sync** (`FENCE-CONTRACT.md` §2.5):
    `DMA_BUF_IOCTL_SYNC`, `EXPORT_SYNC_FILE` and poll() on the dma-buf give no ordering against NVIDIA GPU work, so a
    CPU consumer must wait for the acquire fence fd itself (poll) before touching the mapping, and a CPU producer
    must hand over a fence or finish before releasing. The harness's CPU consumer polls the acquire fd, reads every
    pixel (block-linear: the pixel words are located on a buffer's first frame and checked by class afterwards),
    writes poison into them and releases with −1.
  Every type runs in-process (thread) or with `--consumer-process` in an exec'd process receiving fds and fences over
  SCM_RIGHTS, as SurfaceFlinger does, and hands its release fence over before checking what it read.
- `tools/host-window-stress.sh` / `make stress`: N fresh runs per consumer type, in-process and separate-process
  alternating, J in parallel; each run covers the first frame, steady state, swap interval 1 → 0 → 1 and a resize.
  `STRESS_PLATFORM=amplify` uses `out/host-amplify/` (skips the GPU wait on an already-signalled release fence, so an
  early release is never masked). **Pass bar: 0 failed runs**; any failure is a stop-and-report, never a rate.
  The harness is known to detect D1: with the pre-fix front (amplified, in-process, 200 runs) gl-write 10 and gl-read 3
  failed runs; with the pre-fix HAL vk-write 35 and vk-read 39. The cpu consumer creates no fences, so it tests the
  acquire side (H4) and the explicit CPU contract only.
- Results 2026-10-11 (host): amplified gl-write/gl-read/vk-write 5000 runs each, vk-read/cpu 300; normal 200 per type:
  0 failed runs. `front-fence-check` 23 modes × 200 rounds: 0 early.
- **Cost of the thread-timeline start** (idle GPU, `eglMakeCurrent` medians, stock glvnd → front): re-bind 0.08 →
  1.4 µs (3.3 µs with pending work, which the start flushes), context switch 5.8 → 10.7 µs, unbind + bind 6.1 →
  10.9 µs, a thread's first bind 28.8 → 28.9 ms.
- `test/front-api.c` (host): the client-API surface on the Android platform display and an NVIDIA device display
  through the front, and the platform base's error pass-through (above).
- `test/fence-first-wait.c`: every D1 sequence (23 modes), default stock libglvnd (`driver-check`), `--front` through
  ours (`front-fence-check`); verdict by GPU timestamps (GL_EXT_disjoint_timer_query), see its header.
- `test/fence-probe.c`, `test/fence-race.c`: NVIDIA native-fence semantics without any platform code.
- `test/eglwindow.cpp` (guest): a real SurfaceFlinger layer through libgui, the loader's ANativeWindow steps, OUR front
  through `android_load_sphal_library`, animated frames (yellow top band, colour cycling every frame, checkerboard),
  AHB tests. **ABI-fragile**: `test/sf_client.h` hand-declares the few libgui/libbinder/libutils members it calls
  (no std:: types, objects never destroyed) and links against the guest's own `/system/lib{,64}` libraries read from
  `system.img` with debugfs; the mangled names were checked with `llvm-nm -D` in both ABIs. Rebuild and re-check for
  any other Android release.
