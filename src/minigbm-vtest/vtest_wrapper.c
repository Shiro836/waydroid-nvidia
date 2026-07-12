/*
 * Drop-in replacement for libgbm_mesa_wrapper.so: instead of allocating
 * through the guest's mesa libgbm (AMD buffers the host compositor cannot
 * display), every allocation is served by the Waydroid venus vtest server
 * over its unix socket (VCMD_RESOURCE_ALLOC_GPU).  GPU buffers come back as
 * NVIDIA dma_bufs with real block-linear modifiers; CPU-mappable buffers are
 * host udmabufs that mmap directly.
 *
 * ABI: exports get_gbm_ops() returning struct gbm_ops (gbm_mesa_wrapper.h).
 * The struct gbm_device* / struct gbm_bo* pointers are opaque to the caller,
 * so we return our own structs.
 */

#define LOG_TAG "vtest_wrapper"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <android/log.h>
#include <sys/system_properties.h>

#include "gbm_mesa_wrapper.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

/* ---- vtest protocol (mirror of virglrenderer vtest_protocol.h) ---- */
#define VTEST_HDR_SIZE 2
#define VCMD_CREATE_RENDERER 8
#define VCMD_RESOURCE_ALLOC_GPU 41
#define VCMD_ALLOC_GPU_FLAG_MAPPABLE (1u << 0)
#define VCMD_ALLOC_GPU_FLAG_SCANOUT (1u << 1)
#define VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE 7

#define DEFAULT_SOCKET "/dev/venus/venus.sock"

#define FOURCC(a, b, c, d) \
   ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_R8            FOURCC('R', '8', ' ', ' ')
#define FMT_RGB565        FOURCC('R', 'G', '1', '6')
#define FMT_XRGB8888      FOURCC('X', 'R', '2', '4')
#define FMT_ARGB8888      FOURCC('A', 'R', '2', '4')
#define FMT_XBGR8888      FOURCC('X', 'B', '2', '4')
#define FMT_ABGR8888      FOURCC('A', 'B', '2', '4')
#define FMT_ABGR2101010   FOURCC('A', 'B', '3', '0')
#define FMT_ABGR16161616F FOURCC('A', 'B', '4', 'H')

struct vtest_dev {
   pthread_mutex_t mutex;
   int sock;
};

struct vtest_bo {
   int fd;
   uint64_t size;
   void *map;
};

/* ---------------- socket plumbing ---------------- */

static int
sock_write_all(int fd, const void *buf, size_t len)
{
   const char *p = buf;
   while (len) {
      ssize_t n = write(fd, p, len);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return -1;
      p += n;
      len -= (size_t)n;
   }
   return 0;
}

static int
sock_read_all(int fd, void *buf, size_t len)
{
   char *p = buf;
   while (len) {
      ssize_t n = read(fd, p, len);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return -1;
      p += n;
      len -= (size_t)n;
   }
   return 0;
}

static int
sock_recv_fd(int sock)
{
   char data;
   struct iovec iov = { &data, 1 };
   char ctrl[CMSG_SPACE(sizeof(int))];
   struct msghdr msg = { 0 };
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;
   msg.msg_control = ctrl;
   msg.msg_controllen = sizeof(ctrl);

   ssize_t n;
   do {
      n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
   } while (n < 0 && errno == EINTR);
   if (n <= 0)
      return -1;

   struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
   if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
      return -1;
   int fd;
   memcpy(&fd, CMSG_DATA(c), sizeof(fd));
   return fd;
}

static int
vtest_connect(void)
{
   char path[PROP_VALUE_MAX];
   if (__system_property_get("mesa.vtest.socket.name", path) <= 0)
      strcpy(path, DEFAULT_SOCKET);

   int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (sock < 0)
      return -1;

   struct sockaddr_un addr = { .sun_family = AF_UNIX };
   strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
   if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))) {
      LOGE("connect(%s): %s", path, strerror(errno));
      close(sock);
      return -1;
   }

   /* CREATE_RENDERER; the length field is in bytes (strlen+1) */
   const char name[8] = "gralloc";
   const uint32_t hdr[VTEST_HDR_SIZE] = { sizeof(name), VCMD_CREATE_RENDERER };
   if (sock_write_all(sock, hdr, sizeof(hdr)) ||
       sock_write_all(sock, name, sizeof(name))) {
      close(sock);
      return -1;
   }
   return sock;
}

/* ---------------- gbm_ops implementation ---------------- */

static uint32_t
vtest_get_gbm_format(uint32_t drm_format)
{
   switch (drm_format) {
   /* formats the host allocator can create as renderable NVIDIA images;
    * everything else takes minigbm's linear R8-blob path */
   case FMT_R8:
   case FMT_RGB565:
   case FMT_XRGB8888:
   case FMT_ARGB8888:
   case FMT_XBGR8888:
   case FMT_ABGR8888:
   case FMT_ABGR2101010:
   case FMT_ABGR16161616F:
      return drm_format;
   default:
      return 0;
   }
}

static struct gbm_device *
vtest_dev_create(int fd)
{
   (void)fd; /* the DRM node is irrelevant; buffers come from the host */

   struct vtest_dev *dev = calloc(1, sizeof(*dev));
   if (!dev)
      return NULL;
   pthread_mutex_init(&dev->mutex, NULL);
   dev->sock = -1;
   LOGI("vtest gralloc wrapper active (host-GPU allocations)");
   return (struct gbm_device *)dev;
}

static void
vtest_dev_destroy(struct gbm_device *gbm)
{
   struct vtest_dev *dev = (struct vtest_dev *)gbm;
   if (dev->sock >= 0)
      close(dev->sock);
   free(dev);
}

static int
vtest_alloc(struct alloc_args *args)
{
   struct vtest_dev *dev = (struct vtest_dev *)args->gbm;

   uint32_t flags = 0;
   if (args->force_linear || args->needs_map_stride)
      flags |= VCMD_ALLOC_GPU_FLAG_MAPPABLE;
   if (args->use_scanout)
      flags |= VCMD_ALLOC_GPU_FLAG_SCANOUT;

   const uint32_t req[VTEST_HDR_SIZE + 4] = {
      4, VCMD_RESOURCE_ALLOC_GPU,
      args->width, args->height, args->drm_format, flags,
   };
   uint32_t resp[VTEST_HDR_SIZE + VCMD_RESOURCE_ALLOC_GPU_RESP_SIZE];

   pthread_mutex_lock(&dev->mutex);

   /* lazy connect + one reconnect (server may restart under us) */
   for (int attempt = 0; attempt < 2; attempt++) {
      if (dev->sock < 0)
         dev->sock = vtest_connect();
      if (dev->sock < 0)
         break;
      if (sock_write_all(dev->sock, req, sizeof(req)) == 0 &&
          sock_read_all(dev->sock, resp, sizeof(resp)) == 0)
         goto have_resp;
      close(dev->sock);
      dev->sock = -1;
   }
   pthread_mutex_unlock(&dev->mutex);
   LOGE("alloc: no vtest connection");
   return -ENODEV;

have_resp:;
   const uint32_t *d = &resp[VTEST_HDR_SIZE];
   const uint32_t status = d[0];
   if (status) {
      pthread_mutex_unlock(&dev->mutex);
      LOGE("alloc %ux%u fmt=0x%08x flags=%u failed: host status %u", args->width,
           args->height, args->drm_format, flags, status);
      return -(int)status;
   }

   int fd = sock_recv_fd(dev->sock);
   pthread_mutex_unlock(&dev->mutex);
   if (fd < 0) {
      LOGE("alloc: fd receive failed");
      return -EIO;
   }

   args->out_fd = fd;
   args->out_stride = d[1];
   args->out_map_stride = d[2];
   args->out_modifier = d[3] | (uint64_t)d[4] << 32;
   return 0;
}

static struct gbm_bo *
vtest_import(struct gbm_device *gbm, int buf_fd, uint32_t width, uint32_t height,
             uint32_t stride, uint64_t modifier, uint32_t drm_format)
{
   (void)gbm;
   (void)width;
   (void)height;
   (void)stride;
   (void)modifier;
   (void)drm_format;

   struct vtest_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return NULL;

   const off_t size = lseek(buf_fd, 0, SEEK_END);
   if (size <= 0) {
      free(bo);
      return NULL;
   }
   bo->fd = fcntl(buf_fd, F_DUPFD_CLOEXEC, 0);
   bo->size = (uint64_t)size;
   if (bo->fd < 0) {
      free(bo);
      return NULL;
   }
   return (struct gbm_bo *)bo;
}

static void
vtest_free(struct gbm_bo *gbm_bo)
{
   struct vtest_bo *bo = (struct vtest_bo *)gbm_bo;
   if (bo->map)
      munmap(bo->map, bo->size);
   close(bo->fd);
   free(bo);
}

static void
vtest_map(struct gbm_bo *gbm_bo, int w, int h, void **addr, void **map_data)
{
   (void)w;
   (void)h;
   struct vtest_bo *bo = (struct vtest_bo *)gbm_bo;

   if (!bo->map) {
      void *p = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->fd, 0);
      if (p == MAP_FAILED) {
         /* GPU-only buffers (NVIDIA dma_buf) are not mappable by design */
         LOGE("mmap(size=%llu): %s", (unsigned long long)bo->size, strerror(errno));
         *addr = NULL;
         *map_data = NULL;
         return;
      }
      bo->map = p;
   }
   *addr = bo->map;
   *map_data = bo->map;
}

static void
vtest_unmap(struct gbm_bo *gbm_bo, void *map_data)
{
   /* keep the mapping cached until free(); minigbm maps/unmaps per lock */
   (void)gbm_bo;
   (void)map_data;
}

static struct gbm_ops vtest_gbm_ops = {
   .get_gbm_format = vtest_get_gbm_format,
   .dev_create = vtest_dev_create,
   .dev_destroy = vtest_dev_destroy,
   .alloc = vtest_alloc,
   .import = vtest_import,
   .free = vtest_free,
   .map = vtest_map,
   .unmap = vtest_unmap,
};

__attribute__((visibility("default"))) struct gbm_ops *
get_gbm_ops(void)
{
   return &vtest_gbm_ops;
}
