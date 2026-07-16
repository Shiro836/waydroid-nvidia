// Does NVIDIA EGL honor EGL_DMA_BUF_PLANE0_PITCH_EXT on LINEAR dmabuf import?
// (NVIDIA forum #364360 claims it is IGNORED and rows are assumed packed at
//  32-byte alignment — causes shear for widths where align32 != real pitch.)
//
// Method: allocate a 512-px-wide LINEAR ARGB8888 bo (real pitch 2048) on the
// alloc node, fill every pixel with a color that encodes its own (x,y):
//   R = x & 0xff, B = x >> 8, G = y * 10
// then EGL-import on the import node three ways and sample back:
//   pass A (control): W=512 pitch=2048  -> must be clean or harness is broken
//   pass B (test)   : W=500 pitch=2048  -> correct pitch; shear => pitch IGNORED
//   pass C (lie)    : W=500 pitch=2000  -> wrong pitch; clean => pitch IGNORED,
//                                          shear => pitch HONORED
// From any mismatching probe we solve pitch_used = (y'*2048 + x'*4) / y.
//
// usage: eglstride [alloc node] [import node]   (defaults renderD129 renderD128)
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
#include <unistd.h>
#include <sys/mman.h>

#define ALLOC_W 512
#define H 16

static PFNEGLCREATEIMAGEKHRPROC pCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTex2D;

static const char *vs_src =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *fs_src =
    "#extension GL_OES_EGL_image_external : enable\n"
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform samplerExternalOES tex;\n"
    "void main() { gl_FragColor = texture2D(tex, uv); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log);
               fprintf(stderr, "shader: %s\n", log); exit(1); }
    return s;
}

// import + render 1:1 into a W x H FBO + probe pixels; returns 0 clean,
// 1 sheared/mismatch, -1 import/bind failure
static int run_pass(EGLDisplay dpy, int dmabuf, int w, int pitch,
                    const char *label) {
    EGLint attrs[] = {
        EGL_WIDTH, w,
        EGL_HEIGHT, H,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0, /* DRM_FORMAT_MOD_LINEAR */
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
        EGL_NONE
    };
    EGLImageKHR img = pCreateImage(dpy, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        printf("[%s] eglCreateImageKHR FAILED 0x%x\n", label, eglGetError());
        return -1;
    }
    while (glGetError() != GL_NO_ERROR);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    pImageTargetTex2D(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("[%s] bind FAILED (glError 0x%x)\n", label, err);
        glDeleteTextures(1, &tex);
        pDestroyImage(dpy, img);
        return -1;
    }
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint dst, fbo;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, dst, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("[%s] fbo incomplete\n", label);
        return -1;
    }
    glUseProgram(0); /* program bound by caller stays; rebind below */
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    static const float quad[] = {-1,-1, 3,-1, -1,3};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, w, H);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    static const int probes[][2] = { {10, 0}, {100, 5}, {300, 10}, {450, 15} };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        int x = probes[i][0], y = probes[i][1];
        unsigned char px[4] = {0};
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int gotx = px[0] | (px[2] << 8);       /* R | B<<8 */
        int goty = (px[1] + 5) / 10;           /* G = y*10, rounded */
        if (gotx == x && goty == y) {
            printf("[%s] probe (%3d,%2d): OK\n", label, x, y);
        } else {
            /* driver read (x,y) at y*P + x*4, which landed on true offset
             * goty*2048 + gotx*4  =>  P = (true_offset - x*4) / y */
            long pitch_used =
                y ? ((long)goty * 2048 + (long)gotx * 4 - (long)x * 4) / y : -1;
            printf("[%s] probe (%3d,%2d): READ (%3d,%2d)  raw=%02x %02x %02x %02x"
                   "  => pitch_used ~ %ld\n",
                   label, x, y, gotx, goty, px[0], px[1], px[2], px[3],
                   pitch_used);
            bad = 1;
        }
    }
    printf("[%s] verdict: %s\n", label, bad ? "SHEARED/MISMATCH" : "CLEAN");
    glDeleteProgram(prog);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &dst);
    glDeleteTextures(1, &tex);
    pDestroyImage(dpy, img);
    return bad;
}

int main(int argc, char **argv) {
    const char *alloc_path = argc > 1 ? argv[1] : "/dev/dri/renderD129";
    const char *import_path = argc > 2 ? argv[2] : "/dev/dri/renderD128";

    int afd = open(alloc_path, O_RDWR | O_CLOEXEC);
    if (afd < 0) { perror(alloc_path); return 1; }
    struct gbm_device *gbm = gbm_create_device(afd);
    if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }
    struct gbm_bo *bo = gbm_bo_create(gbm, ALLOC_W, H, GBM_FORMAT_ARGB8888,
                                      GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!bo) { fprintf(stderr, "gbm_bo_create failed\n"); return 1; }
    uint32_t stride = gbm_bo_get_stride(bo);
    int dmabuf = gbm_bo_get_fd(bo);
    printf("alloc: %s linear %dx%d stride=%u fd=%d\n",
           alloc_path, ALLOC_W, H, stride, dmabuf);
    if (stride != 2048) {
        fprintf(stderr, "expected stride 2048 for width 512, got %u — "
                        "adjust the test\n", stride);
        return 1;
    }

    { // fill: every pixel encodes its own coordinates
        uint32_t ms = stride; void *mp = NULL;
        void *map = gbm_bo_map(bo, 0, 0, ALLOC_W, H, GBM_BO_TRANSFER_WRITE,
                               &ms, &mp);
        int via_mmap = 0;
        if (!map) { /* fall back to mapping the exported dma_buf directly */
            map = mmap(NULL, (size_t)stride * H, PROT_READ | PROT_WRITE,
                       MAP_SHARED, dmabuf, 0);
            if (map == MAP_FAILED) {
                fprintf(stderr, "gbm_bo_map and dmabuf mmap both failed\n");
                return 1;
            }
            ms = stride;
            via_mmap = 1;
        }
        for (int y = 0; y < H; y++) {
            uint32_t *row = (uint32_t *)((char *)map + y * ms);
            for (int x = 0; x < ALLOC_W; x++) {
                uint32_t r = x & 0xff, g = (uint32_t)y * 10, b = x >> 8;
                row[x] = 0xff000000u | (r << 16) | (g << 8) | b; /* ARGB */
            }
        }
        if (via_mmap)
            munmap(map, (size_t)stride * H);
        else
            gbm_bo_unmap(bo, mp);
    }

    PFNEGLQUERYDEVICESEXTPROC qdevs =
        (void *)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC qdevstr =
        (void *)eglGetProcAddress("eglQueryDeviceStringEXT");
    EGLDeviceEXT devs[16]; EGLint ndev = 0;
    qdevs(16, devs, &ndev);
    EGLDisplay dpy = EGL_NO_DISPLAY;
    for (int i = 0; i < ndev; i++) {
        const char *node = qdevstr(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        if (!node || strcmp(node, import_path))
            continue;
        EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
        if (d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL)) { dpy = d; break; }
    }
    if (dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "no initializable EGL device for %s\n", import_path);
        return 1;
    }
    printf("import EGL: %s / %s\n", eglQueryString(dpy, EGL_VENDOR),
           eglQueryString(dpy, EGL_VERSION));
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig cfg; EGLint ncfg = 0;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg);
    static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, ncfg ? cfg : NULL, EGL_NO_CONTEXT,
                                      ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext failed 0x%x\n", eglGetError());
        return 1;
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    printf("GL: %s / %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));

    pCreateImage = (void *)eglGetProcAddress("eglCreateImageKHR");
    pDestroyImage = (void *)eglGetProcAddress("eglDestroyImageKHR");
    pImageTargetTex2D = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!pCreateImage || !pImageTargetTex2D) {
        fprintf(stderr, "missing EGLImage entrypoints\n");
        return 1;
    }

    int a = run_pass(dpy, dmabuf, 512, 2048, "A control 512/2048");
    int b = run_pass(dpy, dmabuf, 500, 2048, "B correct 500/2048");
    int c = run_pass(dpy, dmabuf, 500, 2000, "C lie     500/2000");
    /* granularity probe: 2016 is 32-aligned but not 64-aligned.
     * pitch_used 2016 => 32-byte pitch granularity; 1984 => 64-byte. */
    run_pass(dpy, dmabuf, 500, 2016, "D lie     500/2016");

    printf("\n=== SUMMARY ===\n");
    if (a != 0) {
        printf("control pass failed — harness broken, no conclusion\n");
        return 1;
    }
    if (b == 0 && c == 1)
        printf("driver HONORS explicit pitch (correct pitch clean, wrong pitch "
               "sheared) — forum bug NOT reproduced\n");
    else if (b == 1)
        printf("driver IGNORES explicit pitch (shear with the CORRECT pitch) — "
               "forum bug REPRODUCED\n");
    else if (b == 0 && c == 0)
        printf("both clean — driver ignores pitch but its assumption matched "
               "2048? inconclusive, try other widths\n");
    else
        printf("import failures — see above\n");
    return 0;
}
