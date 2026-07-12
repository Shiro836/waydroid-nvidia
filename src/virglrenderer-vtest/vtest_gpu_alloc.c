/* See vtest_gpu_alloc.h. Vulkan is loaded lazily via dlopen so the vtest
 * server keeps working (minus this command) on hosts without a driver. */

#include "vtest_gpu_alloc.h"

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
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>



#define ALLOC_ALIGN(v, a) (((v) + (a)-1) & ~((uint64_t)(a)-1))

/* DRM fourccs we can allocate as renderable Vulkan images (matches the guest
 * wrapper's get_gbm_format whitelist) */
#define DRM_FOURCC(a, b, c, d) \
   ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define ALLOC_DRM_FORMAT_R8            DRM_FOURCC('R', '8', ' ', ' ')
#define ALLOC_DRM_FORMAT_RGB565        DRM_FOURCC('R', 'G', '1', '6')
#define ALLOC_DRM_FORMAT_XRGB8888      DRM_FOURCC('X', 'R', '2', '4')
#define ALLOC_DRM_FORMAT_ARGB8888      DRM_FOURCC('A', 'R', '2', '4')
#define ALLOC_DRM_FORMAT_XBGR8888      DRM_FOURCC('X', 'B', '2', '4')
#define ALLOC_DRM_FORMAT_ABGR8888      DRM_FOURCC('A', 'B', '2', '4')
#define ALLOC_DRM_FORMAT_ABGR2101010   DRM_FOURCC('A', 'B', '3', '0')
#define ALLOC_DRM_FORMAT_ABGR16161616F DRM_FOURCC('A', 'B', '4', 'H')

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

static VkFormat
drm_format_to_vk(uint32_t drm_format)
{
   switch (drm_format) {
   case ALLOC_DRM_FORMAT_R8:
      return VK_FORMAT_R8_UNORM;
   case ALLOC_DRM_FORMAT_RGB565:
      return VK_FORMAT_R5G6B5_UNORM_PACK16;
   case ALLOC_DRM_FORMAT_XRGB8888:
   case ALLOC_DRM_FORMAT_ARGB8888:
      return VK_FORMAT_B8G8R8A8_UNORM;
   case ALLOC_DRM_FORMAT_XBGR8888:
   case ALLOC_DRM_FORMAT_ABGR8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case ALLOC_DRM_FORMAT_ABGR2101010:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   case ALLOC_DRM_FORMAT_ABGR16161616F:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
   default:
      return VK_FORMAT_UNDEFINED;
   }
}

static uint32_t
drm_format_bpp(uint32_t drm_format)
{
   switch (drm_format) {
   case ALLOC_DRM_FORMAT_R8:
      return 1;
   case ALLOC_DRM_FORMAT_RGB565:
      return 2;
   case ALLOC_DRM_FORMAT_ABGR16161616F:
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
   if (alloc_vk_failed)
      return -ENODEV;
   alloc_vk_failed = true; /* cleared on success */

   vk->lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!vk->lib)
      return -ENODEV;
   *(void **)&vk->get_proc = dlsym(vk->lib, "vkGetInstanceProcAddr");
   if (!vk->get_proc)
      return -ENODEV;

#define GET_GLOBAL(name) PFN_vk##name name = (PFN_vk##name)vk->get_proc(NULL, "vk" #name)
#define GET_INST(name) PFN_vk##name name = (PFN_vk##name)vk->get_proc(vk->instance, "vk" #name)

   GET_GLOBAL(CreateInstance);
   if (!CreateInstance)
      return -ENODEV;

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vtest-gpu-alloc",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo inst_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   if (CreateInstance(&inst_info, NULL, &vk->instance) != VK_SUCCESS)
      return -ENODEV;

   GET_INST(EnumeratePhysicalDevices);
   GET_INST(GetPhysicalDeviceProperties);
   GET_INST(EnumerateDeviceExtensionProperties);
   GET_INST(GetPhysicalDeviceMemoryProperties);
   GET_INST(CreateDevice);
   GET_INST(GetDeviceProcAddr);

   VkPhysicalDevice devices[16];
   uint32_t count = 16;
   if (EnumeratePhysicalDevices(vk->instance, &count, devices) < 0 || !count)
      return -ENODEV;

   /* prefer the device named by VTEST_ALLOC_GPU, else first discrete with
    * dma_buf export */
   const char *want = getenv("VTEST_ALLOC_GPU");
   VkPhysicalDevice best = VK_NULL_HANDLE;
   int best_score = -1;
   for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDeviceProperties props;
      GetPhysicalDeviceProperties(devices[i], &props);

      bool has_dma_buf = false, has_modifier = false;
      VkExtensionProperties exts[512];
      uint32_t ext_count = 512;
      EnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, exts);
      for (uint32_t j = 0; j < ext_count; j++) {
         if (!strcmp(exts[j].extensionName, "VK_EXT_external_memory_dma_buf"))
            has_dma_buf = true;
         if (!strcmp(exts[j].extensionName, "VK_EXT_image_drm_format_modifier"))
            has_modifier = true;
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
   if (best == VK_NULL_HANDLE)
      return -ENODEV;
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
       !vk->GetImageDrmFormatModifierPropertiesEXT)
      return -ENODEV;

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
   ret = -EINVAL;

   /* NVIDIA's EGL treats DRM_FORMAT_MOD_LINEAR dmabufs as external-only
    * (unbindable as GL_TEXTURE_2D), which KWin cannot display. Allocate
    * with the driver's real (block-linear) modifiers instead: enumerate
    * what the format supports and let the driver pick.
    */
   uint64_t mod_candidates[64];
   uint32_t mod_count = 0;
   if (linear) {
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
      VkDrmFormatModifierPropertiesEXT props[64];
      mod_list.drmFormatModifierCount =
         mod_list.drmFormatModifierCount < 64 ? mod_list.drmFormatModifierCount
                                              : 64;
      mod_list.pDrmFormatModifierProperties = props;
      vk->GetPhysicalDeviceFormatProperties2(vk->physical_device, format,
                                             &fmt_props);

      const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      for (uint32_t i = 0; i < mod_list.drmFormatModifierCount; i++) {
         /* single-plane, renderable+samplable, not linear */
         if (props[i].drmFormatModifierPlaneCount == 1 &&
             (props[i].drmFormatModifierTilingFeatures & need) == need &&
             props[i].drmFormatModifier != 0)
            mod_candidates[mod_count++] = props[i].drmFormatModifier;
      }
   }
   if (!mod_count)
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
   /* The exported dma_buf keeps the underlying allocation alive on NVIDIA;
    * the VkImage/VkDeviceMemory handles themselves are transient. */
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
   /* experiment control: udmabuf-first (NVIDIA-linear path suspected of
    * breaking hwcomposer's own SW buffers) */
{
   const uint32_t bpp = drm_format_bpp(drm_format);
   const uint32_t stride = (uint32_t)ALLOC_ALIGN((uint64_t)width * bpp, 256);
   const uint64_t size = ALLOC_ALIGN((uint64_t)stride * height, 4096);

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
      close(memfd);
      return -errno;
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
}
