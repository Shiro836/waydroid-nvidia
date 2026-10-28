/* host-window: the EGL front + Android platform on the host GPU, against a fake ANativeWindow.
 *
 * Bionic-free harness (glibc, ASan/UBSan): the window is a struct ANativeWindow per system/window.h
 * (include/nvn_android.h) acting as a small BufferQueue. Its buffers are real dma-bufs allocated by our gralloc
 * backend's host build (the driver's gralloc/, the same code minigbm runs in the guest), wrapped in
 * cros_gralloc_handle + ANativeWindowBuffer exactly as minigbm lays them out. A consumer thread plays
 * SurfaceFlinger: it waits for each queued buffer's acquire fence on the GPU, imports the dma-buf through a
 * *separate* path (EGL_LINUX_DMA_BUF_EXT on a device display, not the platform's NVX import), reads pixels back,
 * optionally overwrites the buffer with a slow GPU "poison" pass, and returns it with a release fence that the
 * producer must wait for. The producer follows the Android loader's steps (ANDROID-EGL-CONTRACT §1.5) and loads
 * the front with dlopen + dlsym(name) + eglGetProcAddress(name), as Loader.cpp does.
 *
 * Checks: every frame's pixels (colour per frame + a marker band at the window's TOP, which must land in the
 * buffer's first rows), acquire fences are sync_files, release fences are honoured (poison never visible),
 * EGL_BUFFER_AGE_EXT equals the queue history, swap interval 0/1 reaches the window, resize, cancel on destroy,
 * every buffer/window reference returned, EGL_ANDROID_image_native_buffer (LINEAR CPU-written buffer sampled as
 * GL_TEXTURE_EXTERNAL_OES; block-linear buffer as a GL_TEXTURE_2D render target).
 * --y-measure (with the EGL_SURFACE_Y_INVERTED_NVX=EGL_FALSE platform build) reports where the marker lands
 * instead of asserting it. Exit status = number of failed checks.
 * Diagnostics of README "D1" (they change the consumer, never the platform; FENCE-CONTRACT.md §3.3):
 *   --x-cfinish      the consumer glFinish()es its poison pass before creating the release fence
 *   --x-prewarm      the consumer draws its poison program once at start-up
 *   --x-cwarmfence   the consumer's thread creates one native fence before its first eglWaitSync, which sidesteps
 *                    the driver defect; a diagnostic only, it is not what a real consumer (SurfaceFlinger) does */
#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <hardware/hwvulkan.h>

#include <drm_fourcc.h>
#include <cros_gralloc_handle.h>
#include <nvn_android.h>
#include "gbm_mesa_wrapper.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define DIE(...) do { printf("FATAL: " __VA_ARGS__); printf("\n"); exit(100); } while (0)

/* ---------------- EGL/GL through the front ---------------- */
static void *front;
static __eglMustCastToProperFunctionPointerType (*p_eglGetProcAddress)(const char *);
static void *sym(const char *n) {
    void *p = dlsym(front, n); /* the loader's order: dlsym, then eglGetProcAddress */
    if (!p) p = (void *)p_eglGetProcAddress(n);
    if (!p) DIE("front resolves neither dlsym nor eglGetProcAddress(\"%s\")", n);
    return p;
}
#define EGLFN(X) \
    X(PFNEGLGETPLATFORMDISPLAYPROC, eglGetPlatformDisplay) X(PFNEGLINITIALIZEPROC, eglInitialize) \
    X(PFNEGLTERMINATEPROC, eglTerminate) X(PFNEGLQUERYSTRINGPROC, eglQueryString) \
    X(PFNEGLCHOOSECONFIGPROC, eglChooseConfig) X(PFNEGLGETCONFIGATTRIBPROC, eglGetConfigAttrib) \
    X(PFNEGLGETCONFIGSPROC, eglGetConfigs) \
    X(PFNEGLCREATEWINDOWSURFACEPROC, eglCreateWindowSurface) X(PFNEGLDESTROYSURFACEPROC, eglDestroySurface) \
    X(PFNEGLCREATECONTEXTPROC, eglCreateContext) X(PFNEGLDESTROYCONTEXTPROC, eglDestroyContext) \
    X(PFNEGLMAKECURRENTPROC, eglMakeCurrent) X(PFNEGLSWAPBUFFERSPROC, eglSwapBuffers) \
    X(PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC, eglSwapBuffersWithDamageKHR) X(PFNEGLSWAPINTERVALPROC, eglSwapInterval) \
    X(PFNEGLQUERYSURFACEPROC, eglQuerySurface) X(PFNEGLSURFACEATTRIBPROC, eglSurfaceAttrib) \
    X(PFNEGLSETDAMAGEREGIONKHRPROC, eglSetDamageRegionKHR) X(PFNEGLGETERRORPROC, eglGetError) \
    X(PFNEGLBINDAPIPROC, eglBindAPI) X(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR) \
    X(PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR) X(PFNEGLCREATESYNCPROC, eglCreateSync) \
    X(PFNEGLDESTROYSYNCPROC, eglDestroySync) X(PFNEGLWAITSYNCPROC, eglWaitSync) \
    X(PFNEGLDUPNATIVEFENCEFDANDROIDPROC, eglDupNativeFenceFDANDROID) X(PFNEGLQUERYDEVICESEXTPROC, eglQueryDevicesEXT) \
    X(PFNEGLQUERYDMABUFMODIFIERSEXTPROC, eglQueryDmaBufModifiersEXT) X(PFNEGLRELEASETHREADPROC, eglReleaseThread)
#define GLFN(X) \
    X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLCLEARPROC, glClear) X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLDISABLEPROC, glDisable) X(PFNGLSCISSORPROC, glScissor) X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLREADPIXELSPROC, glReadPixels) X(PFNGLGETERRORPROC, glGetError) X(PFNGLFINISHPROC, glFinish) \
    X(PFNGLFLUSHPROC, glFlush) X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES) X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM4FPROC, glUniform4f) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLACTIVETEXTUREPROC, glActiveTexture)
#define DECL(t, n) static t n;
EGLFN(DECL)
GLFN(DECL)
#define RES(t, n) n = (t)sym(#n);

/* ---------------- gralloc buffers ---------------- */
static struct gbm_ops *gops;
static struct gbm_device *gdev;

/* the consumer's view of one buffer (in-thread: inside struct buf; in --consumer-process: in the child) */
struct cview {
    int fd, w, h;
    uint32_t fourcc, stride;
    uint64_t modifier;
    EGLImageKHR cimg;
    GLuint ctex, cfbo;
    VkImage vimg;           /* vk-* consumers: the dma-buf as a DRM-modifier image */
    VkDeviceMemory vmem;
    void *map;              /* cpu consumer: the dma-buf mapping */
    size_t map_sz;
    uint32_t *pix_off;      /* cpu consumer: word offsets of the w*h pixels in the (block-linear) mapping */
    uint8_t *pix_mark;      /* ... and whether each is in the marker band */
    int npix;
};
static void cview_release(struct cview *v);

struct buf {
    ANativeWindowBuffer anwb; /* first: the platform sees &anwb */
    struct cros_gralloc_handle h;
    int refs;                 /* the window's own reference is 1 */
    int release_fence;        /* returned by the next dequeue */
    int gen;                  /* window size generation */
    int queued_at;            /* 1-based index of the last queue, 0 = never */
    struct cview cv;          /* consumer's dma-buf import (in-thread consumer) */
};
static int live_bufs;
static pthread_mutex_t bl = PTHREAD_MUTEX_INITIALIZER;

static void b_inc(struct android_native_base_t *b) {
    pthread_mutex_lock(&bl);
    ((struct buf *)b)->refs++;
    pthread_mutex_unlock(&bl);
}
static void b_dec(struct android_native_base_t *b) {
    pthread_mutex_lock(&bl);
    struct buf *x = (struct buf *)b;
    int r = --x->refs;
    pthread_mutex_unlock(&bl);
    if (r == 0) {
        cview_release(&x->cv);
        close(x->h.fds[0]);
        if (x->release_fence >= 0) close(x->release_fence);
        free(x);
        __atomic_fetch_sub(&live_bufs, 1, __ATOMIC_RELAXED);
    }
}

static struct buf *buf_alloc(int w, int h, uint32_t fourcc, int hal, bool linear, uint32_t usage) {
    struct alloc_args a = {.gbm = gdev, .width = (uint32_t)w, .height = (uint32_t)h, .drm_format = fourcc,
                           .force_linear = linear, .needs_map_stride = linear};
    int r = gops->alloc(&a);
    if (r != 0 || a.out_fd < 0) DIE("gralloc alloc %dx%d fourcc 0x%x linear %d failed: %d", w, h, fourcc, linear, r);
    struct buf *b = calloc(1, sizeof(*b));
    b->h.base.version = sizeof(native_handle_t);
    b->h.base.numFds = 1;
    b->h.base.numInts = CROS_GRALLOC_HANDLE_NUM_INTS_TOTAL - 1;
    b->h.fds[0] = a.out_fd;
    for (int i = 1; i < DRV_MAX_FDS; i++) b->h.fds[i] = -1;
    b->h.strides[0] = a.out_stride;
    b->h.width = (uint32_t)w;
    b->h.height = (uint32_t)h;
    b->h.format = fourcc;
    b->h.format_modifier = a.out_modifier;
    b->h.magic = CROS_GRALLOC_MAGIC;
    b->h.droid_format = hal;
    b->h.usage = (int32_t)usage;
    b->h.num_planes = 1;
    b->h.pixel_stride = a.out_stride / 4;
    b->anwb.common.magic = (int)NVN_ANDROID_NATIVE_BUFFER_MAGIC;
    b->anwb.common.version = sizeof(ANativeWindowBuffer);
    b->anwb.common.incRef = b_inc;
    b->anwb.common.decRef = b_dec;
    b->anwb.width = w;
    b->anwb.height = h;
    b->anwb.stride = (int)(a.out_stride / 4);
    b->anwb.format = hal;
    b->anwb.layerCount = 1;
    b->anwb.handle = (const native_handle_t *)(void *)&b->h; /* base is the first member */
    b->anwb.usage = usage;
    b->refs = 1;
    b->release_fence = -1;
    __atomic_fetch_add(&live_bufs, 1, __ATOMIC_RELAXED);
    return b;
}

static bool is_sync_file(int fd) {
    char p[64], t[128] = "";
    snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, t, sizeof(t) - 1);
    return n > 0 && strstr(t, "sync_file") != NULL;
}

static int check_dequeue;
static int poison_on; /* tentative; defined with its default below */
static int consumer_process;
struct buf;
static void send_buf(int slot, struct buf *b);
static void queue_to_child(int slot, int qi, int fence);

/* ---------------- the fake window: a BufferQueue of NBUF buffers ---------------- */
#define NBUF 3
struct win {
    struct ANativeWindow w;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int width, height, gen, hal;
    uint32_t fourcc;
    struct buf *slot[NBUF];
    bool free_[NBUF];
    int queue[64], qhead, qtail; /* slots queued for the consumer */
    int queue_fence[64];
    int refs, interval, connected, nqueued, ncancel, last_cancel_fence_ok;
    uint64_t usage;
    int dequeued_slot, nreleased;
    int last_release_signalled, last_release_fd_given;
    bool quit;
};
static struct win W = {.w = {.minSwapInterval = 0, .maxSwapInterval = 1}};

static int slot_of(struct win *w, ANativeWindowBuffer *b) {
    for (int i = 0; i < NBUF; i++) if (&w->slot[i]->anwb == b) return i;
    return -1;
}
static void w_inc(struct android_native_base_t *b) { __atomic_fetch_add(&((struct win *)b)->refs, 1, __ATOMIC_RELAXED); }
static void w_dec(struct android_native_base_t *b) { __atomic_fetch_sub(&((struct win *)b)->refs, 1, __ATOMIC_RELAXED); }
static int w_setSwapInterval(struct ANativeWindow *w, int i) { ((struct win *)w)->interval = i; return 0; }
static int w_query(const struct ANativeWindow *w_, int what, int *v) {
    const struct win *w = (const struct win *)w_;
    switch (what) {
    case NVN_NATIVE_WINDOW_WIDTH: case NVN_NATIVE_WINDOW_DEFAULT_WIDTH: *v = w->width; return 0;
    case NVN_NATIVE_WINDOW_HEIGHT: case NVN_NATIVE_WINDOW_DEFAULT_HEIGHT: *v = w->height; return 0;
    case NVN_NATIVE_WINDOW_FORMAT: *v = w->hal; return 0;
    case NVN_NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS: *v = 1; return 0;
    case NVN_NATIVE_WINDOW_IS_VALID: *v = 1; return 0;
    default: printf("window: query(%d) not implemented\n", what); return -EINVAL;
    }
}
static int w_perform(struct ANativeWindow *w_, int op, ...) {
    struct win *w = (struct win *)w_;
    va_list ap;
    va_start(ap, op);
    int r = 0;
    switch (op) {
    case NVN_NATIVE_WINDOW_SET_USAGE64: w->usage = va_arg(ap, uint64_t); break;
    case NVN_NATIVE_WINDOW_API_CONNECT: w->connected = va_arg(ap, int); break;
    case NVN_NATIVE_WINDOW_API_DISCONNECT: (void)va_arg(ap, int); w->connected = 0; break;
    case NVN_NATIVE_WINDOW_SET_BUFFERS_FORMAT: w->hal = va_arg(ap, int); break;
    case NVN_NATIVE_WINDOW_SET_BUFFERS_DATASPACE: (void)va_arg(ap, int); break;
    default: printf("window: perform(%d) not implemented\n", op); r = -EINVAL;
    }
    va_end(ap);
    return r;
}
static int w_dequeue(struct ANativeWindow *w_, ANativeWindowBuffer **out, int *fence) {
    struct win *w = (struct win *)w_;
    pthread_mutex_lock(&w->m);
    int s = -1;
    for (;;) {
        for (int i = 0; i < NBUF && s < 0; i++) if (w->free_[i]) s = i;
        if (s >= 0) break;
        pthread_cond_wait(&w->cv, &w->m);
    }
    w->free_[s] = false;
    struct buf *b = w->slot[s];
    if (b->gen != w->gen) { /* resized: reallocate this slot at the new size */
        struct buf *nb = buf_alloc(w->width, w->height, w->fourcc, w->hal, false, (uint32_t)w->usage);
        nb->gen = w->gen;
        if (b->release_fence >= 0) { nb->release_fence = b->release_fence; b->release_fence = -1; }
        w->slot[s] = nb;
        b_dec(&b->anwb.common);
        b = nb;
        if (consumer_process) send_buf(s, nb);
    }
    *out = &b->anwb;
    if (b->release_fence >= 0) {
        struct pollfd pf = {.fd = b->release_fence, .events = POLLIN};
        w->last_release_signalled = poll(&pf, 1, 0);
        w->last_release_fd_given = 1;
        if (check_dequeue && w->last_release_signalled == 1 && poison_on) {
            /* the consumer's release fence says its poison pass is done: is it in memory? */
            size_t sz = (size_t)lseek(b->h.fds[0], 0, SEEK_END), np = 0, n = 0;
            struct dma_buf_sync dsy = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
            ioctl(b->h.fds[0], DMA_BUF_IOCTL_SYNC, &dsy);
            const uint8_t *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, b->h.fds[0], 0);
            for (size_t x = 0; m != MAP_FAILED && x + 4 <= sz; x += 4, n++)
                if (m[x] == 255 && m[x + 1] == 0 && m[x + 2] == 255) np++;
            if (m != MAP_FAILED) munmap((void *)m, sz);
            dsy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
            ioctl(b->h.fds[0], DMA_BUF_IOCTL_SYNC, &dsy);
            printf("dequeue: slot %d release fence signalled; poison in memory %zu of %zu px\n", s, np, n);
        }
    } else {
        w->last_release_signalled = -1;
        w->last_release_fd_given = 0;
    }
    *fence = b->release_fence;
    b->release_fence = -1;
    w->dequeued_slot = s;
    pthread_mutex_unlock(&w->m);
    return 0;
}
static int w_queue(struct ANativeWindow *w_, ANativeWindowBuffer *ab, int fence) {
    struct win *w = (struct win *)w_;
    pthread_mutex_lock(&w->m);
    int s = slot_of(w, ab);
    if (s < 0) { pthread_mutex_unlock(&w->m); printf("FAIL: queueBuffer of a foreign buffer\n"); fails++; if (fence >= 0) close(fence); return -EINVAL; }
    w->slot[s]->queued_at = ++w->nqueued;
    if (consumer_process) {
        queue_to_child(s, w->nqueued, fence);
        pthread_mutex_unlock(&w->m);
        return 0;
    }
    w->queue[w->qtail % 64] = s;
    w->queue_fence[w->qtail % 64] = fence;
    w->qtail++;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->m);
    return 0;
}
static int w_cancel(struct ANativeWindow *w_, ANativeWindowBuffer *ab, int fence) {
    struct win *w = (struct win *)w_;
    pthread_mutex_lock(&w->m);
    int s = slot_of(w, ab);
    w->ncancel++;
    w->last_cancel_fence_ok = fence < 0 || is_sync_file(fence);
    if (s >= 0) {
        if (w->slot[s]->release_fence >= 0) close(w->slot[s]->release_fence);
        w->slot[s]->release_fence = fence;
        w->free_[s] = true;
    } else if (fence >= 0) close(fence);
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->m);
    return s >= 0 ? 0 : -EINVAL;
}

/* ---------------- consumer ("SurfaceFlinger", or any other consumer of the window's buffers) ----------------
 *
 * --consumer TYPE picks what the consumer does with each queued buffer (FENCE-CONTRACT.md §6, H4-H6):
 *   gl-write  (default) EGL/GLES through our front: GPU-waits the acquire fence (eglWaitSync), reads every pixel
 *             through its own dma-buf import, then overwrites the buffer with a slow "poison" pass; release = a native
 *             fence after the poison pass. An early release lets the producer render under the late poison.
 *   gl-read   EGL/GLES through our front: GPU-waits the acquire fence, samples the buffer in a slow pass whose texel
 *             fetch depends on the end of a long loop (so it reads late), release = a native fence after it; the
 *             sampled copy is read back and checked after the release has been handed over. An early release lets
 *             the producer's next frame land before the late fetch.
 *   vk-write  Vulkan through our HAL (--vk-hal): imports the dma-buf as a DRM-modifier VkImage; the acquire fence as a
 *             SYNC_FD semaphore wait; copies the image out (checked), slow fills, clears the image to poison; release
 *             = vkGetFenceFdKHR of the submission's fence.
 *   vk-read   as vk-write, but slow fills first, then the copy out (late read), no poison; release =
 *             vkGetSemaphoreFdKHR of a semaphore the submission signals.
 *   cpu       mmap of the dma-buf + DMA_BUF_IOCTL_SYNC. NVIDIA takes no part in dma-buf implicit sync
 *             (FENCE-CONTRACT.md §2.5: DMA_BUF_IOCTL_SYNC, EXPORT_SYNC_FILE and poll() on the dma-buf give no ordering
 *             against NVIDIA GPU work), so the CPU consumer waits for the acquire fence fd itself (poll) before it
 *             touches the mapping; DMA_BUF_IOCTL_SYNC is issued for the cache-maintenance contract only. It reads
 *             every pixel (block-linear layout: the pixel words are located on the buffer's first frame and checked
 *             by class afterwards), writes poison into them, and releases with -1 (its accesses are complete).
 * Every type runs in this process (thread) or, with --consumer-process, in an exec'd process that receives buffers and
 * fences over SCM_RIGHTS, as SurfaceFlinger does. Each hands its release fence over BEFORE it checks what it read, so
 * an early release fence is caught by the content check rather than hidden by it. */
enum ctype { C_GL_WRITE, C_GL_READ, C_VK_WRITE, C_VK_READ, C_CPU };
static const char *const CTYPE_NAMES[] = {"gl-write", "gl-read", "vk-write", "vk-read", "cpu"};
static enum ctype ctype = C_GL_WRITE;
static const char *vk_hal_path;
static int vk_fills = 24;
static int is_gl(void) { return ctype == C_GL_WRITE || ctype == C_GL_READ; }
static int is_vk(void) { return ctype == C_VK_WRITE || ctype == C_VK_READ; }
static int writes(void) { return ctype == C_GL_WRITE || ctype == C_VK_WRITE || ctype == C_CPU; }

static EGLDisplay cdpy;
static EGLContext cctx;
static int frames_checked, poison_on = 1, y_measure, wrong_pixels;
static uint8_t expect_rgba[64][4]; /* per queue index modulo 64 */
static int expect_w[64], expect_h[64];
static GLuint poison_prog, sample_prog, sample_rb, sample_fb;
static GLint sample_zero_loc;
static int consumer_ready, warm_consumer, draw_mode, probe_f4, poison_from = 1;
static int x_cfinish, x_prewarm, x_cwarmfence;

static GLuint shader(GLenum t, const char *s) {
    GLuint x = glCreateShader(t);
    glShaderSource(x, 1, &s, NULL);
    glCompileShader(x);
    GLint ok = 0;
    glGetShaderiv(x, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024] = ""; glGetShaderInfoLog(x, sizeof(l), NULL, l); DIE("shader: %s", l); }
    return x;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) DIE("link failed");
    return p;
}
static const char *VS_FULL = "#version 300 es\nout vec2 uv;\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
                             "uv=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
/* a deliberately slow full-screen pass (~ms of ALU per pixel row) so a producer that ignored the release fence
 * would race it */
static const char *FS_POISON = "#version 300 es\nprecision highp float;\nin vec2 uv;\nout vec4 o;\nvoid main(){float a=uv.x;"
                               "for(int i=0;i<4000;i++){a=fract(sin(a*12.9898+float(i))*43758.5453);}"
                               "o=vec4(1.0,0.0,1.0,1.0)+vec4(a*1e-9);}\n";
/* gl-read: the same slow loop, and the texel fetch's coordinate depends on its result (times a uniform 0, which the
 * compiler cannot fold), so every fetch happens at the end of the pass */
static const char *FS_SAMPLE = "#version 300 es\nprecision highp float;\nuniform highp sampler2D t;\nuniform float zero;\nin vec2 uv;\n"
                               "out vec4 o;\nvoid main(){float a=uv.x;"
                               "for(int i=0;i<4000;i++){a=fract(sin(a*12.9898+float(i))*43758.5453);}"
                               "o=texelFetch(t, ivec2(gl_FragCoord.xy) + ivec2(int(a*zero)), 0);}\n";

static const uint8_t MARK[4] = {255, 255, 0, 255};   /* yellow band: the window's top quarter */
static const uint8_t POISON[4] = {255, 0, 255, 255}; /* magenta: what a writing consumer leaves behind */

/* Checks a w x h RGBA8 image whose row 0 is the buffer's first row (glReadPixels of the dma-buf import, the Vulkan copy
 * out): the window's top quarter must be the marker, the rest this frame's colour. Returns the wrong-pixel count. */
static int check_pixels(const uint8_t *px, int w, int h, int qi, const uint8_t e[4], int ew, int eh) {
    CHECK(w == ew && h == eh, "frame %d: buffer %dx%d, frame drawn at %dx%d", qi, w, h, ew, eh);
    int bad = 0, top_mark = 0, bottom_mark = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = px + 4 * ((size_t)y * w + x);
            bool in_top = y < h / 4, in_bottom = y >= h - h / 4;
            if (in_top && !memcmp(p, MARK, 4)) top_mark++;
            if (in_bottom && !memcmp(p, MARK, 4)) bottom_mark++;
            const uint8_t *want = (y_measure ? false : in_top) ? MARK : e;
            if (!y_measure && memcmp(p, want, 4) && bad++ < 3)
                printf("frame %d (%s): pixel (%d,%d) = %u,%u,%u,%u expected %u,%u,%u,%u\n", qi, CTYPE_NAMES[ctype], x, y,
                       p[0], p[1], p[2], p[3], want[0], want[1], want[2], want[3]);
        }
    if (qi == 1 || y_measure)
        printf("frame %d (%dx%d, %s consumer): memory row 0 = %u,%u,%u,%u; marker pixels in first quarter of rows %d, in "
               "last quarter %d (window top drawn with the marker)\n", qi, w, h, CTYPE_NAMES[ctype], px[0], px[1], px[2],
               px[3], top_mark, bottom_mark);
    return bad;
}

typedef void (*publish_fn)(void *ctx, int release_fd);

static void import_consumer(struct cview *v) {
    if (v->cimg) return;
    EGLint a[] = {EGL_WIDTH, v->w, EGL_HEIGHT, v->h, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)v->fourcc,
                  EGL_DMA_BUF_PLANE0_FD_EXT, v->fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                  EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)v->stride,
                  EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(v->modifier & 0xffffffff),
                  EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(v->modifier >> 32), EGL_NONE};
    v->cimg = eglCreateImageKHR(cdpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    if (!v->cimg) DIE("consumer dma-buf import failed 0x%x", eglGetError());
    glGenTextures(1, &v->ctex);
    glBindTexture(GL_TEXTURE_2D, v->ctex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, v->cimg);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &v->cfbo);
    glBindFramebuffer(GL_FRAMEBUFFER, v->cfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->ctex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) DIE("consumer FBO incomplete 0x%x", st);
}

static int gl_native_fence(void) {
    EGLSync rs = eglCreateSync(cdpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    glFlush();
    int rfd = eglDupNativeFenceFDANDROID(cdpy, rs);
    eglDestroySync(cdpy, rs);
    CHECK(rfd >= 0, "consumer: release fence dup failed");
    return rfd;
}

static void gl_wait_acquire(int qi, int fence) {
    if (fence < 0) return;
    /* GPU wait, like SurfaceFlinger's RenderEngine with EGL_KHR_wait_sync */
    EGLAttrib fa[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fence, EGL_NONE};
    EGLSync sy = eglCreateSync(cdpy, EGL_SYNC_NATIVE_FENCE_ANDROID, fa);
    if (!sy) DIE("consumer: import acquire fence of frame %d failed 0x%x", qi, eglGetError());
    eglWaitSync(cdpy, sy, 0);
    eglDestroySync(cdpy, sy);
}

/* gl-write: wait, read through the import, check, poison, release */
static void gl_write_consume(struct cview *v, int qi, int fence, const uint8_t e[4], int ew, int eh, publish_fn publish,
                             void *pctx) {
    gl_wait_acquire(qi, fence);
    import_consumer(v);
    glBindFramebuffer(GL_FRAMEBUFFER, v->cfbo);
    int w = v->w, h = v->h;
    uint8_t *px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int bad = check_pixels(px, w, h, qi, e, ew, eh);
    wrong_pixels += bad;
    if (bad) { /* diagnosis: a late read (becomes right) or an overwrite (stays wrong); and the memory itself */
        uint8_t first[4];
        memcpy(first, px, 4);
        glFinish();
        usleep(100000);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        printf("frame %d: first read row0 %u,%u,%u,%u; after glFinish + 100 ms row0 %u,%u,%u,%u\n", qi, first[0], first[1],
               first[2], first[3], px[0], px[1], px[2], px[3]);
        size_t sz = (size_t)lseek(v->fd, 0, SEEK_END);
        struct dma_buf_sync sy = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        ioctl(v->fd, DMA_BUF_IOCTL_SYNC, &sy);
        const uint8_t *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, v->fd, 0);
        if (m != MAP_FAILED) {
            size_t npois = 0, nf = 0, nyel = 0, nother = 0;
            for (size_t o = 0; o + 4 <= sz; o += 4) {
                if (m[o] == 255 && m[o + 1] == 0 && m[o + 2] == 255) npois++;
                else if (m[o] == e[0] && m[o + 1] == e[1] && m[o + 2] == e[2]) nf++;
                else if (m[o] == 255 && m[o + 1] == 255 && m[o + 2] == 0) nyel++;
                else nother++;
            }
            printf("frame %d: dma-buf memory (%zu B mmap): poison %zu, this frame's colour %zu, yellow %zu, other %zu\n", qi,
                   sz, npois, nf, nyel, nother);
            munmap((void *)m, sz);
        }
        sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        ioctl(v->fd, DMA_BUF_IOCTL_SYNC, &sy);
    }
    CHECK(bad == 0, "frame %d: %d wrong pixels", qi, bad);
    free(px);
    frames_checked++;
    if (poison_on && qi >= poison_from) { /* overwrite the whole buffer slowly on the GPU; the producer must wait for this */
        glViewport(0, 0, w, h);
        glUseProgram(poison_prog);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (x_cfinish) glFinish();
    }
    publish(pctx, gl_native_fence());
}

/* gl-read: wait, slow late-sampling pass into a renderbuffer, release, THEN read the copy back and check it */
static void gl_read_consume(struct cview *v, int qi, int fence, const uint8_t e[4], int ew, int eh, publish_fn publish,
                            void *pctx) {
    gl_wait_acquire(qi, fence);
    import_consumer(v);
    int w = v->w, h = v->h;
    static int srw = 256, srh = 128;
    if (w != srw || h != srh) {
        glBindRenderbuffer(GL_RENDERBUFFER, sample_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        srw = w;
        srh = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sample_fb);
    glViewport(0, 0, w, h);
    glUseProgram(sample_prog);
    glUniform1f(sample_zero_loc, 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v->ctex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    publish(pctx, gl_native_fence()); /* the release covers the sampling pass */
    uint8_t *px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int bad = check_pixels(px, w, h, qi, e, ew, eh);
    wrong_pixels += bad;
    CHECK(bad == 0, "frame %d: %d wrong pixels (sampled)", qi, bad);
    free(px);
    frames_checked++;
}

/* ---------------- Vulkan consumer, through our HAL (vulkan.nvidia.so) ---------------- */
#define VKDEV(X) X(GetDeviceQueue) X(DestroyDevice) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) \
    X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(GetMemoryFdPropertiesKHR) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) \
    X(CmdFillBuffer) X(CmdCopyImageToBuffer) X(CmdClearColorImage) X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) \
    X(CreateSemaphore) X(DestroySemaphore) X(ImportSemaphoreFdKHR) X(GetSemaphoreFdKHR) X(CreateFence) \
    X(DestroyFence) X(GetFenceFdKHR)
static struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t fam;
    VkPhysicalDeviceMemoryProperties mp;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkBuffer scratch, host;
    VkDeviceMemory scratch_mem, host_mem;
    uint8_t *host_map;
    PFN_vkDestroyInstance DestroyInstance;
#define VKF(n) PFN_vk##n n;
    VKDEV(VKF)
#undef VKF
} V;
#define VKMUST(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) DIE("vk consumer: %s -> %d", #x, r_); } while (0)

static uint32_t vk_memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < V.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (V.mp.memoryTypes[i].propertyFlags & want) == want) return i;
    DIE("vk consumer: no memory type in 0x%x with flags 0x%x", bits, want);
}
static void vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, VkBuffer *b,
                      VkDeviceMemory *m) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VKMUST(V.CreateBuffer(V.dev, &bci, NULL, b));
    VkMemoryRequirements mr;
    V.GetBufferMemoryRequirements(V.dev, *b, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memtype(mr.memoryTypeBits, flags)};
    VKMUST(V.AllocateMemory(V.dev, &mai, NULL, m));
    VKMUST(V.BindBufferMemory(V.dev, *b, *m, 0));
}

/* The consumer's own VkInstance/VkDevice through the HAL, the way an Android Vulkan compositor would open it. Its
 * device enables VK_KHR_external_semaphore_fd / _fence_fd, so the HAL starts its queue timeline at vkCreateDevice. */
static void vk_init(void) {
    if (!vk_hal_path) DIE("--consumer vk-*: --vk-hal PATH (the host build of vulkan.nvidia.so) is required");
    void *so = dlopen(vk_hal_path, RTLD_NOW | RTLD_LOCAL);
    if (!so) DIE("dlopen %s: %s", vk_hal_path, dlerror());
    hwvulkan_module_t *hmi = dlsym(so, HAL_MODULE_INFO_SYM_AS_STR);
    if (!hmi || strcmp(hmi->common.id, HWVULKAN_HARDWARE_MODULE_ID)) DIE("%s: no hwvulkan HMI", vk_hal_path);
    hmi->common.dso = so;
    hwvulkan_device_t *hd = NULL;
    if (hmi->common.methods->open(&hmi->common, HWVULKAN_DEVICE_0, (hw_device_t **)&hd) || !hd) DIE("HAL open");
    PFN_vkGetInstanceProcAddr gipa = hd->GetInstanceProcAddr;
    PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "host-window consumer",
                            .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VKMUST(CreateInstance(&ici, NULL, &V.inst));
    V.DestroyInstance = (PFN_vkDestroyInstance)gipa(V.inst, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices EnumPD = (PFN_vkEnumeratePhysicalDevices)gipa(V.inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties GetProps = (PFN_vkGetPhysicalDeviceProperties)gipa(V.inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetQF =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(V.inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties GetMP =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(V.inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(V.inst, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(V.inst, "vkGetDeviceProcAddr");
    uint32_t n = 8;
    VkPhysicalDevice pds[8];
    VKMUST(EnumPD(V.inst, &n, pds));
    for (uint32_t i = 0; i < n && !V.pd; i++) {
        VkPhysicalDeviceProperties p;
        GetProps(pds[i], &p);
        if (p.vendorID == 0x10de) V.pd = pds[i]; /* by vendor ID, never by node path */
    }
    if (!V.pd) DIE("vk consumer: no NVIDIA (0x10de) physical device");
    VkQueueFamilyProperties qf[16];
    uint32_t nq = 16;
    GetQF(V.pd, &nq, qf);
    V.fam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && V.fam == UINT32_MAX; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) V.fam = i;
    GetMP(V.pd, &V.mp);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.fam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    const char *exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier",
                          "VK_KHR_image_format_list", "VK_KHR_external_semaphore_fd", "VK_KHR_external_fence_fd",
                          "VK_EXT_queue_family_foreign"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = sizeof exts / sizeof *exts,
                              .ppEnabledExtensionNames = exts};
    VKMUST(CreateDevice(V.pd, &dci, NULL, &V.dev));
#define VKF(n) if (!(V.n = (PFN_vk##n)gdpa(V.dev, "vk" #n))) DIE("vk consumer: vk" #n);
    VKDEV(VKF)
#undef VKF
    V.GetDeviceQueue(V.dev, V.fam, 0, &V.q);
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.fam};
    VKMUST(V.CreateCommandPool(V.dev, &cpci, NULL, &V.pool));
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = V.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKMUST(V.AllocateCommandBuffers(V.dev, &cbai, &V.cb));
    vk_buffer(64u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &V.scratch, &V.scratch_mem);
    vk_buffer(512 * 512 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &V.host, &V.host_mem);
    VKMUST(V.MapMemory(V.dev, V.host_mem, 0, VK_WHOLE_SIZE, 0, (void **)&V.host_map));
}
static void vk_fini(void) {
    if (!V.dev) return;
    V.DeviceWaitIdle(V.dev);
    V.DestroyCommandPool(V.dev, V.pool, NULL);
    V.DestroyBuffer(V.dev, V.scratch, NULL);
    V.DestroyBuffer(V.dev, V.host, NULL);
    V.FreeMemory(V.dev, V.scratch_mem, NULL);
    V.FreeMemory(V.dev, V.host_mem, NULL);
    V.DestroyDevice(V.dev, NULL);
    V.DestroyInstance(V.inst, NULL);
    V.dev = VK_NULL_HANDLE;
}

/* the window buffer as a VkImage: dma-buf import with its explicit DRM format modifier (one plane) */
static void vk_import(struct cview *v) {
    if (v->vimg) return;
    if (v->fourcc != DRM_FORMAT_ABGR8888) DIE("vk consumer: fourcc 0x%x not handled", v->fourcc);
    VkSubresourceLayout pl = {.offset = 0, .size = 0, .rowPitch = v->stride};
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, .drmFormatModifier = v->modifier,
        .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &pl};
    VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &mod,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {(uint32_t)v->w, (uint32_t)v->h, 1},
                             .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VKMUST(V.CreateImage(V.dev, &ici, NULL, &v->vimg));
    int fd = fcntl(v->fd, F_DUPFD_CLOEXEC, 0);
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VKMUST(V.GetMemoryFdPropertiesKHR(V.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp));
    VkMemoryRequirements mr;
    V.GetImageMemoryRequirements(V.dev, v->vimg, &mr);
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = v->vimg};
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memtype(fp.memoryTypeBits & mr.memoryTypeBits, 0)};
    VKMUST(V.AllocateMemory(V.dev, &mai, NULL, &v->vmem)); /* the import owns fd from here */
    VKMUST(V.BindImageMemory(V.dev, v->vimg, v->vmem, 0));
}

static void vk_image_barrier(VkImage img, VkAccessFlags sa, VkAccessFlags da, uint32_t sq, uint32_t dq,
                             VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da,
                              .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = sq, .dstQueueFamilyIndex = dq, .image = img,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    V.CmdPipelineBarrier(V.cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}
static void vk_exec_barrier(void) { /* the next transfer starts after the previous ones completed */
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    V.CmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void vk_consume(struct cview *v, int qi, int fence, const uint8_t e[4], int ew, int eh, publish_fn publish,
                       void *pctx) {
    vk_import(v);
    int w = v->w, h = v->h;
    VkSemaphore acq = VK_NULL_HANDLE, rel = VK_NULL_HANDLE;
    VkFence relf = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (fence >= 0) { /* the acquire fence as a GPU wait (what our HAL's vkAcquireImageANDROID does) */
        VKMUST(V.CreateSemaphore(V.dev, &sci, NULL, &acq));
        VkImportSemaphoreFdInfoKHR ii = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = acq,
                                         .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fence};
        VKMUST(V.ImportSemaphoreFdKHR(V.dev, &ii));
    }
    if (ctype == C_VK_READ) {
        VkExportSemaphoreCreateInfo es = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                          .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkSemaphoreCreateInfo sx = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &es};
        VKMUST(V.CreateSemaphore(V.dev, &sx, NULL, &rel));
    } else {
        VkExportFenceCreateInfo ef = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
                                      .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &ef};
        VKMUST(V.CreateFence(V.dev, &fci, NULL, &relf));
    }
    VKMUST(V.ResetCommandBuffer(V.cb, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKMUST(V.BeginCommandBuffer(V.cb, &bi));
    /* ownership from the foreign producer (VK_EXT_queue_family_foreign), contents kept (GENERAL, not UNDEFINED) */
    vk_image_barrier(v->vimg, 0, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_FOREIGN_EXT,
                     V.fam, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy reg = {.bufferRowLength = (uint32_t)w, .bufferImageHeight = (uint32_t)h,
                             .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
    if (ctype == C_VK_READ) { /* slow work first, then the copy out: a late read */
        for (int i = 0; i < vk_fills; i++) V.CmdFillBuffer(V.cb, V.scratch, 0, VK_WHOLE_SIZE, (uint32_t)i);
        vk_exec_barrier();
        V.CmdCopyImageToBuffer(V.cb, v->vimg, VK_IMAGE_LAYOUT_GENERAL, V.host, 1, &reg);
    } else { /* copy out (checked), slow work, then the poison clear: a late write */
        V.CmdCopyImageToBuffer(V.cb, v->vimg, VK_IMAGE_LAYOUT_GENERAL, V.host, 1, &reg);
        if (poison_on && qi >= poison_from) {
            for (int i = 0; i < vk_fills; i++) V.CmdFillBuffer(V.cb, V.scratch, 0, VK_WHOLE_SIZE, (uint32_t)i);
            vk_exec_barrier();
            VkClearColorValue cc = {.float32 = {1, 0, 1, 1}};
            VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            V.CmdClearColorImage(V.cb, v->vimg, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rr);
        }
    }
    VkBufferMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = V.host, .size = VK_WHOLE_SIZE};
    V.CmdPipelineBarrier(V.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
    /* ownership back to the foreign producer */
    vk_image_barrier(v->vimg, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, 0, V.fam,
                     VK_QUEUE_FAMILY_FOREIGN_EXT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VKMUST(V.EndCommandBuffer(V.cb));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = acq ? 1 : 0, .pWaitSemaphores = &acq,
                       .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &V.cb,
                       .signalSemaphoreCount = rel ? 1 : 0, .pSignalSemaphores = &rel};
    VKMUST(V.QueueSubmit(V.q, 1, &si, relf));
    int rfd = -1;
    if (rel) {
        VkSemaphoreGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = rel,
                                      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VKMUST(V.GetSemaphoreFdKHR(V.dev, &gi, &rfd));
    } else {
        VkFenceGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = relf,
                                  .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VKMUST(V.GetFenceFdKHR(V.dev, &gi, &rfd));
    }
    CHECK(rfd >= 0 && is_sync_file(rfd), "frame %d: vk consumer release fence %d is not a sync_file", qi, rfd);
    publish(pctx, rfd);
    VKMUST(V.QueueWaitIdle(V.q)); /* our own completion, independent of the exported fence */
    int bad = check_pixels(V.host_map, w, h, qi, e, ew, eh);
    wrong_pixels += bad;
    CHECK(bad == 0, "frame %d: %d wrong pixels (Vulkan copy)", qi, bad);
    frames_checked++;
    if (acq) V.DestroySemaphore(V.dev, acq, NULL);
    if (rel) V.DestroySemaphore(V.dev, rel, NULL);
    if (relf) V.DestroyFence(V.dev, relf, NULL);
}

/* ---------------- CPU consumer ---------------- */
static void cpu_consume(struct cview *v, int qi, int fence, const uint8_t e[4], int ew, int eh, publish_fn publish,
                        void *pctx) {
    if (fence >= 0) { /* explicit: NVIDIA does not take part in dma-buf implicit sync (FENCE-CONTRACT.md §2.5) */
        CHECK(is_sync_file(fence), "frame %d: acquire fence %d is not a sync_file", qi, fence);
        struct pollfd p = {.fd = fence, .events = POLLIN};
        while (poll(&p, 1, -1) < 0 && errno == EINTR) {}
        close(fence);
    }
    if (!v->map) {
        v->map_sz = (size_t)lseek(v->fd, 0, SEEK_END);
        v->map = mmap(NULL, v->map_sz, PROT_READ | PROT_WRITE, MAP_SHARED, v->fd, 0);
        if (v->map == MAP_FAILED) DIE("cpu consumer: mmap of the dma-buf (%zu B) failed: %s", v->map_sz, strerror(errno));
    }
    struct dma_buf_sync sy = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW};
    ioctl(v->fd, DMA_BUF_IOCTL_SYNC, &sy);
    CHECK(v->w == ew && v->h == eh, "frame %d: buffer %dx%d, frame drawn at %dx%d", qi, v->w, v->h, ew, eh);
    uint32_t *m = (uint32_t *)v->map;
    uint32_t want_f, want_m, poison;
    memcpy(&want_f, e, 4);
    memcpy(&want_m, MARK, 4);
    memcpy(&poison, POISON, 4);
    int npx = v->w * v->h, nmark = v->w * (v->h / 4), bad = 0;
    if (!v->pix_off) { /* first frame on this buffer: locate the pixel words (block-linear layout) and their class */
        size_t nw = v->map_sz / 4, nf = 0, nm = 0;
        v->pix_off = malloc((size_t)npx * sizeof *v->pix_off);
        v->pix_mark = malloc((size_t)npx);
        for (size_t o = 0; o < nw; o++) {
            int isf = m[o] == want_f, ism = m[o] == want_m;
            if (!isf && !ism) continue;
            if ((int)(nf + nm) < npx) {
                v->pix_off[nf + nm] = (uint32_t)o;
                v->pix_mark[nf + nm] = (uint8_t)ism;
            }
            nf += isf;
            nm += ism;
        }
        v->npix = (int)(nf + nm) <= npx ? (int)(nf + nm) : npx;
        if ((int)nf != npx - nmark || (int)nm != nmark) {
            bad = npx;
            printf("frame %d (cpu): first look at this buffer: %zu words of this frame's colour (want %d), %zu marker "
                   "(want %d)\n", qi, nf, npx - nmark, nm, nmark);
            v->npix = 0;
        }
    } else {
        for (int i = 0; i < v->npix; i++) {
            uint32_t want = v->pix_mark[i] ? want_m : want_f;
            if (m[v->pix_off[i]] != want && bad++ < 3) {
                const uint8_t *p = (const uint8_t *)&m[v->pix_off[i]];
                printf("frame %d (cpu): pixel word %u = %u,%u,%u,%u expected %s\n", qi, v->pix_off[i], p[0], p[1], p[2],
                       p[3], v->pix_mark[i] ? "marker" : "frame colour");
            }
        }
    }
    if (qi == 1) printf("frame 1 (%dx%d, cpu consumer): %d pixel words located in a %zu B mapping\n", v->w, v->h, v->npix,
                        v->map_sz);
    wrong_pixels += bad;
    CHECK(bad == 0, "frame %d: %d wrong pixels (CPU read)", qi, bad);
    frames_checked++;
    if (poison_on && qi >= poison_from)
        for (int i = 0; i < v->npix; i++) m[v->pix_off[i]] = poison;
    __sync_synchronize(); /* the poison stores reach memory before the release is published */
    sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    ioctl(v->fd, DMA_BUF_IOCTL_SYNC, &sy);
    publish(pctx, -1); /* every access is complete: "may access the buffer immediately" (window.h:500-505) */
}

/* One queued frame. Takes ownership of `fence`; hands exactly one release fence (or -1) to publish(). */
static void consume_frame(struct cview *v, int qi, int fence, const uint8_t e[4], int ew, int eh, publish_fn publish,
                          void *pctx) {
    if (ctype != C_CPU) CHECK(fence >= 0 && is_sync_file(fence), "frame %d: acquire fence %d is not a sync_file", qi, fence);
    switch (ctype) {
    case C_GL_WRITE: gl_write_consume(v, qi, fence, e, ew, eh, publish, pctx); break;
    case C_GL_READ: gl_read_consume(v, qi, fence, e, ew, eh, publish, pctx); break;
    case C_VK_WRITE:
    case C_VK_READ: vk_consume(v, qi, fence, e, ew, eh, publish, pctx); break;
    case C_CPU: cpu_consume(v, qi, fence, e, ew, eh, publish, pctx); break;
    }
}

/* a view's per-API resources (the buffer itself is the caller's) */
static void cview_release(struct cview *v) {
    if (v->vimg) { V.DestroyImage(V.dev, v->vimg, NULL); v->vimg = VK_NULL_HANDLE; }
    if (v->vmem) { V.FreeMemory(V.dev, v->vmem, NULL); v->vmem = VK_NULL_HANDLE; }
    if (v->map) { munmap(v->map, v->map_sz); v->map = NULL; }
    free(v->pix_off);
    free(v->pix_mark);
    v->pix_off = NULL;
    v->pix_mark = NULL;
}

static void consumer_gl_init(void) {
    if (!eglMakeCurrent(cdpy, EGL_NO_SURFACE, EGL_NO_SURFACE, cctx)) DIE("consumer makecurrent 0x%x", eglGetError());
    poison_prog = program(VS_FULL, FS_POISON);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    if (ctype == C_GL_READ) {
        sample_prog = program(VS_FULL, FS_SAMPLE);
        glUseProgram(sample_prog);
        glUniform1i(glGetUniformLocation(sample_prog, "t"), 0);
        sample_zero_loc = glGetUniformLocation(sample_prog, "zero");
        glGenRenderbuffers(1, &sample_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, sample_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 256, 128);
        glGenFramebuffers(1, &sample_fb);
        glBindFramebuffer(GL_FRAMEBUFFER, sample_fb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sample_rb);
    }
    if (x_cwarmfence) { /* diagnostic: one native fence in this thread before its first eglWaitSync */
        EGLSync ws = eglCreateSync(cdpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        glFlush();
        close(eglDupNativeFenceFDANDROID(cdpy, ws));
        eglDestroySync(cdpy, ws);
    }
    if (x_prewarm) { /* draw the poison program once into a scratch target and finish */
        GLuint rb, fb;
        glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
        glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        glViewport(0, 0, 64, 64); glUseProgram(poison_prog); glDrawArrays(GL_TRIANGLES, 0, 3); glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}
static void consumer_init(void) {
    if (is_gl()) consumer_gl_init();
    else if (is_vk()) vk_init();
}
static void consumer_fini(void) {
    if (is_gl()) eglMakeCurrent(cdpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static void publish_inthread(void *ctx, int rfd) {
    struct buf *b = ctx;
    pthread_mutex_lock(&W.m);
    if (b->release_fence >= 0) close(b->release_fence);
    b->release_fence = rfd;
    int cs = slot_of(&W, &b->anwb);
    if (cs >= 0) W.free_[cs] = true;
    W.nreleased++;
    pthread_cond_broadcast(&W.cv);
    pthread_mutex_unlock(&W.m);
}

static void *consumer(void *arg) { /* in-thread consumer: same process, so both sides import the same dma-bufs */
    (void)arg;
    consumer_init();
    pthread_mutex_lock(&W.m);
    consumer_ready = 1;
    pthread_cond_broadcast(&W.cv);
    pthread_mutex_unlock(&W.m);
    for (;;) {
        pthread_mutex_lock(&W.m);
        while (W.qhead == W.qtail && !W.quit) pthread_cond_wait(&W.cv, &W.m);
        if (W.qhead == W.qtail && W.quit) { pthread_mutex_unlock(&W.m); break; }
        int s = W.queue[W.qhead % 64], fence = W.queue_fence[W.qhead % 64], qi = W.qhead + 1;
        W.qhead++;
        struct buf *b = W.slot[s];
        b_inc(&b->anwb.common);
        pthread_mutex_unlock(&W.m);
        if (b->cv.w == 0) {
            b->cv.fd = b->h.fds[0]; b->cv.w = b->anwb.width; b->cv.h = b->anwb.height;
            b->cv.fourcc = b->h.format; b->cv.stride = b->h.strides[0]; b->cv.modifier = b->h.format_modifier;
        }
        consume_frame(&b->cv, qi, fence, expect_rgba[(qi - 1) % 64], expect_w[(qi - 1) % 64], expect_h[(qi - 1) % 64],
                      publish_inthread, b);
        b_dec(&b->anwb.common);
    }
    consumer_fini();
    return NULL;
}

/* ---------------- --consumer-process: the compositor in its own process (as SurfaceFlinger is) ---------------- */
enum { MSG_BUF = 1, MSG_QUEUE, MSG_QUIT, MSG_RELEASE, MSG_RESULT };
struct msg {
    int type, slot, qi, w, h, ew, eh, fails, frames_checked, wrong;
    uint32_t fourcc, stride;
    uint64_t modifier;
    uint8_t exp[4];
};
static int csock = -1; /* parent's end */

static void send_msg(int sock, const struct msg *m, int fd) {
    struct iovec io = {.iov_base = (void *)m, .iov_len = sizeof(*m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = {.msg_iov = &io, .msg_iovlen = 1};
    if (fd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    if (sendmsg(sock, &mh, 0) != (ssize_t)sizeof(*m)) DIE("sendmsg: %s", strerror(errno));
}
static int recv_msg(int sock, struct msg *m, int *fd) {
    struct iovec io = {.iov_base = m, .iov_len = sizeof(*m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
    ssize_t n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    if (n <= 0) return -1;
    *fd = -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    if (c && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof(int));
    return 0;
}
static void send_buf(int slot, struct buf *b) {
    struct msg m = {.type = MSG_BUF, .slot = slot, .w = b->anwb.width, .h = b->anwb.height, .fourcc = b->h.format,
                    .stride = b->h.strides[0], .modifier = b->h.format_modifier};
    send_msg(csock, &m, b->h.fds[0]);
}

static void queue_to_child(int slot, int qi, int fence) {
    struct msg m = {.type = MSG_QUEUE, .slot = slot, .qi = qi, .ew = expect_w[(qi - 1) % 64], .eh = expect_h[(qi - 1) % 64]};
    memcpy(m.exp, expect_rgba[(qi - 1) % 64], 4);
    send_msg(csock, &m, fence);
    if (fence >= 0) close(fence);
}

static void *release_listener(void *arg) { /* parent: releases and the final result from the child */
    (void)arg;
    for (;;) {
        struct msg m;
        int fd;
        if (recv_msg(csock, &m, &fd) != 0) DIE("consumer process went away");
        if (m.type == MSG_RESULT) {
            fails += m.fails;
            frames_checked += m.frames_checked;
            wrong_pixels += m.wrong;
            return NULL;
        }
        pthread_mutex_lock(&W.m);
        struct buf *b = W.slot[m.slot];
        if (b->release_fence >= 0) close(b->release_fence);
        b->release_fence = fd;
        W.free_[m.slot] = true;
        W.nreleased++;
        pthread_cond_broadcast(&W.cv);
        pthread_mutex_unlock(&W.m);
    }
}

static int child_sock_fd;
static void publish_child(void *ctx, int rfd) {
    struct msg r = {.type = MSG_RELEASE, .slot = (int)(intptr_t)ctx};
    send_msg(child_sock_fd, &r, rfd);
    if (rfd >= 0) close(rfd);
}

static int consumer_child(int sock) { /* a fresh process (exec'd): its own NVIDIA EGL/Vulkan, its own dma-buf imports */
    child_sock_fd = sock;
    if (is_gl()) {
        EGLDeviceEXT dev;
        EGLint ndev = 0;
        eglQueryDevicesEXT(1, &dev, &ndev);
        cdpy = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
        if (!eglInitialize(cdpy, NULL, NULL)) DIE("child: display");
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint cxa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        cctx = eglCreateContext(cdpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cxa);
        if (!cctx) DIE("child: context 0x%x", eglGetError());
    }
    consumer_init();
    struct cview views[NBUF];
    memset(views, 0, sizeof(views));
    for (int i = 0; i < NBUF; i++) views[i].fd = -1;
    for (;;) {
        struct msg m;
        int fd;
        if (recv_msg(sock, &m, &fd) != 0) DIE("child: parent went away");
        if (m.type == MSG_BUF) {
            struct cview *v = &views[m.slot];
            if (v->cimg) eglDestroyImageKHR(cdpy, v->cimg);
            cview_release(v);
            if (v->fd >= 0) close(v->fd);
            *v = (struct cview){.fd = fd, .w = m.w, .h = m.h, .fourcc = m.fourcc, .stride = m.stride, .modifier = m.modifier};
        } else if (m.type == MSG_QUEUE) {
            consume_frame(&views[m.slot], m.qi, fd, m.exp, m.ew, m.eh, publish_child, (void *)(intptr_t)m.slot);
        } else if (m.type == MSG_QUIT) {
            for (int i = 0; i < NBUF; i++) cview_release(&views[i]);
            consumer_fini();
            vk_fini();
            struct msg r = {.type = MSG_RESULT, .fails = fails, .frames_checked = frames_checked, .wrong = wrong_pixels};
            send_msg(sock, &r, -1);
            return 0;
        }
    }
}

/* ---------------- producer ---------------- */
static int find_nvidia_render_node(void) {
    glob_t g;
    if (glob("/dev/dri/renderD*", 0, NULL, &g) != 0) DIE("no /dev/dri/renderD*");
    int fd = -1;
    for (size_t i = 0; i < g.gl_pathc && fd < 0; i++) {
        char p[256], v[32] = "";
        snprintf(p, sizeof(p), "/sys/class/drm/%s/device/vendor", strrchr(g.gl_pathv[i], '/') + 1);
        FILE *f = fopen(p, "r");
        if (f) { if (!fgets(v, sizeof(v), f)) v[0] = 0; fclose(f); }
        if (strncmp(v, "0x10de", 6) == 0) fd = open(g.gl_pathv[i], O_RDWR | O_CLOEXEC);
    }
    globfree(&g);
    if (fd < 0) DIE("no render node with PCI vendor 0x10de");
    return fd;
}

static int expected_age(struct buf *b, int nqueued) { return b->queued_at ? nqueued - b->queued_at + 1 : 0; }

int main(int argc, char **argv) {
    const char *gralloc = NULL, *frontp = NULL, *platlib = NULL;
    int frames = 60, cycles = 0, child_sock = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gralloc") && i + 1 < argc) gralloc = argv[++i];
        else if (!strcmp(argv[i], "--front") && i + 1 < argc) frontp = argv[++i];
        else if (!strcmp(argv[i], "--platform-lib") && i + 1 < argc) platlib = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--y-measure")) y_measure = 1;
        else if (!strcmp(argv[i], "--no-poison")) poison_on = 0;
        else if (!strcmp(argv[i], "--warm-consumer")) warm_consumer = 1;
        else if (!strcmp(argv[i], "--draw")) draw_mode = 1;
        else if (!strcmp(argv[i], "--probe-f4")) probe_f4 = 1;
        else if (!strcmp(argv[i], "--check-dequeue")) check_dequeue = 1;
        else if (!strcmp(argv[i], "--poison-from") && i + 1 < argc) poison_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--consumer-process")) consumer_process = 1;
        else if (!strcmp(argv[i], "--consumer-child") && i + 1 < argc) child_sock = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--x-cfinish")) x_cfinish = 1;
        else if (!strcmp(argv[i], "--x-prewarm")) x_prewarm = 1;
        else if (!strcmp(argv[i], "--x-cwarmfence")) x_cwarmfence = 1;
        else if (!strcmp(argv[i], "--vk-hal") && i + 1 < argc) vk_hal_path = argv[++i];
        else if (!strcmp(argv[i], "--vk-fills") && i + 1 < argc) vk_fills = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--consumer") && i + 1 < argc) {
            const char *t = argv[++i];
            int k = 0;
            while (k < 5 && strcmp(t, CTYPE_NAMES[k])) k++;
            if (k == 5) DIE("--consumer gl-write|gl-read|vk-write|vk-read|cpu");
            ctype = (enum ctype)k;
        }
        else DIE("usage: host-window --gralloc LIB --front libEGL_nvnative.so --platform-lib LIB [--frames N] [--y-measure] "
                 "[--no-poison] [--consumer gl-write|gl-read|vk-write|vk-read|cpu [--vk-hal vulkan.nvidia.so]] "
                 "[--consumer-process]");
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (child_sock >= 0) { /* the --consumer-process compositor */
        if (!frontp || !(front = dlopen(frontp, RTLD_NOW | RTLD_LOCAL))) DIE("child: front");
        p_eglGetProcAddress = (__eglMustCastToProperFunctionPointerType(*)(const char *))dlsym(front, "eglGetProcAddress");
        EGLFN(RES)
        GLFN(RES)
        return consumer_child(child_sock);
    }
    if (!gralloc || !frontp || !platlib) DIE("--gralloc, --front and --platform-lib are required");
    pid_t child = -1;
    pthread_t rl;
    if (consumer_process) { /* exec a fresh process before this one touches the driver */
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) DIE("socketpair");
        fcntl(sv[0], F_SETFD, FD_CLOEXEC);
        child = fork();
        if (child == 0) {
            char fdstr[16];
            snprintf(fdstr, sizeof(fdstr), "%d", sv[1]);
            char *cargv[20], fills[16];
            int n = 0;
            cargv[n++] = argv[0];
            cargv[n++] = "--consumer-child";
            cargv[n++] = fdstr;
            cargv[n++] = "--front";
            cargv[n++] = (char *)frontp;
            cargv[n++] = "--consumer";
            cargv[n++] = (char *)CTYPE_NAMES[ctype];
            if (vk_hal_path) { cargv[n++] = "--vk-hal"; cargv[n++] = (char *)vk_hal_path; }
            snprintf(fills, sizeof fills, "%d", vk_fills);
            cargv[n++] = "--vk-fills";
            cargv[n++] = fills;
            if (!poison_on) cargv[n++] = "--no-poison";
            if (y_measure) cargv[n++] = "--y-measure";
            if (x_cwarmfence) cargv[n++] = "--x-cwarmfence";
            cargv[n] = NULL;
            execv("/proc/self/exe", cargv);
            _exit(127);
        }
        close(sv[1]);
        csock = sv[0];
        printf("consumer: separate process %d (its own EGL and dma-buf imports, fences over SCM_RIGHTS)\n", (int)child);
    }

    /* The test process owns its environment: point the driver at the platform under test (in the guest the
     * shims' nvglibc-default-env does this without touching the app's environment). */
    char json[] = "/tmp/host-window-XXXXXX.json";
    int jf = mkstemps(json, 5);
    char abs[4096];
    if (jf < 0 || !realpath(platlib, abs)) DIE("json/realpath");
    dprintf(jf, "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"%s\"}}\n", abs);
    close(jf);
    setenv("__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES", json, 1);

    void *gl = dlopen(gralloc, RTLD_NOW);
    if (!gl) DIE("dlopen %s: %s", gralloc, dlerror());
    gops = ((struct gbm_ops * (*)(void))dlsym(gl, "get_gbm_ops"))();
    gdev = gops->dev_create(find_nvidia_render_node());
    if (!gdev) DIE("gralloc dev_create");

    front = dlopen(frontp, RTLD_NOW | RTLD_LOCAL);
    if (!front) DIE("dlopen %s: %s", frontp, dlerror());
    p_eglGetProcAddress = (__eglMustCastToProperFunctionPointerType(*)(const char *))dlsym(front, "eglGetProcAddress");
    if (!p_eglGetProcAddress) DIE("front exports no eglGetProcAddress");
    EGLFN(RES)
    GLFN(RES)

    /* the display, as Android's egl_display_t::getPlatformDisplay does it */
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) DIE("eglGetPlatformDisplay(ANDROID) failed 0x%x", eglGetError());
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) DIE("eglInitialize 0x%x", eglGetError());
    const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
    const char *cext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("EGL %d.%d; client extensions: %s\n", maj, min, cext);
    for (const char *n = "EGL_ANDROID_image_native_buffer\0EGL_ANDROID_recordable\0EGL_ANDROID_framebuffer_target\0"
                         "EGL_ANDROID_native_fence_sync\0EGL_KHR_wait_sync\0EGL_EXT_buffer_age\0EGL_KHR_partial_update\0";
         *n; n += strlen(n) + 1)
        CHECK(strstr(ext, n) != NULL, "display extensions lack %s", n);
    CHECK(strstr(cext, "EGL_KHR_platform_android") != NULL, "client extensions lack EGL_KHR_platform_android");

    /* configs: window configs and their Android attributes */
    EGLint nall = 0;
    eglGetConfigs(dpy, NULL, 0, &nall);
    EGLConfig *all = calloc((size_t)nall, sizeof(EGLConfig));
    eglGetConfigs(dpy, all, nall, &nall);
    int nwin = 0;
    for (int i = 0; i < nall; i++) {
        EGLint st = 0, vid = 0, rec = -1, fbt = -1, ct = 0, r, g, b, a, d, s, ms;
        eglGetConfigAttrib(dpy, all[i], EGL_SURFACE_TYPE, &st);
        if (!(st & EGL_WINDOW_BIT)) continue;
        nwin++;
        eglGetConfigAttrib(dpy, all[i], EGL_NATIVE_VISUAL_ID, &vid);
        CHECK(eglGetConfigAttrib(dpy, all[i], EGL_RECORDABLE_ANDROID, &rec) && rec == EGL_TRUE, "config %d: RECORDABLE", i);
        CHECK(eglGetConfigAttrib(dpy, all[i], EGL_FRAMEBUFFER_TARGET_ANDROID, &fbt) && fbt == EGL_TRUE, "config %d: FBT", i);
        CHECK(eglGetConfigAttrib(dpy, all[i], EGL_COLOR_COMPONENT_TYPE_EXT, &ct), "config %d: EGL_COLOR_COMPONENT_TYPE_EXT not answered (0x%x)", i, eglGetError());
        eglGetConfigAttrib(dpy, all[i], EGL_RED_SIZE, &r); eglGetConfigAttrib(dpy, all[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(dpy, all[i], EGL_BLUE_SIZE, &b); eglGetConfigAttrib(dpy, all[i], EGL_ALPHA_SIZE, &a);
        eglGetConfigAttrib(dpy, all[i], EGL_DEPTH_SIZE, &d); eglGetConfigAttrib(dpy, all[i], EGL_STENCIL_SIZE, &s);
        eglGetConfigAttrib(dpy, all[i], EGL_SAMPLES, &ms);
        if (nwin <= 12 || getenv("HW_ALLCONFIGS"))
            printf("window config %p: rgba %d%d%d%d depth %d stencil %d samples %d visual %d component 0x%x surface 0x%x\n",
                   all[i], r, g, b, a, d, s, ms, vid, ct, st);
    }
    printf("%d of %d configs are window configs\n", nwin, nall);
    CHECK(nwin > 0, "no window configs");
    free(all);

    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
                         EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_RECORDABLE_ANDROID, EGL_TRUE, EGL_NONE};
    EGLConfig cfg;
    EGLint nc = 0;
    if (!eglChooseConfig(dpy, ca, &cfg, 1, &nc) || nc < 1) DIE("eglChooseConfig(WINDOW, RGBA8888, RECORDABLE) 0x%x", eglGetError());
    EGLint vid = 0;
    eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid);
    printf("chosen config %p, EGL_NATIVE_VISUAL_ID %d\n", cfg, vid);
    EGLint mods_n = 0;
    eglQueryDmaBufModifiersEXT(dpy, DRM_FORMAT_ABGR8888, 0, NULL, NULL, &mods_n);
    EGLuint64KHR mods[64];
    EGLBoolean extonly[64];
    if (mods_n > 64) mods_n = 64;
    eglQueryDmaBufModifiersEXT(dpy, DRM_FORMAT_ABGR8888, mods_n, mods, extonly, &mods_n);
    printf("driver dma-buf modifiers for AB24:");
    for (int i = 0; i < mods_n; i++) printf(" 0x%016llx%s", (unsigned long long)mods[i], extonly[i] ? "(ext)" : "");
    printf("\n");

    /* the window: RGBA_8888 (the loader's choice for an 8888 config with alpha, egl_platform_entries.cpp:571-631) */
    W.w.common.magic = (int)NVN_ANDROID_NATIVE_WINDOW_MAGIC;
    W.w.common.version = sizeof(struct ANativeWindow);
    W.w.common.incRef = w_inc;
    W.w.common.decRef = w_dec;
    W.w.setSwapInterval = w_setSwapInterval;
    W.w.query = w_query;
    W.w.perform = w_perform;
    W.w.dequeueBuffer = w_dequeue;
    W.w.queueBuffer = w_queue;
    W.w.cancelBuffer = w_cancel;
    pthread_mutex_init(&W.m, NULL);
    pthread_cond_init(&W.cv, NULL);
    W.width = 256;
    W.height = 128;
    W.fourcc = DRM_FORMAT_ABGR8888;
    W.refs = 1;
    /* loader steps (§1.5): api_connect, buffers format + dataspace, swap interval 1; then the driver */
    W.w.perform(&W.w, NVN_NATIVE_WINDOW_API_CONNECT, NVN_NATIVE_WINDOW_API_EGL);
    W.w.perform(&W.w, NVN_NATIVE_WINDOW_SET_BUFFERS_FORMAT, NVN_HAL_PIXEL_FORMAT_RGBA_8888);
    W.w.perform(&W.w, NVN_NATIVE_WINDOW_SET_BUFFERS_DATASPACE, 0);
    W.w.setSwapInterval(&W.w, 1);
    for (int i = 0; i < NBUF; i++) {
        W.slot[i] = buf_alloc(W.width, W.height, W.fourcc, W.hal, false,
                              GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER);
        W.free_[i] = true;
    }
    if (consumer_process) {
        for (int i = 0; i < NBUF; i++) send_buf(i, W.slot[i]);
        pthread_create(&rl, NULL, release_listener, NULL);
    }
    printf("window buffers: %dx%d AB24 stride %u modifier 0x%016llx (gralloc host build)\n", W.width, W.height,
           W.slot[0]->h.strides[0], (unsigned long long)W.slot[0]->h.format_modifier);

    /* consumer context on the device display */
    EGLDeviceEXT dev;
    EGLint ndev = 0;
    eglQueryDevicesEXT(1, &dev, &ndev);
    cdpy = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
    if (!eglInitialize(cdpy, NULL, NULL)) DIE("consumer display");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cxa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    cctx = eglCreateContext(cdpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cxa);
    if (!cctx) DIE("consumer context 0x%x", eglGetError());
    pthread_t ct;
    if (!consumer_process) pthread_create(&ct, NULL, consumer, NULL);
    if (warm_consumer && !consumer_process) {
        pthread_mutex_lock(&W.m);
        while (!consumer_ready) pthread_cond_wait(&W.cv, &W.m);
        pthread_mutex_unlock(&W.m);
    }

    /* --cycles: many short surface lifetimes on the same window (create, 6 frames, destroy), each frame checked */
    if (cycles > 0) {
        EGLContext cctx2 = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cxa);
        int fbase = 0;
        for (int cy = 0; cy < cycles; cy++) {
            EGLSurface s2 = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)(uintptr_t)&W.w, NULL);
            if (s2 == EGL_NO_SURFACE || !eglMakeCurrent(dpy, s2, s2, cctx2)) DIE("cycle %d: create/makecurrent 0x%x", cy, eglGetError());
            for (int f = 0; f < 6; f++, fbase++) {
                EGLint sw = 0, sh = 0;
                eglQuerySurface(dpy, s2, EGL_WIDTH, &sw);
                eglQuerySurface(dpy, s2, EGL_HEIGHT, &sh);
                uint8_t c[4] = {(uint8_t)(40 + 7 * fbase), (uint8_t)(200 - 3 * fbase), (uint8_t)(fbase * 13), 255};
                pthread_mutex_lock(&W.m);
                int qi = W.nqueued; /* this frame's queue index - 1 */
                pthread_mutex_unlock(&W.m);
                memcpy(expect_rgba[qi % 64], c, 4);
                expect_w[qi % 64] = sw;
                expect_h[qi % 64] = sh;
                glViewport(0, 0, sw, sh);
                glClearColor(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glEnable(GL_SCISSOR_TEST);
                glScissor(0, sh - sh / 4, sw, sh / 4);
                glClearColor(1, 1, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
                CHECK(eglSwapBuffers(dpy, s2), "cycle %d frame %d swap 0x%x", cy, f, eglGetError());
            }
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(dpy, s2);
        }
        /* wait until the consumer has seen everything */
        for (;;) {
            pthread_mutex_lock(&W.m);
            int done = W.nreleased >= W.nqueued;
            pthread_mutex_unlock(&W.m);
            if (done) break;
            usleep(1000);
        }
        usleep(200000);
        printf("cycles: %d surface lifetimes x 6 frames, %d frames checked, %d wrong pixels\n", cycles, frames_checked, wrong_pixels);
        eglDestroyContext(dpy, cctx2);
        frames_checked = 0;
    }

    for (int i = 0; i < NBUF; i++) W.slot[i]->queued_at = 0; /* a new surface starts a new age history */
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)(uintptr_t)&W.w, NULL);
    if (surf == EGL_NO_SURFACE) DIE("eglCreateWindowSurface 0x%x", eglGetError());
    CHECK(W.usage == GRALLOC_USAGE_HW_RENDER, "window usage 0x%llx, expected HW_RENDER", (unsigned long long)W.usage);
    CHECK(W.refs == 2, "window references after create %d, expected 2", W.refs);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cxa);
    if (!ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) DIE("producer context/makecurrent 0x%x", eglGetError());
    printf("GL_RENDERER %s, GL_VERSION %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    EGLBoolean sb = eglSurfaceAttrib(dpy, surf, EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED);
    printf("eglSurfaceAttrib(EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED) -> %d (0x%x) [HWUI needs TRUE]\n", sb,
           sb ? EGL_SUCCESS : eglGetError());

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int age_bad = 0;
    for (int f = 0; f < frames; f++) {
        if (frames >= 12 && f == frames / 2) { /* resize: the window hands out new buffers from the next dequeue on */
            pthread_mutex_lock(&W.m);
            W.width = 192;
            W.height = 96;
            W.gen++;
            pthread_mutex_unlock(&W.m);
        }
        if (frames >= 12 && f == frames / 4) CHECK(eglSwapInterval(dpy, 0) && W.interval == 0, "swap interval 0 not forwarded (%d)", W.interval);
        if (frames >= 12 && f == frames / 4 + 5) CHECK(eglSwapInterval(dpy, 1) && W.interval == 1, "swap interval 1 not forwarded (%d)", W.interval);
        EGLint age = -1, sw = 0, sh = 0;
        eglQuerySurface(dpy, surf, EGL_BUFFER_AGE_EXT, &age);
        eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
        pthread_mutex_lock(&W.m);
        struct buf *cur = W.slot[W.dequeued_slot];
        int want_age = expected_age(cur, W.nqueued), qn = W.nqueued;
        pthread_mutex_unlock(&W.m);
        if (f < 6) printf("frame %d: back buffer slot %d, release fence %s\n", f + 1, W.dequeued_slot,
                          !W.last_release_fd_given ? "none" : W.last_release_signalled == 1 ? "given, already signalled" : "given, pending");
        if (age != want_age && age_bad++ < 3) printf("frame %d: buffer age %d, expected %d\n", f + 1, age, want_age);
        CHECK(sw == cur->anwb.width && sh == cur->anwb.height, "frame %d: EGL_WIDTH/HEIGHT %dx%d, buffer %dx%d", f + 1, sw,
              sh, cur->anwb.width, cur->anwb.height);
        EGLint dmg[4] = {0, 0, sw, sh};
        CHECK(eglSetDamageRegionKHR(dpy, surf, dmg, 1), "eglSetDamageRegionKHR 0x%x", eglGetError());
        uint8_t c[4] = {(uint8_t)(40 + 7 * f), (uint8_t)(200 - 3 * f), (uint8_t)(f * 13), 255};
        memcpy(expect_rgba[qn % 64], c, 4);
        expect_w[qn % 64] = sw;
        expect_h[qn % 64] = sh;
        glViewport(0, 0, sw, sh);
        glDisable(GL_SCISSOR_TEST);
        if (draw_mode) { /* the same picture with draws instead of clears */
            static GLuint dprog, dvao;
            static GLint uc;
            if (!dprog) {
                dprog = program(VS_FULL, "#version 300 es\nprecision highp float;\nuniform vec4 c;\nout vec4 o;\nvoid main(){o=c;}\n");
                uc = glGetUniformLocation(dprog, "c");
                glGenVertexArrays(1, &dvao);
            }
            glUseProgram(dprog);
            glBindVertexArray(dvao);
            glUniform4f(uc, c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, sh - sh / 4, sw, sh / 4);
            glUniform4f(uc, 1, 1, 0, 1);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDisable(GL_SCISSOR_TEST);
        } else {
        glClearColor(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, sh - sh / 4, sw, sh / 4); /* GL window coordinates: the top quarter of the window */
        glClearColor(1, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        }
        if (probe_f4 && f == 3) { /* where does frame 4 land? (producer side, before the swap) */
            uint8_t q[4];
            glReadPixels(sw / 2, sh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
            glFinish();
            printf("f4probe: producer glReadPixels centre %u,%u,%u,%u (frame colour %u,%u,%u)\n", q[0], q[1], q[2], q[3], c[0], c[1], c[2]);
            pthread_mutex_lock(&W.m);
            for (int k = 0; k < NBUF; k++) {
                struct buf *o = W.slot[k];
                size_t sz = (size_t)lseek(o->h.fds[0], 0, SEEK_END);
                const uint8_t *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, o->h.fds[0], 0);
                size_t n4 = 0, npois = 0;
                for (size_t x = 0; m != MAP_FAILED && x + 4 <= sz; x += 4) {
                    if (m[x] == c[0] && m[x + 1] == c[1] && m[x + 2] == c[2]) n4++;
                    else if (m[x] == 255 && m[x + 1] == 0 && m[x + 2] == 255) npois++;
                }
                if (m != MAP_FAILED) munmap((void *)m, sz);
                printf("f4probe: slot %d%s: frame-4 colour %zu px, poison %zu px\n", k, k == W.dequeued_slot ? " (dequeued = back)" : "", n4, npois);
            }
            pthread_mutex_unlock(&W.m);
        }
        EGLBoolean ok = (f & 1) ? eglSwapBuffersWithDamageKHR(dpy, surf, dmg, 1) : eglSwapBuffers(dpy, surf);
        CHECK(ok, "frame %d: swap failed 0x%x", f + 1, eglGetError());
        CHECK(glGetError() == GL_NO_ERROR, "frame %d: GL error", f + 1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(age_bad == 0, "%d frames with a wrong EGL_BUFFER_AGE", age_bad);

    /* ---- EGL_ANDROID_image_native_buffer ---- */
    {
        /* (a) LINEAR CPU-written buffer (what gralloc makes for SW usage) sampled as GL_TEXTURE_EXTERNAL_OES */
        const int iw = 64, ih = 32;
        struct buf *lb = buf_alloc(iw, ih, DRM_FORMAT_ABGR8888, NVN_HAL_PIXEL_FORMAT_RGBA_8888, true, 0x33 /* SW_*_OFTEN */);
        struct gbm_bo *bo = gops->import(gdev, lb->h.fds[0], iw, ih, lb->h.strides[0], lb->h.format_modifier, lb->h.format);
        void *addr = NULL, *md = NULL;
        gops->map(bo, iw, ih, &addr, &md);
        if (!addr || addr == (void *)-1) DIE("map of the LINEAR buffer failed");
        for (int y = 0; y < ih; y++)
            for (int x = 0; x < iw; x++) {
                uint8_t *p = (uint8_t *)addr + (size_t)y * lb->h.strides[0] + 4 * x;
                p[0] = (uint8_t)(x * 4); p[1] = (uint8_t)(y * 8); p[2] = (uint8_t)(x ^ y); p[3] = 255;
            }
        gops->unmap(bo, md);
        gops->free(bo);
        EGLint ia[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        EGLImageKHR img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)&lb->anwb, ia);
        CHECK(img != EGL_NO_IMAGE_KHR, "image_native_buffer (LINEAR) failed 0x%x", eglGetError());
        if (img) {
            GLuint tex, fbo, rb;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
            CHECK(glGetError() == GL_NO_ERROR, "glEGLImageTargetTexture2DOES(EXTERNAL_OES) on the LINEAR image");
            glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, iw, ih);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            GLuint prog = program(VS_FULL, "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\n"
                                           "precision highp float;\nuniform samplerExternalOES t;\nin vec2 uv;\nout vec4 o;\n"
                                           "void main(){o=texelFetch(t, ivec2(gl_FragCoord.xy), 0);}\n");
            glUseProgram(prog);
            glUniform1i(glGetUniformLocation(prog, "t"), 0);
            GLuint vao;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glViewport(0, 0, iw, ih);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            uint8_t px[64 * 32 * 4];
            glReadPixels(0, 0, iw, ih, GL_RGBA, GL_UNSIGNED_BYTE, px);
            int bad = 0;
            for (int y = 0; y < ih; y++)
                for (int x = 0; x < iw; x++) {
                    const uint8_t *p = px + 4 * (y * iw + x);
                    if (p[0] != (uint8_t)(x * 4) || p[1] != (uint8_t)(y * 8) || p[2] != (uint8_t)(x ^ y) || p[3] != 255) bad++;
                }
            printf("image_native_buffer LINEAR (CPU-written, sampled as GL_TEXTURE_EXTERNAL_OES): %d of %d texels wrong\n", bad, iw * ih);
            CHECK(bad == 0, "LINEAR image contents");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteRenderbuffers(1, &rb);
            glDeleteTextures(1, &tex);
            glDeleteProgram(prog);
            eglDestroyImageKHR(dpy, img);
        }
        b_dec(&lb->anwb.common);

        /* (b) block-linear GPU buffer as a GL_TEXTURE_2D render target, read back */
        struct buf *gb = buf_alloc(iw, ih, DRM_FORMAT_ABGR8888, NVN_HAL_PIXEL_FORMAT_RGBA_8888, false, 0x300);
        img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)&gb->anwb, ia);
        CHECK(img != EGL_NO_IMAGE_KHR, "image_native_buffer (block-linear 0x%016llx) failed 0x%x",
              (unsigned long long)gb->h.format_modifier, eglGetError());
        EGLImageKHR bad_ctx = eglCreateImageKHR(dpy, ctx, EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)&gb->anwb, ia);
        CHECK(bad_ctx == EGL_NO_IMAGE_KHR && eglGetError() == EGL_BAD_CONTEXT, "a context with EGL_NATIVE_BUFFER_ANDROID must fail EGL_BAD_CONTEXT");
        if (img) {
            GLuint tex, fbo;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            CHECK(st == GL_FRAMEBUFFER_COMPLETE, "block-linear image FBO 0x%x", st);
            glViewport(0, 0, iw, ih);
            glClearColor(0.25f, 0.5f, 0.75f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            uint8_t px[4];
            glReadPixels(5, 7, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            printf("image_native_buffer block-linear 0x%016llx as GL_TEXTURE_2D render target: readback %u,%u,%u,%u\n",
                   (unsigned long long)gb->h.format_modifier, px[0], px[1], px[2], px[3]);
            CHECK(px[0] == 64 && (px[1] == 127 || px[1] == 128) && px[2] == 191 && px[3] == 255, "block-linear image readback");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            eglDestroyImageKHR(dpy, img);
        }
        b_dec(&gb->anwb.common);
    }

    /* ---- teardown: cancel on destroy, references returned ---- */
    int cancels = W.ncancel;
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    CHECK(eglDestroySurface(dpy, surf), "eglDestroySurface 0x%x", eglGetError());
    CHECK(W.ncancel == cancels + 1 && W.last_cancel_fence_ok, "destroy: %d cancelBuffer calls (expected 1), fence ok %d",
          W.ncancel - cancels, W.last_cancel_fence_ok);
    CHECK(W.refs == 1, "window references after destroy %d, expected 1", W.refs);
    eglDestroyContext(dpy, ctx);
    if (consumer_process) {
        for (;;) { /* let the compositor finish the queued frames */
            pthread_mutex_lock(&W.m);
            int done = W.nreleased >= W.nqueued;
            pthread_mutex_unlock(&W.m);
            if (done) break;
            usleep(1000);
        }
        struct msg q = {.type = MSG_QUIT};
        send_msg(csock, &q, -1);
        pthread_join(rl, NULL);
        int st = 0;
        waitpid(child, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "consumer process exit status 0x%x", st);
    } else {
        pthread_mutex_lock(&W.m);
        W.quit = true;
        pthread_cond_broadcast(&W.cv);
        pthread_mutex_unlock(&W.m);
        pthread_join(ct, NULL);
    }
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%d frames rendered, %d checked by the %s consumer (%s), %d wrong pixels, %.1f fps (poison pass %s)\n", frames,
           frames_checked, CTYPE_NAMES[ctype], consumer_process ? "separate process" : "in-process thread", wrong_pixels,
           frames / secs, writes() && poison_on ? "on" : "off");
    CHECK(frames_checked == frames, "consumer checked %d of %d frames", frames_checked, frames);
    for (int i = 0; i < NBUF; i++) {
        CHECK(W.slot[i]->refs == 1, "buffer %d: %d references after teardown (expected the window's 1)", i, W.slot[i]->refs);
    }
    for (int i = 0; i < NBUF; i++) {
        struct buf *b = W.slot[i];
        if (b->cv.cimg) eglDestroyImageKHR(cdpy, b->cv.cimg);
        b_dec(&b->anwb.common);
    }
    if (!consumer_process) vk_fini();
    CHECK(live_bufs == 0, "%d gralloc buffers still alive (leaked references)", live_bufs);
    eglTerminate(dpy);
    eglReleaseThread();
    unlink(json);
    printf("host-window: %s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails;
}
