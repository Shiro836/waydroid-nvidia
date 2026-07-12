// Cross-vendor EGL dmabuf import probe:
// allocate a LINEAR ARGB8888 gbm bo on one DRM node, fill it, export dmabuf,
// import on another node's EGL (device platform), try binding as
// GL_TEXTURE_2D and GL_TEXTURE_EXTERNAL_OES, render/sample and verify.
//
// usage: eglimport /dev/dri/renderD129 /dev/dri/renderD128
//        (alloc node, import node)
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define W 64
#define H 64

static PFNEGLCREATEIMAGEKHRPROC pCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTex2D;

static const char *vs_src =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";

static char fs_src[256];

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log);
               fprintf(stderr, "shader: %s\n", log); exit(1); }
    return s;
}

static int try_target(EGLDisplay dpy, EGLImageKHR img, GLenum target,
                      const char *sampler_type, const char *name) {
    while (glGetError() != GL_NO_ERROR);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(target, tex);
    pImageTargetTex2D(target, (GLeglImageOES)img);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("%s: BIND FAILED (glError 0x%x)\n", name, err);
        glDeleteTextures(1, &tex);
        return -1;
    }
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // render to an ordinary FBO, sampling from the imported texture
    GLuint dst, fbo;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("%s: fbo incomplete\n", name); return -1;
    }
    snprintf(fs_src, sizeof(fs_src),
        "#extension GL_OES_EGL_image_external : enable\n"
        "precision mediump float;\n"
        "varying vec2 uv;\n"
        "uniform %s tex;\n"
        "void main() { gl_FragColor = texture2D(tex, uv); }\n", sampler_type);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    GLint ok; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { printf("%s: link failed\n", name); return -1; }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(target, tex);
    static const float quad[] = {-1,-1, 3,-1, -1,3};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, W, H);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char px[4] = {0};
    glReadPixels(W/2, H/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    err = glGetError();
    // buffer was filled with B=0x40 G=0x80 R=0xc0 A=0xff (ARGB little-endian)
    printf("%s: bind OK, center pixel RGBA = %02x %02x %02x %02x (%s, glErr 0x%x)\n",
           name, px[0], px[1], px[2], px[3],
           (px[0] == 0xc0 && px[1] == 0x80 && px[2] == 0x40) ? "CONTENT OK" : "CONTENT WRONG",
           err);
    return (px[0] == 0xc0 && px[1] == 0x80 && px[2] == 0x40) ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *alloc_path = argc > 1 ? argv[1] : "/dev/dri/renderD129";
    const char *import_path = argc > 2 ? argv[2] : "/dev/dri/renderD128";

    // --- allocate + fill linear bo on the alloc node ---
    int afd = open(alloc_path, O_RDWR | O_CLOEXEC);
    if (afd < 0) { perror(alloc_path); return 1; }
    struct gbm_device *gbm = gbm_create_device(afd);
    if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }
    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ARGB8888,
                                      GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!bo) { fprintf(stderr, "gbm_bo_create failed\n"); return 1; }
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t mod = gbm_bo_get_modifier(bo);
    int dmabuf = gbm_bo_get_fd(bo);
    printf("alloc: %s linear bo %dx%d stride=%u modifier=0x%llx fd=%d\n",
           alloc_path, W, H, stride, (unsigned long long)mod, dmabuf);

    { // fill via mmap (linear, so direct write is fine)
        uint32_t ms; void *mp;
        void *map = gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &ms, &mp);
        if (!map) { fprintf(stderr, "gbm_bo_map failed\n"); return 1; }
        for (int y = 0; y < H; y++) {
            uint32_t *row = (uint32_t *)((char *)map + y * ms);
            for (int x = 0; x < W; x++) row[x] = 0xffc08040; // A R G B
        }
        gbm_bo_unmap(bo, mp);
    }

    // --- pick the EGL device matching import node ---
    PFNEGLQUERYDEVICESEXTPROC qdevs =
        (void *)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC qdevstr =
        (void *)eglGetProcAddress("eglQueryDeviceStringEXT");
    EGLDeviceEXT devs[16]; EGLint ndev = 0;
    qdevs(16, devs, &ndev);
    EGLDisplay dpy = EGL_NO_DISPLAY;
    for (int i = 0; i < ndev; i++) {
        const char *node = qdevstr(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        const char *drm = qdevstr(devs[i], EGL_DRM_DEVICE_FILE_EXT);
        printf("egl device %d: render=%s drm=%s\n", i, node ? node : "-", drm ? drm : "-");
        if (!node || strcmp(node, import_path))
            continue;
        EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
        if (d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL)) { dpy = d; break; }
    }
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no initializable EGL device for %s\n", import_path); return 1; }
    printf("import EGL: %s / %s\n", eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig cfg; EGLint ncfg = 0;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg);
    static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, ncfg ? cfg : NULL, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed 0x%x\n", eglGetError()); return 1; }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    printf("GL: %s / %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));

    pCreateImage = (void *)eglGetProcAddress("eglCreateImageKHR");
    pDestroyImage = (void *)eglGetProcAddress("eglDestroyImageKHR");
    pImageTargetTex2D = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!pCreateImage || !pImageTargetTex2D) {
        fprintf(stderr, "missing EGLImage entrypoints\n"); return 1;
    }

    // --- import: with explicit LINEAR modifier, and as implicit ---
    for (int pass = 0; pass < 2; pass++) {
        EGLint attrs[64]; int a = 0;
        attrs[a++] = EGL_WIDTH; attrs[a++] = W;
        attrs[a++] = EGL_HEIGHT; attrs[a++] = H;
        attrs[a++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[a++] = DRM_FORMAT_ARGB8888;
        attrs[a++] = EGL_DMA_BUF_PLANE0_FD_EXT; attrs[a++] = dmabuf;
        attrs[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[a++] = 0;
        attrs[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; attrs[a++] = (EGLint)stride;
        if (pass == 0) {
            /* bo was created USE_LINEAR; amdgpu may still report INVALID */
            attrs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attrs[a++] = 0; /* DRM_FORMAT_MOD_LINEAR */
            attrs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attrs[a++] = 0;
        }
        attrs[a++] = EGL_NONE;
        EGLImageKHR img = pCreateImage(dpy, EGL_NO_CONTEXT,
                                            EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
        const char *label = pass == 0 ? "explicit-linear" : "implicit-mod";
        if (img == EGL_NO_IMAGE_KHR) {
            printf("[%s] eglCreateImageKHR FAILED 0x%x\n", label, eglGetError());
            continue;
        }
        printf("[%s] eglCreateImageKHR OK\n", label);
        char n1[64], n2[64];
        snprintf(n1, sizeof(n1), "[%s] GL_TEXTURE_2D", label);
        snprintf(n2, sizeof(n2), "[%s] GL_TEXTURE_EXTERNAL_OES", label);
        try_target(dpy, img, GL_TEXTURE_2D, "sampler2D", n1);
        try_target(dpy, img, GL_TEXTURE_EXTERNAL_OES, "samplerExternalOES", n2);
        pDestroyImage(dpy, img);
    }
    return 0;
}
