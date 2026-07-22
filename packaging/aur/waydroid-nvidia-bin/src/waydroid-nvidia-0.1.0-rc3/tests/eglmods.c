// Print eglQueryDmaBufModifiersEXT for ARGB8888 on the NVIDIA EGL device:
// which DRM modifiers KWin could import, and which are external-only.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    PFNEGLQUERYDEVICESEXTPROC qdevs = (void *)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC qdevstr = (void *)eglGetProcAddress("eglQueryDeviceStringEXT");
    EGLDeviceEXT devs[16]; EGLint ndev = 0;
    qdevs(16, devs, &ndev);
    EGLDisplay dpy = EGL_NO_DISPLAY;
    for (int i = 0; i < ndev; i++) {
        const char *node = qdevstr(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        if (!node || strcmp(node, "/dev/dri/renderD128")) continue;
        EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
        if (d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL)) { dpy = d; break; }
    }
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no NVIDIA EGL\n"); return 1; }

    PFNEGLQUERYDMABUFMODIFIERSEXTPROC qmods =
        (void *)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    if (!qmods) { fprintf(stderr, "no eglQueryDmaBufModifiersEXT\n"); return 1; }

    EGLuint64KHR mods[128]; EGLBoolean external[128]; EGLint n = 0;
    if (!qmods(dpy, 0x34325241 /* AR24 */, 128, mods, external, &n)) {
        fprintf(stderr, "query failed\n"); return 1;
    }
    printf("AR24: %d modifiers\n", n);
    for (int i = 0; i < n; i++)
        printf("  0x%016llx %s\n", (unsigned long long)mods[i],
               external[i] ? "EXTERNAL-ONLY" : "renderable/samplable");
    return 0;
}
