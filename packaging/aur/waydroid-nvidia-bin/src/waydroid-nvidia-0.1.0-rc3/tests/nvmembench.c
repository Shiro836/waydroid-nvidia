// Bandwidth of NVIDIA host-visible memory: vkMapMemory vs mmap(exported
// dma_buf), per memory type. Explains the 27x vtest host-visible slowdown.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SZ (64u << 20)

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double bench_write(void *p) {
    double t0 = now_s();
    memset(p, 0xA5, SZ);
    return SZ / (now_s() - t0) / 1e9;
}
static double bench_read(void *p) {
    static volatile uint64_t sink;
    uint64_t acc = 0; const uint64_t *q = p;
    double t0 = now_s();
    for (size_t i = 0; i < SZ / 8; i += 8) acc += q[i] + q[i+1] + q[i+2] + q[i+3] + q[i+4] + q[i+5] + q[i+6] + q[i+7];
    double t = now_s() - t0;
    sink = acc;
    return SZ / t / 1e9;
}

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t n = 8; VkPhysicalDevice pds[8]; vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (strstr(p.deviceName, "NVIDIA")) { pd = pds[i]; printf("GPU: %s\n", p.deviceName); break; }
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
    PFN_vkGetMemoryFdKHR pGetFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t mt = 0; mt < mp.memoryTypeCount; mt++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[mt].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        printf("type %u [%s%s%s%s]\n", mt,
               f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DEV " : "",
               f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "VIS " : "",
               f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "COH " : "",
               f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "CACHED" : "");
        VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, SZ, mt};
        VkDeviceMemory mem;
        if (vkAllocateMemory(dev, &mai, NULL, &mem)) { printf("  alloc FAIL\n"); continue; }

        void *p = NULL;
        if (!vkMapMemory(dev, mem, 0, SZ, 0, &p) && p) {
            printf("  vkMapMemory: write %.2f GB/s, read %.2f GB/s\n", bench_write(p), bench_read(p));
            vkUnmapMemory(dev, mem);
        } else printf("  vkMapMemory FAIL\n");

        VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        int fd = -1;
        if (!pGetFd(dev, &gfi, &fd) && fd >= 0) {
            void *q = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (q != MAP_FAILED) {
                printf("  mmap(dmabuf): write %.2f GB/s, read %.2f GB/s\n", bench_write(q), bench_read(q));
                munmap(q, SZ);
            } else printf("  mmap(dmabuf) FAIL\n");
            close(fd);
        } else printf("  export FAIL\n");
        vkFreeMemory(dev, mem, NULL);
    }
    return 0;
}
