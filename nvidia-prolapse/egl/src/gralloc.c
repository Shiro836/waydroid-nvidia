/* ANativeWindowBuffer -> one dma-buf plane, for the driver's single-plane imports
 * (eglPlatformImportColorBufferNVX, EGL_LINUX_DMA_BUF_EXT).
 *
 * The buffer handle is minigbm's struct cros_gralloc_handle; the layout header is shared with the
 * Vulkan HAL (the driver's hal/include/minigbm/cros_gralloc_handle.h, pinned minigbm 16844ba),
 * and so is its validity check (cros_gralloc_handle_mismatch). */
#include <stdio.h>
#include <string.h>

#include <drm_fourcc.h>
#include <cros_gralloc_handle.h>

#include "nvn.h"

const char *nvn_fourcc_str(uint32_t f, char buf[5]) {
    for (int i = 0; i < 4; i++) {
        char c = (char)((f >> (8 * i)) & 0xff);
        buf[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    buf[4] = '\0';
    return buf;
}

/* minigbm's HAL -> DRM mapping (build/minigbm cros_gralloc/cros_gralloc_helpers.cc:31-77), inverted for
 * the formats an EGL window config can carry. */
int nvn_hal_format_for_fourcc(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_ABGR8888: return NVN_HAL_PIXEL_FORMAT_RGBA_8888;
    case DRM_FORMAT_XBGR8888: return NVN_HAL_PIXEL_FORMAT_RGBX_8888;
    case DRM_FORMAT_RGB565: return NVN_HAL_PIXEL_FORMAT_RGB_565;
    case DRM_FORMAT_ARGB8888: return NVN_HAL_PIXEL_FORMAT_BGRA_8888;
    case DRM_FORMAT_ABGR16161616F: return NVN_HAL_PIXEL_FORMAT_RGBA_FP16;
    case DRM_FORMAT_ABGR2101010: return NVN_HAL_PIXEL_FORMAT_RGBA_1010102;
    default: return 0;
    }
}

int nvn_gralloc_decode(const ANativeWindowBuffer *b, struct nvn_dmabuf *out, char *why, size_t whyn) {
    if (b == NULL || b->common.magic != (int)NVN_ANDROID_NATIVE_BUFFER_MAGIC ||
        b->common.version != (int)sizeof(ANativeWindowBuffer)) {
        snprintf(why, whyn, "%p is not an ANativeWindowBuffer (magic 0x%08x, version %d; expected 0x%08x, %zu)",
                 (const void *)b, b ? (unsigned)b->common.magic : 0u, b ? b->common.version : 0,
                 (unsigned)NVN_ANDROID_NATIVE_BUFFER_MAGIC, sizeof(ANativeWindowBuffer));
        return -1;
    }
    const char *bad = cros_gralloc_handle_mismatch(b->handle);
    if (bad != NULL) {
        snprintf(why, whyn, "buffer %p: %s. Remedy: the guest must run minigbm gralloc "
                 "(ro.hardware.gralloc=minigbm_gbm_mesa) with the native backend", (const void *)b, bad);
        return -1;
    }
    const struct cros_gralloc_handle *g = (const struct cros_gralloc_handle *)b->handle;
    char fcc[5];
    if (g->num_planes != 1 || g->fds[0] < 0) {
        snprintf(why, whyn, "buffer %p (fourcc %s, %ux%u, droid_format %d) has %u planes%s: only single-plane "
                 "buffers are implemented (multi-planar/YUV window and image buffers are not)",
                 (const void *)b, nvn_fourcc_str(g->format, fcc), g->width, g->height, g->droid_format,
                 g->num_planes, g->fds[0] < 0 ? " and no fd" : "");
        return -1;
    }
    if (g->format_modifier == DRM_FORMAT_MOD_INVALID) {
        snprintf(why, whyn, "buffer %p (fourcc %s) carries DRM_FORMAT_MOD_INVALID (implicit layout), which cannot "
                 "be imported with an explicit modifier", (const void *)b, nvn_fourcc_str(g->format, fcc));
        return -1;
    }
    if (b->width <= 0 || b->height <= 0 || (uint32_t)b->width > g->width || (uint32_t)b->height > g->height) {
        snprintf(why, whyn, "buffer %p is %dx%d but its gralloc allocation is %ux%u", (const void *)b, b->width,
                 b->height, g->width, g->height);
        return -1;
    }
    out->fd = g->fds[0];
    out->width = b->width;
    out->height = b->height;
    out->fourcc = g->format;
    out->stride = g->strides[0];
    out->offset = g->offsets[0];
    out->modifier = g->format_modifier;
    return 0;
}
