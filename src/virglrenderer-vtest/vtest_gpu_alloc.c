/* See vtest_gpu_alloc.h. Vulkan is loaded lazily via dlopen so the vtest
 * server keeps working (minus this command) on hosts without a driver. */

#include "vtest_gpu_alloc.h"
#include "vtest_alloc_formats.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define ALLOC_ALIGN(v, a) (((v) + (a)-1) & ~((uint64_t)(a)-1))

struct alloc_vk {
   void *lib;
   PFN_vkGetInstanceProcAddr get_proc;
   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkDevice device;
   VkPhysicalDeviceMemoryProperties mem_props;

   PFN_vkCreateImage CreateImage;
   PFN_vkDestroyImage DestroyImage;
   PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
   PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkBindImageMemory BindImageMemory;
   PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
   PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2;
   PFN_vkGetImageDrmFormatModifierPropertiesEXT GetImageDrmFormatModifierPropertiesEXT;
};

static struct alloc_vk alloc_vk;
static pthread_mutex_t alloc_vk_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool alloc_vk_failed;
static struct timespec alloc_vk_last_fail;

static VkFormat
drm_format_to_vk(uint32_t drm_format)
{
   switch (drm_format) {
   case VTEST_FORMAT_R8:
      return VK_FORMAT_R8_UNORM;
   case VTEST_FORMAT_RGB565:
      return VK_FORMAT_R5G6B5_UNORM_PACK16;
   case VTEST_FORMAT_XRGB8888:
   case VTEST_FORMAT_ARGB8888:
      return VK_FORMAT_B8G8R8A8_UNORM;
   case VTEST_FORMAT_XBGR8888:
   case VTEST_FORMAT_ABGR8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case VTEST_FORMAT_ABGR2101010:
   case VTEST_FORMAT_XBGR2101010:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   case VTEST_FORMAT_ABGR16161616F:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
   case VTEST_FORMAT_NV12:
      return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   case VTEST_FORMAT_P010:
      return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
   default:
      return VK_FORMAT_UNDEFINED;
   }
}

static uint32_t
drm_format_bpp(uint32_t drm_format)
{
   switch (drm_format) {
   case VTEST_FORMAT_R8:
   case VTEST_FORMAT_NV12:
      return 1;
   case VTEST_FORMAT_RGB565:
   case VTEST_FORMAT_P010:
      return 2;
   case VTEST_FORMAT_ABGR16161616F:
      return 8;
   default:
      return 4;
   }
}

static int
alloc_vk_init_locked(void)
{
   struct alloc_vk *vk = &alloc_vk;

   if (vk->device)
      return 0;
   if (alloc_vk_failed) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec - alloc_vk_last_fail.tv_sec < 5)
         return -ENODEV;
   }

   vk->lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!vk->lib) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }
   *(void **)&vk->get_proc = dlsym(vk->lib, "vkGetInstanceProcAddr");
   if (!vk->get_proc) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

#define GET_GLOBAL(name) PFN_vk##name name = (PFN_vk##name)vk->get_proc(NULL, "vk" #name)
#define GET_INST(name) PFN_vk##name name = (PFN_vk##name)vk->get_proc(vk->instance, "vk" #name)

   GET_GLOBAL(CreateInstance);
   if (!CreateInstance) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vtest-gpu-alloc",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   if (CreateInstance(&inst_info, NULL, &vk->instance) != VK_SUCCESS) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   GET_INST(EnumeratePhysicalDevices);
   GET_INST(GetPhysicalDeviceProperties);
   GET_INST(EnumerateDeviceExtensionProperties);
   GET_INST(GetPhysicalDeviceMemoryProperties);
   GET_INST(CreateDevice);
   GET_INST(GetDeviceProcAddr);

   uint32_t count = 0;
   if (EnumeratePhysicalDevices(vk->instance, &count, NULL) < 0 || !count) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   VkPhysicalDevice *devices = malloc(count * sizeof(*devices));
   if (!devices) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENOMEM;
   }
   if (EnumeratePhysicalDevices(vk->instance, &count, devices) != VK_SUCCESS) {
      free(devices);
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   /* prefer the device named by VTEST_ALLOC_GPU, else first discrete with
    * dma_buf export */
   const char *want = getenv("VTEST_ALLOC_GPU");
   VkPhysicalDevice best = VK_NULL_HANDLE;
   int best_score = -1;
   for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDeviceProperties props;
      GetPhysicalDeviceProperties(devices[i], &props);

      bool has_dma_buf = false, has_modifier = false;
      uint32_t ext_count = 0;
      EnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, NULL);
      if (ext_count > 0) {
         VkExtensionProperties *exts = malloc(ext_count * sizeof(*exts));
         if (exts) {
            if (EnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, exts) == VK_SUCCESS) {
               for (uint32_t j = 0; j < ext_count; j++) {
                  if (!strcmp(exts[j].extensionName, "VK_EXT_external_memory_dma_buf"))
                     has_dma_buf = true;
                  if (!strcmp(exts[j].extensionName, "VK_EXT_image_drm_format_modifier"))
                     has_modifier = true;
               }
            }
            free(exts);
         }
      }
      if (!has_dma_buf || !has_modifier)
         continue;

      int score = 0;
      if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
         score += 2;
      if (want && strstr(props.deviceName, want))
         score += 10;
      if (score > best_score) {
         best_score = score;
         best = devices[i];
      }
   }
   free(devices);
   if (best == VK_NULL_HANDLE) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }
   vk->physical_device = best;
   GetPhysicalDeviceMemoryProperties(best, &vk->mem_props);

   {
      VkPhysicalDeviceProperties props;
      GetPhysicalDeviceProperties(best, &props);
      fprintf(stderr, "vtest_gpu_alloc: allocating on \"%s\"\n", props.deviceName);
   }

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const char *dev_exts[] = {
      "VK_KHR_external_memory_fd",
      "VK_EXT_external_memory_dma_buf",
      "VK_EXT_image_drm_format_modifier",
   };
   const VkDeviceCreateInfo dev_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = sizeof(dev_exts) / sizeof(dev_exts[0]),
      .ppEnabledExtensionNames = dev_exts,
   };
   if (CreateDevice(best, &dev_info, NULL, &vk->device) != VK_SUCCESS) {
      vk->device = VK_NULL_HANDLE;
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

#define GET_DEV(name) \
   vk->name = (PFN_vk##name)GetDeviceProcAddr(vk->device, "vk" #name)
   GET_DEV(CreateImage);
   GET_DEV(DestroyImage);
   GET_DEV(GetImageMemoryRequirements);
   GET_DEV(GetImageSubresourceLayout);
   GET_DEV(AllocateMemory);
   GET_DEV(FreeMemory);
   GET_DEV(BindImageMemory);
   GET_DEV(GetMemoryFdKHR);
   GET_DEV(GetImageDrmFormatModifierPropertiesEXT);
   vk->GetPhysicalDeviceFormatProperties2 =
      (PFN_vkGetPhysicalDeviceFormatProperties2)vk->get_proc(
         vk->instance, "vkGetPhysicalDeviceFormatProperties2");
#undef GET_DEV
#undef GET_INST
#undef GET_GLOBAL

   if (!vk->GetMemoryFdKHR || !vk->GetPhysicalDeviceFormatProperties2 ||
       !vk->GetImageDrmFormatModifierPropertiesEXT) {
      clock_gettime(CLOCK_MONOTONIC, &alloc_vk_last_fail);
      alloc_vk_failed = true;
      return -ENODEV;
   }

   alloc_vk_failed = false;
   return 0;
}

static int
vtest_gpu_alloc_image(uint32_t width, uint32_t height, uint32_t drm_format,
                      bool linear, uint32_t *out_stride,
                      uint64_t *out_modifier, uint64_t *out_size, int *out_fd)
{
   const VkFormat format = drm_format_to_vk(drm_format);
   if (format == VK_FORMAT_UNDEFINED)
      return -EINVAL;

   pthread_mutex_lock(&alloc_vk_mutex);

   struct alloc_vk *vk = &alloc_vk;
   int ret = alloc_vk_init_locked();
   if (ret) {
      pthread_mutex_unlock(&alloc_vk_mutex);
      return ret;
   }

   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkDrmFormatModifierPropertiesEXT *props = NULL;
   uint64_t *mod_candidates = NULL;
   ret = -EINVAL;

   /* NVIDIA's EGL treats DRM_FORMAT_MOD_LINEAR dmabufs as external-only
    * (unbindable as GL_TEXTURE_2D), which KWin cannot display. Allocate
    * with the driver's real (block-linear) modifiers instead: enumerate
    * what the format supports and let the driver pick.
    */
   uint32_t mod_count = 0;
   if (linear) {
      mod_candidates = malloc(sizeof(*mod_candidates));
      if (!mod_candidates)
         goto out;
      /* CPU-mappable: explicit LINEAR so the guest can mmap and compute
       * pixel offsets; NVIDIA dma_bufs mmap fine from any memory type */
      mod_candidates[mod_count++] = 0; /* DRM_FORMAT_MOD_LINEAR */
   } else {
      VkDrmFormatModifierPropertiesListEXT mod_list = {
         .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
      };
      VkFormatProperties2 fmt_props = {
         .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
         .pNext = &mod_list,
      };
      vk->GetPhysicalDeviceFormatProperties2(vk->physical_device, format,
                                             &fmt_props);
      uint32_t mod_count_avail = mod_list.drmFormatModifierCount;
      if (mod_count_avail > 0) {
         props = malloc(mod_count_avail * sizeof(*props));
         mod_candidates = malloc(mod_count_avail * sizeof(*mod_candidates));
         if (props && mod_candidates) {
            mod_list.pDrmFormatModifierProperties = props;
            vk->GetPhysicalDeviceFormatProperties2(vk->physical_device, format,
                                                   &fmt_props);

            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            const uint32_t expected_planes =
               (drm_format == VTEST_FORMAT_NV12 || drm_format == VTEST_FORMAT_P010) ? 2 : 1;

            for (uint32_t i = 0; i < mod_list.drmFormatModifierCount; i++) {
               if (props[i].drmFormatModifierPlaneCount == expected_planes &&
                   (props[i].drmFormatModifierTilingFeatures & need) == need &&
                   props[i].drmFormatModifier != 0)
                  mod_candidates[mod_count++] = props[i].drmFormatModifier;
            }
         }
      }
   }
   if (!mod_count || !mod_candidates)
      goto out;

   const VkExternalMemoryImageCreateInfo ext_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageDrmFormatModifierListCreateInfoEXT mod_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
      .pNext = &ext_info,
      .drmFormatModifierCount = mod_count,
      .pDrmFormatModifiers = mod_candidates,
   };
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &mod_info,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (vk->CreateImage(vk->device, &image_info, NULL, &image) != VK_SUCCESS)
      goto out;

   VkMemoryRequirements reqs;
   vk->GetImageMemoryRequirements(vk->device, image, &reqs);

   uint32_t mem_type = UINT32_MAX;
   const VkMemoryPropertyFlags want =
      linear ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   for (uint32_t i = 0; i < vk->mem_props.memoryTypeCount; i++) {
      if ((reqs.memoryTypeBits & (1u << i)) &&
          (vk->mem_props.memoryTypes[i].propertyFlags & want) == want) {
         mem_type = i;
         break;
      }
   }
   if (mem_type == UINT32_MAX)
      mem_type = ffs(reqs.memoryTypeBits) - 1;
   if (reqs.memoryTypeBits == 0)
      goto out;

   const VkExportMemoryAllocateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkMemoryDedicatedAllocateInfo dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &export_info,
      .image = image,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated_info,
      .allocationSize = reqs.size,
      .memoryTypeIndex = mem_type,
   };
   if (vk->AllocateMemory(vk->device, &alloc_info, NULL, &memory) != VK_SUCCESS) {
      memory = VK_NULL_HANDLE;
      goto out;
   }
   if (vk->BindImageMemory(vk->device, image, memory, 0) != VK_SUCCESS)
      goto out;

   VkImageDrmFormatModifierPropertiesEXT chosen = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
   };
   if (vk->GetImageDrmFormatModifierPropertiesEXT(vk->device, image, &chosen) !=
       VK_SUCCESS)
      goto out;

   const VkImageSubresource subres = {
      .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
   };
   VkSubresourceLayout layout;
   vk->GetImageSubresourceLayout(vk->device, image, &subres, &layout);

   const VkMemoryGetFdInfoKHR fd_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
      .memory = memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   int fd = -1;
   if (vk->GetMemoryFdKHR(vk->device, &fd_info, &fd) != VK_SUCCESS || fd < 0)
      goto out;

   *out_stride = (uint32_t)layout.rowPitch;
   *out_modifier = chosen.drmFormatModifier;
   *out_size = reqs.size;
   *out_fd = fd;
   ret = 0;

out:
   free(props);
   free(mod_candidates);
   if (image)
      vk->DestroyImage(vk->device, image, NULL);
   if (memory)
      vk->FreeMemory(vk->device, memory, NULL);
   pthread_mutex_unlock(&alloc_vk_mutex);
   return ret;
}

int
vtest_gpu_alloc_gpu(uint32_t width, uint32_t height, uint32_t drm_format,
                    uint32_t *out_stride, uint64_t *out_modifier,
                    uint64_t *out_size, int *out_fd)
{
   return vtest_gpu_alloc_image(width, height, drm_format, false, out_stride,
                                out_modifier, out_size, out_fd);
}

int
vtest_gpu_alloc_cpu(uint32_t width, uint32_t height, uint32_t drm_format,
                    uint32_t *out_stride, uint64_t *out_size, int *out_fd)
{
   /* CPU-mappable gralloc allocations use /dev/udmabuf over a shrink-sealed memfd. */
   uint32_t stride = 0;
   uint64_t size = 0;

   if (drm_format == VTEST_FORMAT_NV12) {
      stride = (uint32_t)ALLOC_ALIGN((uint64_t)width, 256);
      size = ALLOC_ALIGN((uint64_t)stride * height * 3 / 2, 4096);
   } else if (drm_format == VTEST_FORMAT_P010) {
      stride = (uint32_t)ALLOC_ALIGN((uint64_t)width * 2, 256);
      size = ALLOC_ALIGN((uint64_t)stride * height * 3 / 2, 4096);
   } else {
      const uint32_t bpp = drm_format_bpp(drm_format);
      stride = (uint32_t)ALLOC_ALIGN((uint64_t)width * bpp, 256);
      size = ALLOC_ALIGN((uint64_t)stride * height, 4096);
   }

   int memfd = memfd_create("vtest-gralloc", MFD_ALLOW_SEALING | MFD_CLOEXEC);
   if (memfd < 0)
      return -errno;
   if (ftruncate(memfd, (off_t)size) < 0) {
      close(memfd);
      return -errno;
   }
   /* udmabuf requires the memfd to be shrink-sealed */
   fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK);

   int ufd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
   if (ufd < 0) {
      int err = errno;
      if (err == EACCES || err == EPERM)
         fprintf(stderr,
                 "vtest_gpu_alloc: cannot open /dev/udmabuf (%s). Install "
                 "70-waydroid-nvidia.rules (udev uaccess) and re-login, or "
                 "grant this user access to /dev/udmabuf.\n",
                 strerror(err));
      close(memfd);
      return -err;
   }
   struct udmabuf_create create = {
      .memfd = (uint32_t)memfd,
      .flags = UDMABUF_FLAGS_CLOEXEC,
      .offset = 0,
      .size = size,
   };
   int dmabuf = ioctl(ufd, UDMABUF_CREATE, &create);
   close(ufd);
   close(memfd);
   if (dmabuf < 0)
      return -errno;

   *out_stride = stride;
   *out_size = size;
   *out_fd = dmabuf;
   return 0;
}
