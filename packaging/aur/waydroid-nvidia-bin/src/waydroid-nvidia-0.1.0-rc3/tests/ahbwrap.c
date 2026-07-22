// Guest-side reproduction of SF RenderEngine's output-buffer wrap:
// AHardwareBuffer(GPU_FRAMEBUFFER|GPU_SAMPLED) -> eglCreateImageKHR ->
// glEGLImageTargetTexture2DOES, printing the failing step and error codes.
// Build with NDK, run inside the Waydroid container.
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <stdio.h>
#include <android/log.h>

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC pGetNCB;
static PFNEGLCREATEIMAGEKHRPROC pCreateImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTex2D;
typedef void (*PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC2)(GLenum, GLeglImageOES, const GLint *);
static PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC2 pImageTargetTexStorage;

typedef void (*PFNEGLDEBUGCALLBACK)(EGLenum error, const char *command, EGLint messageType, EGLLabelKHR threadLabel, EGLLabelKHR objectLabel, const char *message);
typedef EGLint (*PFNEGLDEBUGMESSAGECONTROL)(PFNEGLDEBUGCALLBACK callback, const EGLAttrib *attrib_list);
static void dbg_cb(EGLenum error, const char *command, EGLint messageType, EGLLabelKHR t, EGLLabelKHR o, const char *message) {
    printf("EGL-DEBUG [0x%x] %s: %s\n", error, command, message ? message : "(null)");
}

int main(void) {
    PFNEGLDEBUGMESSAGECONTROL dbgctl = (PFNEGLDEBUGMESSAGECONTROL)eglGetProcAddress("eglDebugMessageControlKHR");
    if (dbgctl) {
        static const EGLAttrib dbg_attribs[] = {0x33B9 /*EGL_DEBUG_MSG_ERROR_KHR*/, EGL_TRUE, 0x33BA, EGL_TRUE, EGL_NONE};
        dbgctl(dbg_cb, dbg_attribs);
    }
    pGetNCB = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    pCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    pImageTargetTex2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    pImageTargetTexStorage = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC2)eglGetProcAddress("glEGLImageTargetTexStorageEXT");
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, NULL, NULL)) {
        printf("FAIL eglInitialize 0x%x\n", eglGetError());
        return 1;
    }
    printf("EGL: %s / %s\n", eglQueryString(dpy, EGL_VENDOR),
           eglQueryString(dpy, EGL_VERSION));

    EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig cfg; EGLint ncfg = 0;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg);
    EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) { printf("FAIL eglCreateContext 0x%x\n", eglGetError()); return 1; }
    EGLint pb_attr[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    EGLSurface pb = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (!eglMakeCurrent(dpy, pb, pb, ctx)) { printf("FAIL eglMakeCurrent 0x%x\n", eglGetError()); return 1; }
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    static const struct { const char *label; uint32_t w, h; uint64_t usage; } passes[] = {
        {"sampled", 512, 512, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE},
        {"sampled|cpuW", 512, 512,
         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN},
        {"sampled|cpuR", 512, 512,
         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN},
        {"fb|sampled|cpuW (SF capture)", 512, 512,
         AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
         AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN},
        {"fb|sampled|cpuR", 512, 512,
         AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
         AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN},
        {"sampled|cpuW rarely", 512, 512,
         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY},
    };
    for (int pass = 0; pass < 6; pass++) {
        AHardwareBuffer_Desc desc = {
            .width = passes[pass].w,
            .height = passes[pass].h,
            .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            .usage = passes[pass].usage,
        };
        const char *label = passes[pass].label;
        __android_log_print(ANDROID_LOG_ERROR, "MESA", "==== PASS: %s ====", label);
        AHardwareBuffer *ahb = NULL;
        int r = AHardwareBuffer_allocate(&desc, &ahb);
        if (r) { printf("[%s] FAIL AHardwareBuffer_allocate %d\n", label, r); continue; }
        printf("[%s] AHB allocated\n", label);

        EGLClientBuffer cb = pGetNCB(ahb);
        if (!cb) { printf("[%s] FAIL getNativeClientBuffer\n", label); continue; }

        EGLint img_attr[] = {EGL_NONE};
        EGLImageKHR img = pCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, img_attr);
        if (img == EGL_NO_IMAGE_KHR) {
            printf("[%s] FAIL eglCreateImageKHR 0x%x\n", label, eglGetError());
            continue;
        }
        printf("[%s] EGLImage created\n", label);

        while (glGetError() != GL_NO_ERROR);
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        pImageTargetTex2D(GL_TEXTURE_2D, (GLeglImageOES)img);
        GLenum err = glGetError();
        printf("[%s] TEXTURE_2D bind: %s (0x%x)\n", label,
               err == GL_NO_ERROR ? "OK" : "FAIL", err);
        if (pImageTargetTexStorage) {
            GLuint tex2;
            glGenTextures(1, &tex2);
            glBindTexture(GL_TEXTURE_2D, tex2);
            pImageTargetTexStorage(GL_TEXTURE_2D, (GLeglImageOES)img, NULL);
            err = glGetError();
            printf("[%s] TexStorageEXT bind: %s (0x%x)\n", label,
                   err == GL_NO_ERROR ? "OK" : "FAIL", err);
        }

        // renderability: attach to FBO
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        printf("[%s] FBO status: %s (0x%x)\n", label,
               st == GL_FRAMEBUFFER_COMPLETE ? "COMPLETE" : "INCOMPLETE", st);
        if (st == GL_FRAMEBUFFER_COMPLETE) {
            glClearColor(1, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glFinish();
            printf("[%s] clear+finish OK (glErr 0x%x)\n", label, glGetError());
        }
    }
    printf("DONE\n");
    return 0;
}
