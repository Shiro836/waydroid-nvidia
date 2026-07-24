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
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <android/log.h>
#include <sys/system_properties.h>

#include "gbm_mesa_wrapper.h"
#include "vtest_alloc_formats.h"

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
   if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
       c->cmsg_len < CMSG_LEN(sizeof(int)))
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

   /* non-blocking connect + poll(1s) deadline */
   int orig_flags = fcntl(sock, F_GETFL, 0);
   if (orig_flags >= 0)
      fcntl(sock, F_SETFL, orig_flags | O_NONBLOCK);

   struct sockaddr_un addr = { .sun_family = AF_UNIX };
   strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
   int res = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
   if (res < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK)) {
      struct pollfd pfd = { .fd = sock, .events = POLLOUT };
      res = poll(&pfd, 1, 1000); /* 1 sec deadline for connection phase */
      if (res <= 0) {
         LOGE("connect(%s): connection deadline timed out", path);
         close(sock);
         return -1;
      }
      int err = 0;
      socklen_t len = sizeof(err);
      if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
         LOGE("connect(%s): %s", path, strerror(err ? err : errno));
         close(sock);
         return -1;
      }
      res = 0;
   } else if (res < 0) {
      LOGE("connect(%s): %s", path, strerror(errno));
      close(sock);
      return -1;
   }

   /* restore socket flags and configure 1-second read/write I/O timeouts */
   if (orig_flags >= 0)
      fcntl(sock, F_SETFL, orig_flags);

   struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
   setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

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
   if (vtest_is_supported_drm_format(drm_format))
      return drm_format;
   return 0;
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
   pthread_mutex_destroy(&dev->mutex);
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

   struct stat st;
   if (fstat(buf_fd, &st) < 0 || st.st_size <= 0) {
      free(bo);
      return NULL;
   }
   bo->fd = fcntl(buf_fd, F_DUPFD_CLOEXEC, 0);
   bo->size = (uint64_t)st.st_size;
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
