/*
 * C TRANSLATION of the Android 13 native window / native buffer ABI that an EGL driver sees.
 * Source: https://android.googlesource.com/platform/frameworks/native  android13-release
 *         2fe7eddf185ecb63223eeadc58804f9bf6cbd426 (Apache-2.0), pinned clone build/p7/src/fw-native:
 *   libs/nativebase/include/nativebase/nativebase.h   android_native_base_t, ANativeWindowBuffer,
 *                                                      ANDROID_NATIVE_MAKE_CONSTANT, ANDROID_NATIVE_BUFFER_MAGIC
 *   libs/nativewindow/include/system/window.h          struct ANativeWindow (:340-560), ANDROID_NATIVE_WINDOW_MAGIC
 *                                                      (:60), NATIVE_WINDOW_* query/perform/api codes (:62-300),
 *                                                      native_window_* inline helpers (:580-870)
 *   libs/nativewindow/include/vndk/window.h            ANATIVEWINDOW_QUERY_* values (:87-166)
 *   libs/nativewindow/include/apex/window.h            ANATIVEWINDOW_PERFORM_* values (:35-38)
 *   system/core libsystem graphics-base-v1.0.h         HAL_PIXEL_FORMAT_* (same values as the NDK's
 *                                                      AHARDWAREBUFFER_FORMAT_*)
 * Only what an EGL driver and its tests touch is spelled out. The struct layouts are the vendor ABI
 * (an EGL driver receives these pointers from libEGL.so); the static asserts pin LP64 and ILP32.
 * native_handle_t and the gralloc usage bits come from the HAL's vendored headers
 * (the driver's hal/include/android/), shared with vulkan.nvidia.so.
 */
#ifndef NVN_ANDROID_H
#define NVN_ANDROID_H

#include <stddef.h>
#include <stdint.h>
#include <cutils/native_handle.h>
#include <hardware/gralloc_usage.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVN_ANDROID_MAKE_CONSTANT(a, b, c, d) \
    (((unsigned)(a) << 24) | ((unsigned)(b) << 16) | ((unsigned)(c) << 8) | (unsigned)(d))
#define NVN_ANDROID_NATIVE_BUFFER_MAGIC NVN_ANDROID_MAKE_CONSTANT('_', 'b', 'f', 'r')
#define NVN_ANDROID_NATIVE_WINDOW_MAGIC NVN_ANDROID_MAKE_CONSTANT('_', 'w', 'n', 'd')

typedef struct android_native_base_t {
    int magic;
    int version;
    void *reserved[4];
    void (*incRef)(struct android_native_base_t *base);
    void (*decRef)(struct android_native_base_t *base);
} android_native_base_t;

typedef struct ANativeWindowBuffer {
    struct android_native_base_t common;
    int width;
    int height;
    int stride;
    int format;
    int usage_deprecated;
    uintptr_t layerCount;
    void *reserved[1];
    const native_handle_t *handle;
    uint64_t usage;
    void *reserved_proc[8 - (sizeof(uint64_t) / sizeof(void *))];
} ANativeWindowBuffer;

struct ANativeWindow {
    struct android_native_base_t common;
    const uint32_t flags;
    const int minSwapInterval;
    const int maxSwapInterval;
    const float xdpi;
    const float ydpi;
    intptr_t oem[4];
    int (*setSwapInterval)(struct ANativeWindow *window, int interval);
    int (*dequeueBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer **buffer);
    int (*lockBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
    int (*queueBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
    int (*query)(const struct ANativeWindow *window, int what, int *value);
    int (*perform)(struct ANativeWindow *window, int operation, ...);
    int (*cancelBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
    int (*dequeueBuffer)(struct ANativeWindow *window, struct ANativeWindowBuffer **buffer, int *fenceFd);
    int (*queueBuffer)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fenceFd);
    int (*cancelBuffer)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fenceFd);
};

#if UINTPTR_MAX == 0xffffffffffffffffu
_Static_assert(sizeof(android_native_base_t) == 56, "android_native_base_t LP64");
_Static_assert(offsetof(ANativeWindowBuffer, handle) == 96, "ANativeWindowBuffer LP64");
_Static_assert(sizeof(ANativeWindowBuffer) == 168, "ANativeWindowBuffer LP64");
_Static_assert(offsetof(struct ANativeWindow, setSwapInterval) == 112, "ANativeWindow LP64");
_Static_assert(sizeof(struct ANativeWindow) == 192, "ANativeWindow LP64");
#else
_Static_assert(sizeof(android_native_base_t) == 32, "android_native_base_t ILP32");
_Static_assert(offsetof(ANativeWindowBuffer, handle) == 60, "ANativeWindowBuffer ILP32 (uint64_t is 4-aligned)");
_Static_assert(sizeof(ANativeWindowBuffer) == 96, "ANativeWindowBuffer ILP32");
_Static_assert(offsetof(struct ANativeWindow, setSwapInterval) == 68, "ANativeWindow ILP32");
_Static_assert(sizeof(struct ANativeWindow) == 108, "ANativeWindow ILP32");
#endif

/* window.h query codes */
enum {
    NVN_NATIVE_WINDOW_WIDTH = 0,
    NVN_NATIVE_WINDOW_HEIGHT = 1,
    NVN_NATIVE_WINDOW_FORMAT = 2,
    NVN_NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS = 3,
    NVN_NATIVE_WINDOW_DEFAULT_WIDTH = 6,
    NVN_NATIVE_WINDOW_DEFAULT_HEIGHT = 7,
    NVN_NATIVE_WINDOW_IS_VALID = 17,
};
/* window.h perform codes */
enum {
    NVN_NATIVE_WINDOW_SET_BUFFER_COUNT = 4,
    NVN_NATIVE_WINDOW_SET_BUFFERS_FORMAT = 9,
    NVN_NATIVE_WINDOW_API_CONNECT = 13,
    NVN_NATIVE_WINDOW_API_DISCONNECT = 14,
    NVN_NATIVE_WINDOW_SET_BUFFERS_DATASPACE = 19,
    NVN_NATIVE_WINDOW_SET_SURFACE_DAMAGE = 20,
    NVN_NATIVE_WINDOW_SET_USAGE64 = 30,
};
enum { NVN_NATIVE_WINDOW_API_EGL = 1 };

/* HAL pixel formats (graphics-base-v1.0.h / AHARDWAREBUFFER_FORMAT_*) */
enum {
    NVN_HAL_PIXEL_FORMAT_RGBA_8888 = 1,
    NVN_HAL_PIXEL_FORMAT_RGBX_8888 = 2,
    NVN_HAL_PIXEL_FORMAT_RGB_888 = 3,
    NVN_HAL_PIXEL_FORMAT_RGB_565 = 4,
    NVN_HAL_PIXEL_FORMAT_BGRA_8888 = 5,
    NVN_HAL_PIXEL_FORMAT_RGBA_FP16 = 0x16,
    NVN_HAL_PIXEL_FORMAT_RGBA_1010102 = 0x2B,
};

#ifdef __cplusplus
}
#endif
#endif
