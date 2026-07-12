// End-to-end test of VCMD_RESOURCE_ALLOC_GPU:
//  1. connect to the vtest socket, CREATE_RENDERER, request a GPU buffer
//  2. EGL-import the returned dma_buf on the NVIDIA device and bind it as
//     GL_TEXTURE_2D — the exact operation KWin needs for the Waydroid window
//  3. request a MAPPABLE buffer and verify mmap write/read works
//
// usage: nvalloc [socket-path]
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define VTEST_HDR_SIZE 2
#define VCMD_CREATE_RENDERER 8
#define VCMD_RESOURCE_ALLOC_GPU 41
#define VCMD_ALLOC_GPU_FLAG_MAPPABLE 1
#define RESP_DWORDS 7

#define W 512
#define H 512
#define FMT_ARGB8888 0x34325241 /* AR24 */

static int sock_write(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

static int sock_read(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

static int recv_fd(int sock) {
    char data;
    struct iovec iov = {&data, 1};
    char ctrl[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = ctrl; msg.msg_controllen = sizeof(ctrl);
    if (recvmsg(sock, &msg, 0) <= 0) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_type != SCM_RIGHTS) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(fd));
    return fd;
}

static int alloc_buffer(int sock, uint32_t flags, uint32_t *stride,
                        uint64_t *modifier, uint64_t *size) {
    uint32_t req[VTEST_HDR_SIZE + 4] = {4, VCMD_RESOURCE_ALLOC_GPU,
                                        W, H, FMT_ARGB8888, flags};
    if (sock_write(sock, req, sizeof(req))) return -1;
    uint32_t resp[VTEST_HDR_SIZE + RESP_DWORDS];
    if (sock_read(sock, resp, sizeof(resp))) return -1;
    uint32_t *d = &resp[VTEST_HDR_SIZE];
    printf("alloc(flags=%u): status=%u stride=%u map_stride=%u modifier=0x%llx size=%llu\n",
           flags, d[0], d[1], d[2],
           (unsigned long long)(d[3] | (uint64_t)d[4] << 32),
           (unsigned long long)(d[5] | (uint64_t)d[6] << 32));
    if (d[0]) return -1;
    *stride = d[1];
    *modifier = d[3] | (uint64_t)d[4] << 32;
    *size = d[5] | (uint64_t)d[6] << 32;
    return recv_fd(sock);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/run/waydroid-venus/venus.sock";
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("connect"); return 1;
    }

    /* quirk: CREATE_RENDERER length is in BYTES (strlen+1), not dwords */
    const char name[8] = "nvalloc";
    uint32_t hdr[VTEST_HDR_SIZE] = {sizeof(name), VCMD_CREATE_RENDERER};
    if (sock_write(sock, hdr, sizeof(hdr)) || sock_write(sock, name, sizeof(name))) {
        fprintf(stderr, "handshake failed\n"); return 1;
    }

    // --- GPU buffer → EGL TEXTURE_2D on NVIDIA (the KWin criterion) ---
    uint32_t stride; uint64_t modifier, size;
    int fd = alloc_buffer(sock, 0, &stride, &modifier, &size);
    if (fd < 0) { fprintf(stderr, "GPU alloc failed\n"); return 1; }

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
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, NULL, EGL_NO_CONTEXT, ctx_attr);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    printf("GL: %s\n", glGetString(GL_RENDERER));

    PFNEGLCREATEIMAGEKHRPROC createImage = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImage =
        (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    EGLint attrs[] = {
        EGL_WIDTH, W, EGL_HEIGHT, H,
        EGL_LINUX_DRM_FOURCC_EXT, FMT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)modifier,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(modifier >> 32),
        EGL_NONE,
    };
    EGLImageKHR img = createImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        printf("FAIL: eglCreateImageKHR 0x%x\n", eglGetError()); return 1;
    }
    while (glGetError() != GL_NO_ERROR);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    bindImage(GL_TEXTURE_2D, (GLeglImageOES)img);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("FAIL: TEXTURE_2D bind error 0x%x (KWin would show black)\n", err);
        return 1;
    }
    printf("PASS: NVIDIA-allocated buffer binds as GL_TEXTURE_2D on NVIDIA EGL\n");

    // --- mappable buffer → mmap write/read ---
    int mfd = alloc_buffer(sock, VCMD_ALLOC_GPU_FLAG_MAPPABLE, &stride, &modifier, &size);
    if (mfd < 0) { fprintf(stderr, "FAIL: mappable alloc\n"); return 1; }
    uint32_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (map == MAP_FAILED) { printf("FAIL: mmap: %s\n", strerror(errno)); return 1; }
    map[0] = 0xdeadbeef;
    map[(size / 4) - 1] = 0xcafebabe;
    if (map[0] != 0xdeadbeef || map[(size / 4) - 1] != 0xcafebabe) {
        printf("FAIL: mmap readback\n"); return 1;
    }
    munmap(map, size);
    printf("PASS: mappable buffer mmap write/read OK\n");
    return 0;
}
