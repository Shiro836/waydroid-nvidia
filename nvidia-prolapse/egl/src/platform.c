/* libnvnative-egl-android.so: load entry, display, configs, hooks, EGL_ANDROID_image_native_buffer.
 *
 * Structure follows NVIDIA's egl-wayland2 v1.0.2 (wayland-platform.c / wayland-fbconfig.c /
 * wayland-display.c), Android semantics follow Mesa's platform_android.c (mesa-25.2.0 0b97dcdd).
 * Every refusal is loud: logcat/stderr plus the EGL error, naming the fact and the remedy. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

#include "nvn.h"

/* ---------------- logging ---------------- */

static void vlog(enum nvn_prio prio, const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
#ifdef __ANDROID__
    static const int map[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_write(map[prio], "nvnative-egl", buf);
#endif
    if (prio >= NVN_LOG_WARN || getenv("NVNATIVE_EGL_DEBUG") != NULL) {
        fprintf(stderr, "nvnative-egl: %s\n", buf);
    }
}

void nvn_log(enum nvn_prio prio, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(prio, fmt, ap);
    va_end(ap);
}

/* The front's opt-in diagnosis switch (front.c "GL error log"): debug.nvnative.gldebug=1 / NVNATIVE_GL_DEBUG=1. */
int nvn_debug_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVNATIVE_GL_DEBUG");
        int v = e != NULL && strcmp(e, "1") == 0;
#ifdef __ANDROID__
        char p[PROP_VALUE_MAX] = "";
        if (__system_property_get("debug.nvnative.gldebug", p) > 0 && strcmp(p, "1") == 0) v = 1;
#endif
        on = v;
    }
    return on;
}

void nvn_error(EplPlatformData *plat, EGLint error, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    nvn_log(NVN_LOG_ERROR, "%s (EGL error 0x%04x)", buf, error);
    if (plat != NULL) {
        eplSetError(plat, error, "%s", buf);
    }
}

/* ---------------- display ---------------- */

static void nvnCleanupPlatform(EplPlatformData *plat) { (void)plat; }

EplImplDisplay *nvnDisplayPriv(EplDisplay *pdpy) { return pdpy->priv; }

static EGLBoolean nvnGetPlatformDisplay(EplPlatformData *plat, EplDisplay *pdpy, void *native_display,
                                        const EGLAttrib *attribs, struct glvnd_list *existing_displays) {
    (void)attribs;
    (void)existing_displays;
    /* Android has exactly one display, EGL_DEFAULT_DISPLAY (egl_display.cpp:126-130). */
    if (native_display != EGL_DEFAULT_DISPLAY) {
        nvn_error(plat, EGL_BAD_PARAMETER, "EGL_PLATFORM_ANDROID_KHR: native display %p is not EGL_DEFAULT_DISPLAY",
                  native_display);
        return EGL_FALSE;
    }
    EGLint count = 0;
    EGLDeviceEXT *devs = eplGetAllDevices(plat, &count);
    if (devs == NULL || count < 1) {
        free(devs);
        nvn_error(plat, EGL_NOT_INITIALIZED, "EGL_PLATFORM_ANDROID_KHR: the driver enumerates no EGL device "
                  "(eglQueryDevicesEXT count %d). Remedy: /dev/nvidiactl, /dev/nvidia0 and the NVIDIA render node "
                  "must be visible to this process", count);
        return EGL_FALSE;
    }
    if (count > 1) {
        free(devs);
        nvn_error(plat, EGL_BAD_ALLOC, "EGL_PLATFORM_ANDROID_KHR: %d NVIDIA EGL devices; choosing the device that "
                  "allocated the gralloc buffers is not implemented (single NVIDIA GPU only)", count);
        return EGL_FALSE;
    }
    EplImplDisplay *priv = calloc(1, sizeof(*priv));
    if (priv == NULL) {
        free(devs);
        nvn_error(plat, EGL_BAD_ALLOC, "out of memory");
        return EGL_FALSE;
    }
    priv->device = devs[0];
    free(devs);
    pdpy->priv = priv;
    return EGL_TRUE;
}

static void nvnCleanupDisplay(EplDisplay *pdpy) {
    free(pdpy->priv);
    pdpy->priv = NULL;
}

static void free_display_state(EplImplDisplay *priv) {
    eplConfigListFree(priv->configs);
    priv->configs = NULL;
    free(priv->extra);
    priv->extra = NULL;
    free(priv->extension_string);
    priv->extension_string = NULL;
}

/* Non-external modifiers the driver imports for `fourcc` (wayland-fbconfig.c:21-155: external-only
 * modifiers cannot be render targets). Returns the count, 0 if none. */
static EGLint render_modifier_count(EplPlatformData *plat, EGLDisplay idpy, uint32_t fourcc) {
    EGLint n = 0;
    if (!plat->priv->egl.QueryDmaBufModifiers(idpy, (EGLint)fourcc, 0, NULL, NULL, &n) || n <= 0) {
        return 0;
    }
    EGLuint64KHR *mods = calloc((size_t)n, sizeof(*mods));
    EGLBoolean *ext = calloc((size_t)n, sizeof(*ext));
    EGLint good = 0;
    if (mods != NULL && ext != NULL && plat->priv->egl.QueryDmaBufModifiers(idpy, (EGLint)fourcc, n, mods, ext, &n)) {
        for (EGLint i = 0; i < n; i++) {
            if (!ext[i]) good++;
        }
    }
    free(mods);
    free(ext);
    return good;
}

static EGLBoolean nvnInitializeDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor) {
    EplImplDisplay *priv = pdpy->priv;
    priv->idpy = eplGetDeviceInternalDisplay(plat, priv->device);
    if (priv->idpy == NULL) {
        nvn_error(plat, EGL_NOT_INITIALIZED, "EGL_PLATFORM_ANDROID_KHR: eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT) "
                  "failed (driver error 0x%04x)", plat->egl.GetError());
        return EGL_FALSE;
    }
    if (!eplInitializeInternalDisplay(plat, priv->idpy, major, minor)) {
        nvn_error(plat, EGL_NOT_INITIALIZED, "EGL_PLATFORM_ANDROID_KHR: eglInitialize of the device display failed "
                  "(driver error 0x%04x)", plat->egl.GetError());
        return EGL_FALSE;
    }
    EGLDisplay idpy = priv->idpy->edpy;
    pdpy->internal_display = idpy;

    /* What the window/image paths need from the driver's display; refusing is loud (no glFinish or CPU-wait
     * fallback). EGL_ANDROID_native_fence_sync disappears silently when libdrm.so.2 is absent (abi-contract
     * §12), so name that remedy. */
    const char *ext = plat->egl.QueryString(idpy, EGL_EXTENSIONS);
    static const struct { const char *name, *remedy; } need[] = {
        {"EGL_ANDROID_native_fence_sync", "the driver drops it when libdrm.so.2 cannot be loaded: put the bionic "
                                          "libdrm.so.2 the driver builds next to libEGL_nvidia.so.0"},
        {"EGL_KHR_wait_sync", "driver too old"},
        {"EGL_EXT_image_dma_buf_import", "the NVIDIA render node must be visible and libdrm.so.2 loadable"},
        {"EGL_EXT_image_dma_buf_import_modifiers", "the NVIDIA render node must be visible and libdrm.so.2 loadable"},
    };
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        if (!eplFindExtension(need[i].name, ext)) {
            nvn_error(plat, EGL_NOT_INITIALIZED, "EGL_PLATFORM_ANDROID_KHR: the driver's device display lacks %s "
                      "(remedy: %s)", need[i].name, need[i].remedy);
            eplTerminateInternalDisplay(plat, priv->idpy);
            return EGL_FALSE;
        }
    }

    /* Window configs: stream-capable configs whose fourcc is a format Android buffers come in and the driver
     * can import as a render target (wayland-fbconfig.c SetupConfig; Mesa droid_add_configs_for_visuals). */
    priv->configs = eplConfigListCreate(plat, idpy);
    if (priv->configs == NULL) {
        nvn_error(plat, EGL_NOT_INITIALIZED, "EGL_PLATFORM_ANDROID_KHR: the driver lists no EGLConfigs");
        eplTerminateInternalDisplay(plat, priv->idpy);
        return EGL_FALSE;
    }
    priv->extra = calloc((size_t)priv->configs->num_configs, sizeof(*priv->extra));
    if (priv->extra == NULL) {
        nvn_error(plat, EGL_BAD_ALLOC, "out of memory");
        free_display_state(priv);
        eplTerminateInternalDisplay(plat, priv->idpy);
        return EGL_FALSE;
    }
    EGLint windows = 0;
    for (EGLint i = 0; i < priv->configs->num_configs; i++) {
        EplConfig *c = &priv->configs->configs[i];
        EGLint fourcc = DRM_FORMAT_INVALID;
        c->surfaceMask &= ~(EGL_WINDOW_BIT | EGL_PIXMAP_BIT);
        c->nativeVisualID = 0;
        c->nativeVisualType = EGL_NONE;
        if (!plat->priv->nvx.GetConfigAttrib(idpy, c->config, EGL_LINUX_DRM_FOURCC_EXT, &fourcc)) {
            fourcc = DRM_FORMAT_INVALID;
        }
        c->fourcc = (uint32_t)fourcc;
        int hal = nvn_hal_format_for_fourcc(c->fourcc);
        if (hal == 0 || !(c->surfaceMask & EGL_STREAM_BIT_KHR) ||
            render_modifier_count(plat, idpy, c->fourcc) == 0) {
            continue;
        }
        c->surfaceMask |= EGL_WINDOW_BIT;
        c->nativeVisualID = hal;
        c->nativeVisualType = hal; /* Mesa sets both (platform_android.c:838-847) */
        priv->extra[i].recordable = EGL_TRUE;
        priv->extra[i].framebuffer_target = EGL_TRUE;
        windows++;
    }
    if (windows == 0) {
        nvn_error(plat, EGL_NOT_INITIALIZED, "EGL_PLATFORM_ANDROID_KHR: no EGLConfig of the driver is a stream config "
                  "with an Android buffer format the driver imports as a render target");
        free_display_state(priv);
        eplTerminateInternalDisplay(plat, priv->idpy);
        return EGL_FALSE;
    }

    static const char ADD[] = " EGL_ANDROID_image_native_buffer EGL_ANDROID_recordable EGL_ANDROID_framebuffer_target";
    size_t len = ext ? strlen(ext) : 0;
    priv->extension_string = malloc(len + sizeof(ADD));
    if (priv->extension_string == NULL) {
        nvn_error(plat, EGL_BAD_ALLOC, "out of memory");
        free_display_state(priv);
        eplTerminateInternalDisplay(plat, priv->idpy);
        return EGL_FALSE;
    }
    memcpy(priv->extension_string, ext ? ext : "", len);
    memcpy(priv->extension_string + len, len ? ADD : ADD + 1, sizeof(ADD) - (len ? 0 : 1));
    nvn_log(NVN_LOG_INFO, "EGL_PLATFORM_ANDROID_KHR display %p initialised: EGL %d.%d, %d of %d configs are window "
            "configs, NVX interface 0x%x", (void *)pdpy->external_display, major ? *major : 0, minor ? *minor : 0,
            windows, priv->configs->num_configs, (unsigned)plat->priv->nvx_version);
    return EGL_TRUE;
}

static void nvnTerminateDisplay(EplPlatformData *plat, EplDisplay *pdpy) {
    EplImplDisplay *priv = pdpy->priv;
    free_display_state(priv);
    if (priv->idpy != NULL) {
        eplTerminateInternalDisplay(plat, priv->idpy);
        priv->idpy = NULL;
    }
}

static const char *nvnQueryString(EplPlatformData *plat, EplDisplay *pdpy, EGLExtPlatformString name) {
    (void)plat;
    (void)pdpy;
    switch (name) {
    case EGL_EXT_PLATFORM_PLATFORM_CLIENT_EXTENSIONS:
        return "EGL_KHR_platform_android";
    case EGL_EXT_PLATFORM_DISPLAY_EXTENSIONS:
        return ""; /* the display string comes from the eglQueryString hook */
    default:
        return NULL;
    }
}

/* ---------------- hooks ---------------- */

static const char *HookQueryString(EGLDisplay edpy, EGLint name) {
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL) {
        return NULL;
    }
    const char *s = NULL;
    if (name == EGL_EXTENSIONS) {
        s = pdpy->priv->extension_string;
    }
    if (s == NULL) {
        s = pdpy->platform->egl.QueryString(pdpy->internal_display, name);
    }
    eplDisplayRelease(pdpy);
    return s;
}

static EGLBoolean HookChooseConfig(EGLDisplay edpy, const EGLint *attribs, EGLConfig *configs, EGLint size,
                                   EGLint *count) {
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL) {
        return EGL_FALSE;
    }
    EplImplDisplay *priv = pdpy->priv;
    /* The driver rejects EGL_RECORDABLE_ANDROID (EGL_BAD_ATTRIBUTE, NV-EGL-CONTRACT §3): strip the two
     * Android booleans and filter on them here. */
    EGLint n = eplCountAttribs32(attribs);
    EGLint *copy = malloc(((size_t)n + 1) * sizeof(EGLint));
    EGLint want_rec = EGL_DONT_CARE, want_fbt = EGL_DONT_CARE, m = 0;
    EGLBoolean ok = EGL_FALSE;
    EplConfig **found = NULL;
    EGLint nfound = 0;
    if (copy == NULL) {
        nvn_error(pdpy->platform, EGL_BAD_ALLOC, "out of memory");
        goto done;
    }
    for (EGLint i = 0; attribs != NULL && attribs[i] != EGL_NONE; i += 2) {
        if (attribs[i] == EGL_RECORDABLE_ANDROID) {
            want_rec = attribs[i + 1];
        } else if (attribs[i] == EGL_FRAMEBUFFER_TARGET_ANDROID) {
            want_fbt = attribs[i + 1];
        } else {
            copy[m++] = attribs[i];
            copy[m++] = attribs[i + 1];
        }
    }
    copy[m] = EGL_NONE;
    found = eplConfigListChooseConfigs(pdpy->platform, pdpy->internal_display, priv->configs, copy, &nfound, NULL);
    if (found == NULL) {
        goto done;
    }
    EGLint k = 0;
    for (EGLint i = 0; i < nfound; i++) {
        const struct nvn_config_extra *x = &priv->extra[found[i] - priv->configs->configs];
        if (want_rec != EGL_DONT_CARE && x->recordable != (want_rec != 0)) continue;
        if (want_fbt != EGL_DONT_CARE && x->framebuffer_target != (want_fbt != 0)) continue;
        found[k++] = found[i];
    }
    eplConfigListReturnConfigs(found, k, configs, size, count);
    ok = EGL_TRUE;
done:
    free(found);
    free(copy);
    eplDisplayRelease(pdpy);
    return ok;
}

static EGLBoolean HookGetConfigAttrib(EGLDisplay edpy, EGLConfig config, EGLint attribute, EGLint *value) {
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL) {
        return EGL_FALSE;
    }
    EplImplDisplay *priv = pdpy->priv;
    EGLBoolean ok;
    if (attribute == EGL_RECORDABLE_ANDROID || attribute == EGL_FRAMEBUFFER_TARGET_ANDROID) {
        EGLint idx = eplConfigListFindIndex(priv->configs, config);
        if (idx < 0) {
            eplSetError(pdpy->platform, EGL_BAD_CONFIG, "invalid EGLConfig %p", config);
            ok = EGL_FALSE;
        } else {
            if (value != NULL) {
                *value = attribute == EGL_RECORDABLE_ANDROID ? priv->extra[idx].recordable
                                                             : priv->extra[idx].framebuffer_target;
            }
            ok = EGL_TRUE;
        }
    } else {
        ok = eplConfigListGetAttribute(pdpy->platform, pdpy->internal_display, priv->configs, config, attribute, value);
    }
    eplDisplayRelease(pdpy);
    return ok;
}

/* EGL_ANDROID_image_native_buffer: EGL_NATIVE_BUFFER_ANDROID + ANativeWindowBuffer* -> EGL_LINUX_DMA_BUF_EXT
 * with an explicit modifier (Mesa dri2_create_image_android_native_buffer). Everything else goes to the
 * driver unchanged on the internal display. */
static int image_hook_calls;

static EGLImage create_native_buffer_image(EplDisplay *pdpy, EGLContext ctx, EGLClientBuffer buffer,
                                           const EGLAttrib *attribs) {
    EplPlatformData *plat = pdpy->platform;
    if (ctx != EGL_NO_CONTEXT) {
        nvn_error(plat, EGL_BAD_CONTEXT, "eglCreateImage(EGL_NATIVE_BUFFER_ANDROID): the context must be "
                  "EGL_NO_CONTEXT (EGL_ANDROID_image_native_buffer)");
        return EGL_NO_IMAGE;
    }
    EGLAttrib protected_content = EGL_FALSE;
    for (int i = 0; attribs != NULL && attribs[i] != EGL_NONE; i += 2) {
        if (attribs[i] == EGL_IMAGE_PRESERVED_KHR) {
            /* a dma-buf import always refers to the buffer's existing contents */
        } else if (attribs[i] == EGL_PROTECTED_CONTENT_EXT) {
            protected_content = attribs[i + 1];
        } else {
            nvn_error(plat, EGL_BAD_PARAMETER, "eglCreateImage(EGL_NATIVE_BUFFER_ANDROID): attribute 0x%04x is not "
                      "supported", (unsigned)attribs[i]);
            return EGL_NO_IMAGE;
        }
    }
    struct nvn_dmabuf d;
    char why[512];
    if (nvn_gralloc_decode((const ANativeWindowBuffer *)buffer, &d, why, sizeof(why)) != 0) {
        nvn_error(plat, EGL_BAD_PARAMETER, "eglCreateImage(EGL_NATIVE_BUFFER_ANDROID): %s", why);
        return EGL_NO_IMAGE;
    }
    EGLAttrib a[] = {
        EGL_WIDTH, d.width,
        EGL_HEIGHT, d.height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)d.fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, d.fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLAttrib)d.offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLAttrib)d.stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLAttrib)(d.modifier & 0xffffffffu),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLAttrib)(d.modifier >> 32),
        EGL_NONE, EGL_NONE, EGL_NONE,
    };
    if (protected_content) {
        a[16] = EGL_PROTECTED_CONTENT_EXT;
        a[17] = EGL_TRUE;
    }
    EGLImage img = plat->priv->egl.CreateImage(pdpy->internal_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    if (nvn_debug_on()) {
        char fcc[5];
        const ANativeWindowBuffer *b = (const ANativeWindowBuffer *)buffer;
        nvn_log(NVN_LOG_DEBUG, "image_native_buffer %p: ANWB %dx%d stride %d format %d usage 0x%llx -> dma-buf fd %d "
                "fourcc %s %dx%d pitch %u offset %u modifier 0x%016llx%s -> image %p", (const void *)b, b->width,
                b->height, b->stride, b->format, (unsigned long long)b->usage, d.fd, nvn_fourcc_str(d.fourcc, fcc),
                d.width, d.height, d.stride, d.offset, (unsigned long long)d.modifier,
                protected_content ? " protected" : "", img);
    }
    if (img == EGL_NO_IMAGE) {
        char fcc[5];
        EGLint e = plat->egl.GetError();
        nvn_error(plat, e == EGL_SUCCESS ? EGL_BAD_PARAMETER : e,
                  "eglCreateImage(EGL_NATIVE_BUFFER_ANDROID): the driver refused the dma-buf (fourcc %s, %dx%d, "
                  "stride %u, offset %u, modifier 0x%016llx)", nvn_fourcc_str(d.fourcc, fcc), d.width, d.height,
                  d.stride, d.offset, (unsigned long long)d.modifier);
    }
    return img;
}

static EGLImage HookCreateImage(EGLDisplay edpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                const EGLAttrib *attribs) {
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL) {
        return EGL_NO_IMAGE;
    }
    if (__atomic_fetch_add(&image_hook_calls, 1, __ATOMIC_RELAXED) == 0) {
        nvn_log(NVN_LOG_DEBUG, "eglCreateImage hook reached (target 0x%04x)", target);
    }
    EGLImage img;
    if (target == EGL_NATIVE_BUFFER_ANDROID) {
        img = create_native_buffer_image(pdpy, ctx, buffer, attribs);
    } else {
        img = pdpy->platform->priv->egl.CreateImage(pdpy->internal_display, ctx, target, buffer, attribs);
    }
    eplDisplayRelease(pdpy);
    return img;
}

static EGLImageKHR HookCreateImageKHR(EGLDisplay edpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
                                      const EGLint *attribs) {
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL) {
        return EGL_NO_IMAGE_KHR;
    }
    if (__atomic_fetch_add(&image_hook_calls, 1, __ATOMIC_RELAXED) == 0) {
        nvn_log(NVN_LOG_DEBUG, "eglCreateImageKHR hook reached (target 0x%04x)", target);
    }
    EGLImageKHR img;
    if (target == EGL_NATIVE_BUFFER_ANDROID) {
        EGLint n = eplCountAttribs32(attribs);
        EGLAttrib *a = malloc(((size_t)n + 1) * sizeof(EGLAttrib));
        if (a == NULL) {
            nvn_error(pdpy->platform, EGL_BAD_ALLOC, "out of memory");
            eplDisplayRelease(pdpy);
            return EGL_NO_IMAGE_KHR;
        }
        for (EGLint i = 0; i < n; i++) a[i] = attribs[i];
        a[n] = EGL_NONE;
        img = create_native_buffer_image(pdpy, ctx, buffer, a);
        free(a);
    } else {
        img = pdpy->platform->priv->egl.CreateImageKHR(pdpy->internal_display, ctx, target, buffer, attribs);
    }
    eplDisplayRelease(pdpy);
    return img;
}

static void *nvnGetHookFunction(EplPlatformData *plat, const char *name) {
    (void)plat;
    if (strcmp(name, "eglChooseConfig") == 0) return (void *)HookChooseConfig;
    if (strcmp(name, "eglGetConfigAttrib") == 0) return (void *)HookGetConfigAttrib;
    if (strcmp(name, "eglQueryString") == 0) return (void *)HookQueryString;
    if (strcmp(name, "eglCreateImageKHR") == 0) return (void *)HookCreateImageKHR;
    if (strcmp(name, "eglCreateImage") == 0) return (void *)HookCreateImage;
    return NULL;
}

static const EplImplFuncs NVN_IMPL_FUNCS = {
    .CleanupPlatform = nvnCleanupPlatform,
    .QueryString = nvnQueryString,
    .GetHookFunction = nvnGetHookFunction,
    .GetPlatformDisplay = nvnGetPlatformDisplay,
    .CleanupDisplay = nvnCleanupDisplay,
    .InitializeDisplay = nvnInitializeDisplay,
    .TerminateDisplay = nvnTerminateDisplay,
    .CreateWindowSurface = nvnCreateWindowSurface,
    .DestroySurface = nvnDestroySurface,
    .SwapBuffers = nvnSwapBuffers,
    .SwapInterval = nvnSwapInterval,
    .QuerySurface = nvnQuerySurface,
};

/* ---------------- load ---------------- */

PUBLIC EGLBoolean loadEGLExternalPlatform(int major, int minor, const EGLExtDriver *driver, EGLExtPlatform *extplatform) {
    /* Loud version checks: the driver's platform interface must have our major (it asks 1.3 on 615 and accepts a
     * 1.2 library: NV-EGL-CONTRACT §2.2); the NVX interface must be 0.x with x >= 2. */
    if (major != NVN_PLATFORM_MAJOR || !EGL_EXTERNAL_PLATFORM_VERSION_CMP(major, minor,
                                                                         EGL_EXTERNAL_PLATFORM_VERSION_MAJOR,
                                                                         EGL_EXTERNAL_PLATFORM_VERSION_MINOR)) {
        nvn_log(NVN_LOG_ERROR, "refusing to load: the driver asks for external platform interface %d.%d, this library "
                "implements %d.%d (major must match, driver minor >= ours)", major, minor,
                EGL_EXTERNAL_PLATFORM_VERSION_MAJOR, EGL_EXTERNAL_PLATFORM_VERSION_MINOR);
        return EGL_FALSE;
    }
    EplPlatformData *plat = eplPlatformBaseAllocate(major, minor, driver, extplatform, EGL_PLATFORM_ANDROID_KHR,
                                                    &NVN_IMPL_FUNCS, sizeof(EplImplPlatform));
    if (plat == NULL) {
        nvn_log(NVN_LOG_ERROR, "refusing to load: the platform base could not resolve the driver's core EGL "
                "functions through EGLExtDriver::getProcAddress");
        return EGL_FALSE;
    }
    pfn_eglPlatformGetVersionNVX getver = driver->getProcAddress("eglPlatformGetVersionNVX");
    if (getver == NULL) {
        nvn_log(NVN_LOG_ERROR, "refusing to load: the driver has no eglPlatformGetVersionNVX (platform surface "
                "interface needs driver 560 or later)");
        eplPlatformBaseInitFail(plat);
        return EGL_FALSE;
    }
    plat->priv->nvx_version = getver();
    if (!EGL_PLATFORM_SURFACE_INTERFACE_CHECK_VERSION(plat->priv->nvx_version, NVN_NVX_MIN_MINOR)) {
        nvn_log(NVN_LOG_ERROR, "refusing to load: the driver's platform surface interface is %d.%d; this library is "
                "written for %d.x with x >= %d", EGL_PLATFORM_SURFACE_INTERFACE_GET_MAJOR_VERSION(plat->priv->nvx_version),
                EGL_PLATFORM_SURFACE_INTERFACE_GET_MINOR_VERSION(plat->priv->nvx_version),
                EGL_PLATFORM_SURFACE_INTERFACE_MAJOR_VERSION, NVN_NVX_MIN_MINOR);
        eplPlatformBaseInitFail(plat);
        return EGL_FALSE;
    }
#define NEED(dst, name)                                                                                    \
    do {                                                                                                   \
        *(void **)&(dst) = driver->getProcAddress(name);                                                   \
        if ((dst) == NULL) {                                                                               \
            nvn_log(NVN_LOG_ERROR, "refusing to load: EGLExtDriver::getProcAddress(\"%s\") is NULL", name); \
            eplPlatformBaseInitFail(plat);                                                                 \
            return EGL_FALSE;                                                                              \
        }                                                                                                  \
    } while (0)
    NEED(plat->priv->nvx.ImportColorBuffer, "eglPlatformImportColorBufferNVX");
    NEED(plat->priv->nvx.FreeColorBuffer, "eglPlatformFreeColorBufferNVX");
    NEED(plat->priv->nvx.CreateSurface, "eglPlatformCreateSurfaceNVX");
    NEED(plat->priv->nvx.SetColorBuffers, "eglPlatformSetColorBuffersNVX");
    NEED(plat->priv->nvx.GetConfigAttrib, "eglPlatformGetConfigAttribNVX");
    NEED(plat->priv->egl.CreateSync, "eglCreateSync");
    NEED(plat->priv->egl.DestroySync, "eglDestroySync");
    NEED(plat->priv->egl.WaitSync, "eglWaitSync");
    NEED(plat->priv->egl.DupNativeFenceFD, "eglDupNativeFenceFDANDROID");
    NEED(plat->priv->egl.CreateImageKHR, "eglCreateImageKHR");
    NEED(plat->priv->egl.CreateImage, "eglCreateImage");
    NEED(plat->priv->egl.QueryDmaBufFormats, "eglQueryDmaBufFormatsEXT");
    NEED(plat->priv->egl.QueryDmaBufModifiers, "eglQueryDmaBufModifiersEXT");
    NEED(plat->priv->egl.Flush, "glFlush");
#undef NEED
    eplPlatformBaseInitFinish(plat);
    nvn_log(NVN_LOG_INFO, "loaded: external platform interface %d.%d (library %d.%d), NVX 0x%x, EGL_PLATFORM_ANDROID_KHR",
            major, minor, EGL_EXTERNAL_PLATFORM_VERSION_MAJOR, EGL_EXTERNAL_PLATFORM_VERSION_MINOR,
            (unsigned)plat->priv->nvx_version);
    return EGL_TRUE;
}
