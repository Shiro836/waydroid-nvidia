/*
 * Single source of truth for DRM format FOURCC definitions and whitelisting
 * shared between guest minigbm vtest_wrapper and host virglrenderer vtest_gpu_alloc.
 *
 * Copyright 2026 waydroid-nvidia project
 * SPDX-License-Identifier: MIT
 */

#ifndef VTEST_ALLOC_FORMATS_H
#define VTEST_ALLOC_FORMATS_H

#include <stdint.h>

#define VTEST_FOURCC(a, b, c, d) \
   ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Formats supported by host virglrenderer GPU/CPU allocators */
#define VTEST_FORMAT_R8            VTEST_FOURCC('R', '8', ' ', ' ')
#define VTEST_FORMAT_RGB565        VTEST_FOURCC('R', 'G', '1', '6')
#define VTEST_FORMAT_XRGB8888      VTEST_FOURCC('X', 'R', '2', '4')
#define VTEST_FORMAT_ARGB8888      VTEST_FOURCC('A', 'R', '2', '4')
#define VTEST_FORMAT_XBGR8888      VTEST_FOURCC('X', 'B', '2', '4')
#define VTEST_FORMAT_ABGR8888      VTEST_FOURCC('A', 'B', '2', '4')
#define VTEST_FORMAT_ABGR2101010   VTEST_FOURCC('A', 'B', '3', '0')
#define VTEST_FORMAT_XBGR2101010   VTEST_FOURCC('X', 'B', '3', '0')
#define VTEST_FORMAT_ABGR16161616F VTEST_FOURCC('A', 'B', '4', 'H')
#define VTEST_FORMAT_NV12          VTEST_FOURCC('N', 'V', '1', '2')
#define VTEST_FORMAT_P010          VTEST_FOURCC('P', '0', '1', '0')

static inline int
vtest_is_supported_drm_format(uint32_t drm_format)
{
   switch (drm_format) {
   case VTEST_FORMAT_R8:
   case VTEST_FORMAT_RGB565:
   case VTEST_FORMAT_XRGB8888:
   case VTEST_FORMAT_ARGB8888:
   case VTEST_FORMAT_XBGR8888:
   case VTEST_FORMAT_ABGR8888:
   case VTEST_FORMAT_ABGR2101010:
   case VTEST_FORMAT_XBGR2101010:
   case VTEST_FORMAT_ABGR16161616F:
   case VTEST_FORMAT_NV12:
   case VTEST_FORMAT_P010:
      return 1;
   default:
      return 0;
   }
}

#endif /* VTEST_ALLOC_FORMATS_H */
