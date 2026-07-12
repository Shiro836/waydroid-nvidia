// Can we mmap an NVIDIA dma_buf exported from HOST_VISIBLE memory?
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t n = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (strstr(p.deviceName, "NVIDIA")) { pd = pds[i]; break; }
    }
    if (!pd) return 1;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char *exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = exts;
    VkDevice dev; if (vkCreateDevice(pd, &dci, NULL, &dev)) return 1;

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t mt = 0; mt < mp.memoryTypeCount; mt++) {
        VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL,
                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, 1 << 20, mt};
        VkDeviceMemory mem;
        if (vkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS) {
            printf("type %u (flags 0x%x): alloc-export FAIL\n", mt, mp.memoryTypes[mt].propertyFlags);
            continue;
        }
        PFN_vkGetMemoryFdKHR getFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
        VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem,
                                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        int fd = -1;
        VkResult r = getFd(dev, &gfi, &fd);
        if (r != VK_SUCCESS || fd < 0) {
            printf("type %u (flags 0x%x): getfd FAIL %d\n", mt, mp.memoryTypes[mt].propertyFlags, r);
            vkFreeMemory(dev, mem, NULL);
            continue;
        }
        void *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        printf("type %u (flags 0x%x): export OK, mmap %s\n", mt,
               mp.memoryTypes[mt].propertyFlags, p == MAP_FAILED ? "FAIL" : "OK!!");
        if (p != MAP_FAILED) { ((char*)p)[0] = 42; munmap(p, 1 << 20); }
        close(fd);
        vkFreeMemory(dev, mem, NULL);
    }
    return 0;
}
