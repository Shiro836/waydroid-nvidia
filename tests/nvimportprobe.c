// nvimportprobe — standalone NVIDIA dmabuf import-quirk probe for the
// waydroid-nvidia stack. Reproduces the guest AHB wrap chain (the one behind
// SurfaceFlinger's "Unable to generate SkSurface" abort) directly against the
// host driver, no venus/server needed:
//
//   allocate VkImage (block-linear or LINEAR modifier, gralloc-style usage)
//   -> export dma_buf (vkGetMemoryFdKHR)          [requires open kernel module]
//   -> vkGetMemoryFdPropertiesKHR                  (print memoryTypeBits mask)
//   -> re-create image with ANGLE's usage set (with/without INPUT_ATTACHMENT)
//   -> dedicated-import the dma_buf into EVERY type in the mask, bind
//   -> print a PASS/FAIL(VkResult) table per (modifier, usage, memory type)
//
// The 5090/610.172 baseline: block-linear imports into type 0 with
// INPUT_ATTACHMENT succeed; LINEAR + INPUT_ATTACHMENT fails (worked around
// in guest mesa). Run this on other GPU generations to see which
// combination their driver rejects.
//
// Build:  gcc -O1 -o nvimportprobe nvimportprobe.c -ldl
// Run:    ./nvimportprobe            (picks the first NVIDIA device)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 1024
#define H 1024
#define FMT VK_FORMAT_R8G8B8A8_UNORM
#define MAX_MODS 128

// gralloc GPU-buffer creation usage (vtest_gpu_alloc.c allocates with these)
#define USAGE_ALLOC (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | \
                     VK_IMAGE_USAGE_SAMPLED_BIT | \
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | \
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT)
// ANGLE's import-side usage for a sampled+renderable AHB
#define USAGE_ANGLE_BASE USAGE_ALLOC
#define USAGE_ANGLE_INPUT (USAGE_ANGLE_BASE | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define IPROC(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DPROC(dev, name)  PFN_##name name = (PFN_##name)gdpa(dev, #name)

static const char *rstr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "OK";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
        return "INVALID_DRM_MODIFIER_LAYOUT";
    case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "VkResult(%d)", r);
        return buf;
    }}
}

struct exported {
    int fd;                 // dma_buf
    uint64_t modifier;
    uint64_t size;
    uint32_t export_type;   // memory type the source was allocated from
};

int main(void) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) { fprintf(stderr, "FATAL: no libvulkan (install vulkan loader)\n"); return 2; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");

    IPROC(NULL, vkCreateInstance);
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };
    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r) { fprintf(stderr, "FATAL: vkCreateInstance %s\n", rstr(r)); return 2; }

    IPROC(inst, vkEnumeratePhysicalDevices);
    IPROC(inst, vkGetPhysicalDeviceProperties2);
    IPROC(inst, vkGetPhysicalDeviceMemoryProperties);
    IPROC(inst, vkGetPhysicalDeviceFormatProperties2);
    IPROC(inst, vkGetPhysicalDeviceImageFormatProperties2);
    IPROC(inst, vkGetPhysicalDeviceQueueFamilyProperties);
    IPROC(inst, vkCreateDevice);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");

    VkPhysicalDevice pds[8]; uint32_t npd = 8;
    vkEnumeratePhysicalDevices(inst, &npd, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < npd; i++) {
        VkPhysicalDeviceDriverProperties drv = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
        vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (drv.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
            pd = pds[i];
            printf("GPU: %s  driver: %s %s  vk: %u.%u.%u\n",
                   p2.properties.deviceName, drv.driverName, drv.driverInfo,
                   VK_API_VERSION_MAJOR(p2.properties.apiVersion),
                   VK_API_VERSION_MINOR(p2.properties.apiVersion),
                   VK_API_VERSION_PATCH(p2.properties.apiVersion));
            break;
        }
    }
    if (!pd) { fprintf(stderr, "FATAL: no NVIDIA proprietary Vulkan device\n"); return 2; }

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    printf("memory types:");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        printf(" %u:{heap%u,flags0x%x}", i, mp.memoryTypes[i].heapIndex,
               mp.memoryTypes[i].propertyFlags);
    printf("\n");

    // modifier list for FMT
    VkDrmFormatModifierPropertiesListEXT modlist = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 fp2 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                                .pNext = &modlist };
    vkGetPhysicalDeviceFormatProperties2(pd, FMT, &fp2);
    VkDrmFormatModifierPropertiesEXT mods[MAX_MODS];
    modlist.drmFormatModifierCount =
        modlist.drmFormatModifierCount > MAX_MODS ? MAX_MODS : modlist.drmFormatModifierCount;
    modlist.pDrmFormatModifierProperties = mods;
    vkGetPhysicalDeviceFormatProperties2(pd, FMT, &fp2);
    printf("modifiers advertised: %u\n", modlist.drmFormatModifierCount);

    IPROC(inst, vkGetPhysicalDeviceFeatures);
    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(pd, &feats);
    printf("host feature textureCompressionETC2: %s (Venus %s compute shader emulation)\n",
           feats.textureCompressionETC2 ? "YES" : "NO",
           feats.textureCompressionETC2 ? "unneeded" : "requires");

    const char *want_exts[] = {
        "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list",
        "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2",
        "VK_KHR_bind_memory2",
    };
    // request only what the driver advertises (promoted-to-core exts may be
    // absent from the list on some driver branches); report what's missing
    IPROC(inst, vkEnumerateDeviceExtensionProperties);
    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &next, NULL);
    VkExtensionProperties *eps = calloc(next, sizeof(*eps));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &next, eps);
    const char *dev_exts[7]; uint32_t ndev_exts = 0;
    int has_dmabuf_ext = 0;
    for (int i = 0; i < 7; i++) {
        int found = 0;
        for (uint32_t j = 0; j < next; j++)
            if (!strcmp(eps[j].extensionName, want_exts[i])) { found = 1; break; }
        if (found) {
            dev_exts[ndev_exts++] = want_exts[i];
            if (i == 1) has_dmabuf_ext = 1;
        } else
            printf("note: device lacks %s (relying on core)\n", want_exts[i]);
    }
    free(eps);
    if (!has_dmabuf_ext) {
        printf("== UNTESTABLE: driver exposes no VK_EXT_external_memory_dma_buf.\n"
               "   Usual cause: nvidia-drm.modeset=0 on the host, or closed "
               "kernel module.\n   This host cannot run waydroid-nvidia either "
               "way — pick another.\n");
        return 3;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = 0, .queueCount = 1,
                                    .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = ndev_exts,
                               .ppEnabledExtensionNames = dev_exts };
    VkDevice dev;
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r) { fprintf(stderr, "FATAL: vkCreateDevice %s\n", rstr(r)); return 2; }

    DPROC(dev, vkCreateImage); DPROC(dev, vkDestroyImage);
    DPROC(dev, vkAllocateMemory); DPROC(dev, vkFreeMemory);
    DPROC(dev, vkGetImageMemoryRequirements);
    DPROC(dev, vkBindImageMemory);
    DPROC(dev, vkGetMemoryFdKHR);
    DPROC(dev, vkGetMemoryFdPropertiesKHR);

    // pick the "alloc" modifier the way vtest_gpu_alloc does: first modifier
    // that supports our usage as an optimal image; prefer non-linear.
    uint64_t alloc_mods[2] = { UINT64_MAX, UINT64_MAX }; // [0]=block-linear [1]=LINEAR
    for (uint32_t i = 0; i < modlist.drmFormatModifierCount; i++) {
        uint64_t m = mods[i].drmFormatModifier;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = m, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VkPhysicalDeviceExternalImageFormatInfo ei = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .pNext = &mi,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkPhysicalDeviceImageFormatInfo2 ii = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &ei, .format = FMT, .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = USAGE_ALLOC };
        VkExternalImageFormatProperties ep = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkImageFormatProperties2 ip = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ep };
        if (vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip) != VK_SUCCESS)
            continue;
        if (!(ep.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            continue;
        if (m == 0 /* DRM_FORMAT_MOD_LINEAR */) {
            if (alloc_mods[1] == UINT64_MAX) alloc_mods[1] = m;
        } else {
            if (alloc_mods[0] == UINT64_MAX) alloc_mods[0] = m;
        }
    }
    printf("chosen modifiers: block-linear=0x%" PRIx64 " linear=%s\n",
           alloc_mods[0],
           alloc_mods[1] == UINT64_MAX ? "unsupported" : "0x0");

    int failures = 0, tested = 0;
    for (int mi = 0; mi < 2; mi++) {
        uint64_t mod = alloc_mods[mi];
        const char *modname = mi == 0 ? "BLOCK-LINEAR" : "LINEAR";
        if (mod == UINT64_MAX) { printf("[%s] skipped (no exportable modifier)\n", modname); continue; }
        tested++;

        // --- allocate + export the source image ---
        VkImageDrmFormatModifierListCreateInfoEXT mlci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
            .drmFormatModifierCount = 1, .pDrmFormatModifiers = &mod };
        VkExternalMemoryImageCreateInfo emi = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &mlci,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkImageCreateInfo ici2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D, .format = FMT,
            .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = USAGE_ALLOC, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImage src;
        r = vkCreateImage(dev, &ici2, NULL, &src);
        if (r) { printf("[%s] FAIL create src: %s\n", modname, rstr(r)); failures++; continue; }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, src, &req);
        uint32_t at = 0;
        while (!((req.memoryTypeBits >> at) & 1)) at++;   // any type is fine for the source
        // prefer DEVICE_LOCAL for the source (that's where gralloc buffers live)
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
            if (((req.memoryTypeBits >> t) & 1) &&
                (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { at = t; break; }
        VkExportMemoryAllocateInfo exp = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkMemoryDedicatedAllocateInfo ded = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &exp,
            .image = src };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &ded, .allocationSize = req.size, .memoryTypeIndex = at };
        VkDeviceMemory smem;
        r = vkAllocateMemory(dev, &mai, NULL, &smem);
        if (r) { printf("[%s] FAIL alloc src (type %u): %s\n", modname, at, rstr(r)); failures++; continue; }
        vkBindImageMemory(dev, src, smem, 0);
        VkMemoryGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = smem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        int fd = -1;
        r = vkGetMemoryFdKHR(dev, &gfi, &fd);
        if (r) {
            printf("[%s] FAIL export dma_buf: %s  <-- closed kernel module? "
                   "(open KM required)\n", modname, rstr(r));
            failures++; continue;
        }
        printf("[%s] source: alloc type %u, size %" PRIu64 ", exported fd ok\n",
               modname, at, (uint64_t)req.size);

        // --- what does the driver say this fd can import into? ---
        VkMemoryFdPropertiesKHR fdp = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
        r = vkGetMemoryFdPropertiesKHR(dev,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp);
        printf("[%s] GetMemoryFdProperties: %s mask=0x%x (ffs=%d)\n", modname,
               rstr(r), fdp.memoryTypeBits,
               fdp.memoryTypeBits ? __builtin_ffs(fdp.memoryTypeBits) - 1 : -1);

        // --- import matrix: usage x memory-type ---
        const struct { const char *name; VkImageUsageFlags usage; } usages[] = {
            { "angle+INPUT_ATTACHMENT", USAGE_ANGLE_INPUT },
            { "angle-base            ", USAGE_ANGLE_BASE },
        };
        for (int u = 0; u < 2; u++) {
            VkImage dst;
            VkImageCreateInfo dci2 = ici2;
            VkExternalMemoryImageCreateInfo demi = emi;
            VkImageDrmFormatModifierListCreateInfoEXT dmlci = mlci;
            demi.pNext = &dmlci;
            dci2.pNext = &demi;
            dci2.usage = usages[u].usage;
            r = vkCreateImage(dev, &dci2, NULL, &dst);
            if (r) {
                // LINEAR + INPUT_ATTACHMENT rejection is the known 5090/610
                // quirk, already worked around in guest mesa (strip the bit
                // for LINEAR-modifier imports) — expected, don't count it.
                int known = (mi == 1 && u == 0);
                printf("[%s][%s] CreateImage: FAIL %s%s\n", modname,
                       usages[u].name, rstr(r),
                       known ? "  (KNOWN quirk, worked around in guest)" : "");
                if (!known) failures++;
                continue;
            }
            for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
                if (!((fdp.memoryTypeBits >> t) & 1)) continue;
                int dfd = dup(fd);
                VkImportMemoryFdInfoKHR imp = {
                    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                    .fd = dfd };
                VkMemoryDedicatedAllocateInfo dded = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                    .pNext = &imp, .image = dst };
                VkMemoryAllocateInfo dmai = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dded,
                    .allocationSize = req.size, .memoryTypeIndex = t };
                VkDeviceMemory dmem;
                r = vkAllocateMemory(dev, &dmai, NULL, &dmem);
                if (r != VK_SUCCESS) {
                    printf("[%s][%s] import type %u: FAIL %s\n",
                           modname, usages[u].name, t, rstr(r));
                    failures++;
                    close(dfd);
                    continue;
                }
                VkResult br = vkBindImageMemory(dev, dst, dmem, 0);
                printf("[%s][%s] import type %u: alloc OK, bind %s\n",
                       modname, usages[u].name, t, rstr(br));
                if (br != VK_SUCCESS) failures++;
                vkFreeMemory(dev, dmem, NULL);
                // NOTE: image is bound once at most; recreate for next type
                vkDestroyImage(dev, dst, NULL);
                r = vkCreateImage(dev, &dci2, NULL, &dst);
                if (r) { dst = VK_NULL_HANDLE; break; }
            }
            if (dst) vkDestroyImage(dev, dst, NULL);
        }
        close(fd);
        vkDestroyImage(dev, src, NULL);
        vkFreeMemory(dev, smem, NULL);
    }
    if (!tested) {
        printf("== UNTESTABLE: no exportable modifier for RGBA8 — nothing was "
               "actually tested (nvidia-drm.modeset=0? closed KM?)\n");
        return 3;
    }
    printf(failures ? "== %d UNEXPECTED FAILURES (see above) — this likely "
                      "explains the SurfaceFlinger crash on this GPU\n"
                    : "== ALL PASS (stack-relevant import paths work on this GPU)\n",
           failures);
    return failures ? 1 : 0;
}
