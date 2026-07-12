// Does the NVIDIA proprietary driver EXPORT dma_buf from VkDeviceMemory?
// Queries external buffer/image caps for DMA_BUF, then actually allocates
// an exportable image + memory, exports the fd, and EGL-imports it back
// (the exact path KWin would take with an NVIDIA-born client buffer).
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "FAIL %s = %d (line %d)\n", #x, _r, __LINE__); return 1; } } while (0)

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t n = 8; VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (strstr(p.deviceName, "NVIDIA")) { pd = pds[i]; printf("device: %s\n", p.deviceName); break; }
    }
    if (!pd) { fprintf(stderr, "no NVIDIA device\n"); return 1; }

    // image-level external caps for DMA_BUF, optimal + linear
    for (int linear = 0; linear <= 1; linear++) {
        VkPhysicalDeviceExternalImageFormatInfo ext = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 info = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext,
            VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
            linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0};
        VkExternalImageFormatProperties extp = {
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props = {
            VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &extp};
        VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &info, &props);
        printf("%s RGBA8 attach+sample dma_buf: r=%d features=0x%x (export=%s import=%s)\n",
               linear ? "LINEAR " : "OPTIMAL",
               r, extp.externalMemoryProperties.externalMemoryFeatures,
               (extp.externalMemoryProperties.externalMemoryFeatures &
                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? "YES" : "no",
               (extp.externalMemoryProperties.externalMemoryFeatures &
                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "YES" : "no");
    }

    // actually allocate + export
    float prio = 1.0f;
    uint32_t qf = 0;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char *dev_exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
                              "VK_EXT_image_drm_format_modifier"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 3; dci.ppEnabledExtensionNames = dev_exts;
    VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

    VkExternalMemoryImageCreateInfo emi = {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo imgci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
    imgci.imageType = VK_IMAGE_TYPE_2D;
    imgci.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgci.extent = (VkExtent3D){256, 256, 1};
    imgci.mipLevels = 1; imgci.arrayLayers = 1;
    imgci.samples = VK_SAMPLE_COUNT_1_BIT;
    imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img; CHECK(vkCreateImage(dev, &imgci, NULL, &img));

    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL,
                                      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &exp};
    ded.image = img;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, mr.size, mt};
    VkDeviceMemory mem; CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    CHECK(vkBindImageMemory(dev, img, mem, 0));

    PFN_vkGetMemoryFdKHR getFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem,
                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    CHECK(getFd(dev, &gfi, &fd));
    printf("EXPORTED dma_buf fd=%d size=%llu\n", fd, (unsigned long long)mr.size);

    // drm format modifier of the image
    PFN_vkGetImageDrmFormatModifierPropertiesEXT getMod =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
        vkGetDeviceProcAddr(dev, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (getMod) {
        VkImageDrmFormatModifierPropertiesEXT mprop = {
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
        if (getMod(dev, img, &mprop) == VK_SUCCESS)
            printf("image modifier=0x%llx\n", (unsigned long long)mprop.drmFormatModifier);
        else
            printf("modifier query failed (OPTIMAL tiling image)\n");
    }
    printf("PASS\n");
    return 0;
}
