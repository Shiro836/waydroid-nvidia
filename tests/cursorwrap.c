// Guest-side reproduction of hwcomposer's cursor-layer shm conversion
// (egl-tools.cpp egl_render_to_pixels): AHB shaped like the pointer sprite
// (CPU_WRITE|GPU_SAMPLED, 68x68 RGBA8888) -> eglCreateImageKHR with and
// without EGL_IMAGE_PRESERVED_KHR -> TEXTURE_2D -> FBO -> glReadPixels.
// Prints the failing step; EGL debug callback prints ANGLE's own message.
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <stdio.h>
#include <string.h>

typedef void (*PFNEGLDEBUGCALLBACK)(EGLenum error, const char *command, EGLint messageType, EGLLabelKHR threadLabel, EGLLabelKHR objectLabel, const char *message);
typedef EGLint (*PFNEGLDEBUGMESSAGECONTROL)(PFNEGLDEBUGCALLBACK callback, const EGLAttrib *attrib_list);
static void dbg_cb(EGLenum error, const char *command, EGLint messageType, EGLLabelKHR t, EGLLabelKHR o, const char *message) {
    printf("EGL-DEBUG [0x%x] %s: %s\n", error, command, message ? message : "(null)");
}

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC pGetNCB;
static PFNEGLCREATEIMAGEKHRPROC pCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pTargetTex2D;

static int run_pass(EGLDisplay dpy, int preserved, uint64_t usage, const char *label) {
    printf("--- pass '%s' preserved=%d usage=0x%llx\n", label, preserved, (unsigned long long)usage);
    AHardwareBuffer_Desc desc = {
        .width = 68, .height = 68, .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = usage,
    };
    AHardwareBuffer *ahb = NULL;
    if (AHardwareBuffer_allocate(&desc, &ahb) != 0 || !ahb) {
        printf("FAIL AHardwareBuffer_allocate\n");
        return 1;
    }
    AHardwareBuffer_Desc got; AHardwareBuffer_describe(ahb, &got);
    printf("alloc ok stride=%u\n", got.stride);

    // fill with pattern if CPU-writable
    if (usage & AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN) {
        void *map = NULL;
        if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &map) == 0 && map) {
            for (unsigned y = 0; y < 68; y++)
                memset((char *)map + (size_t)y * got.stride * 4, 0xA5, 68 * 4);
            AHardwareBuffer_unlock(ahb, NULL);
            printf("cpu fill ok\n");
        } else {
            printf("WARN cpu lock failed\n");
        }
    }

    EGLint attrs_pres[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLint attrs_plain[] = { EGL_NONE };
    EGLClientBuffer cb = pGetNCB(ahb);
    EGLImageKHR img = pCreateImage(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                   cb, preserved ? attrs_pres : attrs_plain);
    if (img == EGL_NO_IMAGE_KHR) {
        printf("FAIL eglCreateImageKHR err=0x%x\n", eglGetError());
        AHardwareBuffer_release(ahb);
        return 1;
    }
    printf("eglCreateImageKHR ok\n");

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    pTargetTex2D(GL_TEXTURE_2D, img);
    GLenum e = glGetError();
    printf("glEGLImageTargetTexture2DOES: 0x%x\n", e);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    printf("fbo status: 0x%x\n", st);

    unsigned char px[16] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    e = glGetError();
    printf("glReadPixels: 0x%x px=%02x%02x%02x%02x\n", e, px[0], px[1], px[2], px[3]);

    glDeleteTextures(1, &tex);
    pDestroyImage(dpy, img);
    AHardwareBuffer_release(ahb);
    printf("pass '%s' done\n", label);
    return 0;
}

int main(void) {
    PFNEGLDEBUGMESSAGECONTROL dbgctl = (PFNEGLDEBUGMESSAGECONTROL)eglGetProcAddress("eglDebugMessageControlKHR");
    if (dbgctl) {
        static const EGLAttrib dbg_attribs[] = {0x33B9, EGL_TRUE, 0x33BA, EGL_TRUE, EGL_NONE};
        dbgctl(dbg_cb, dbg_attribs);
    }
    pGetNCB = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    pCreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    pDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    pTargetTex2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, NULL, NULL)) { printf("FAIL eglInitialize\n"); return 1; }
    printf("EGL: %s / %s\n", eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));

    // exactly egl-tools.cpp egl_init: ES2 + pbuffer + FBO
    EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                          EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                          EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                          EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    EGLint pb_attr[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (!eglMakeCurrent(dpy, pb, pb, ctx)) { printf("FAIL eglMakeCurrent 0x%x\n", eglGetError()); return 1; }
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // sprite-like usage (Surface::lock software canvas): CPU RW + GPU texture
    uint64_t sprite = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                      AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    run_pass(dpy, 0, sprite, "sprite-plain");
    run_pass(dpy, 1, sprite, "sprite-preserved");
    // GPU-rendered variant (in case sprite is drawn by HWUI)
    uint64_t gpu = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                   AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
    run_pass(dpy, 0, gpu, "gpu-plain");
    run_pass(dpy, 1, gpu, "gpu-preserved");
    printf("ALL DONE\n");
    return 0;
}
