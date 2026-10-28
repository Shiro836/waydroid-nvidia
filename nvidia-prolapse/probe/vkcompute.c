/* vkcompute: minimal headless Vulkan compute probe.
 * Picks the GPU by vendor ID (NVIDIA 0x10de) never by node path, runs one
 * dispatch and verifies the result. Host ground truth for P0 (strace/LD_DEBUG),
 * and the body of the in-guest nvprobe in P1. Exit 0 only on a verified result. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "compute_spv.h"
#include <dlfcn.h>
#include <hardware/hwvulkan.h> /* pinned AOSP copy: the driver repository's hal/include/android */

/* Bootstraps for the same ICD entry points:
 *   loader      : libvulkan.so.1 -> ICD manifest -> libGLX_nvidia.so.0 (NVIDIA's shipped front)
 *   --icd PATH  : dlopen PATH and use the Khronos loader<->ICD interface directly
 *                 (vk_icdNegotiateLoaderICDInterfaceVersion + vk_icdGetInstanceProcAddr).
 *                 libEGL_nvidia.so.0 exports that interface itself (readelf, 615.71.09);
 *                 headless, the GLX front reaches it through EGL (Ghidra, libGLX_nvidia
 *                 FUN_0019e600..FUN_0019ff80), so this is the same driver with no
 *                 libvulkan, libGLX_nvidia, glcore, libnvidia-tls, glvnd or X11.
 *   --sphal NAME: (Android only) load NAME through android_load_sphal_library(), i.e. by
 *                 soname from the "sphal" linker namespace, exactly the call AOSP's
 *                 libvulkan uses for vulkan.<hw>.so. No LD_LIBRARY_PATH, no absolute path:
 *                 the ICD, its glibc-named dependencies and the shims must all be found by
 *                 the namespace's own search paths (P2 gate).
 *   --hal X     : load the Android Vulkan HAL (vulkan.<hw>.so, the driver repository's hal/) the way AOSP's libvulkan
 *                 does (driver.cpp:189-209,271-273): host: dlopen(X); Android:
 *                 android_load_sphal_library(X). dlsym "HMI", check id "vulkan", write
 *                 HMI.common.dso, open("vk0") and use the hwvulkan_device_t's
 *                 GetInstanceProcAddr as gipa. Exercises the HAL's dispatch, not just the ICD. */
typedef PFN_vkVoidFunction (*PFN_vk_icdGetInstanceProcAddr)(VkInstance, const char *);
typedef VkResult (*PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(uint32_t *);
typedef void *(*PFN_glGetVkProcAddrNV)(const char *);
static PFN_vk_icdGetInstanceProcAddr gipa;
static PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
static PFN_vkAllocateDescriptorSets p_vkAllocateDescriptorSets;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
static PFN_vkBindBufferMemory p_vkBindBufferMemory;
static PFN_vkCmdBindDescriptorSets p_vkCmdBindDescriptorSets;
static PFN_vkCmdBindPipeline p_vkCmdBindPipeline;
static PFN_vkCmdDispatch p_vkCmdDispatch;
static PFN_vkCreateBuffer p_vkCreateBuffer;
static PFN_vkCreateCommandPool p_vkCreateCommandPool;
static PFN_vkCreateComputePipelines p_vkCreateComputePipelines;
static PFN_vkCreateDescriptorPool p_vkCreateDescriptorPool;
static PFN_vkCreateDescriptorSetLayout p_vkCreateDescriptorSetLayout;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkCreatePipelineLayout p_vkCreatePipelineLayout;
static PFN_vkCreateShaderModule p_vkCreateShaderModule;
static PFN_vkDestroyBuffer p_vkDestroyBuffer;
static PFN_vkDestroyCommandPool p_vkDestroyCommandPool;
static PFN_vkDestroyDescriptorPool p_vkDestroyDescriptorPool;
static PFN_vkDestroyDescriptorSetLayout p_vkDestroyDescriptorSetLayout;
static PFN_vkDestroyDevice p_vkDestroyDevice;
static PFN_vkDestroyFence p_vkDestroyFence;
static PFN_vkDestroyInstance p_vkDestroyInstance;
static PFN_vkDestroyPipeline p_vkDestroyPipeline;
static PFN_vkDestroyPipelineLayout p_vkDestroyPipelineLayout;
static PFN_vkDestroyShaderModule p_vkDestroyShaderModule;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
static PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements;
static PFN_vkGetDeviceQueue p_vkGetDeviceQueue;
static PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkMapMemory p_vkMapMemory;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkUnmapMemory p_vkUnmapMemory;
static PFN_vkUpdateDescriptorSets p_vkUpdateDescriptorSets;
static PFN_vkWaitForFences p_vkWaitForFences;

#define N 4096u
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

static uint32_t find_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no suitable memory type\n"); exit(2);
}

static void bootstrap_loader(void) {
    void *h = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen libvulkan.so.1: %s\n", dlerror()); exit(2); }
    gipa = (PFN_vk_icdGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) { fprintf(stderr, "no vkGetInstanceProcAddr in loader\n"); exit(2); }
}

static void icd_entry_points(void *h, const char *what);
static void bootstrap_icd(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); exit(2); }
    icd_entry_points(h, path);
}

static void icd_entry_points(void *h, const char *what) {
    PFN_vk_icdNegotiateLoaderICDInterfaceVersion neg = (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    gipa = (PFN_vk_icdGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!neg || !gipa) { fprintf(stderr, "%s: vk_icd* entry points missing\n", what); exit(2); }
    uint32_t ver = 5; VkResult r = neg(&ver);
    if (r != VK_SUCCESS) { fprintf(stderr, "ICD interface negotiation failed: %d\n", r); exit(2); }
    fprintf(stderr, "icd: %s, loader interface version %u\n", what, ver);
}

#ifdef __ANDROID__
/* libvndksupport is a platform library (no NDK header); this is its public prototype
 * (system/core/libvndksupport/include/vndksupport/linker.h). It resolves the "sphal"
 * namespace with android_get_exported_namespace() and dlopens NAME in it. */
typedef void *(*PFN_android_load_sphal_library)(const char *name, int flag);
static void *sphal_dlopen(const char *name) {
    void *vndk = dlopen("libvndksupport.so", RTLD_NOW | RTLD_LOCAL);
    if (!vndk) { fprintf(stderr, "dlopen libvndksupport.so: %s (this process is not in a linker section that can see it)\n", dlerror()); exit(2); }
    PFN_android_load_sphal_library load = (PFN_android_load_sphal_library)dlsym(vndk, "android_load_sphal_library");
    if (!load) { fprintf(stderr, "libvndksupport.so has no android_load_sphal_library\n"); exit(2); }
    void *h = load(name, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "android_load_sphal_library(%s): %s\n", name, dlerror()); exit(2); }
    return h;
}
static void bootstrap_sphal(const char *name) {
    void *h = sphal_dlopen(name);
    Dl_info di; PFN_vk_icdGetInstanceProcAddr p = (PFN_vk_icdGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (p && dladdr((void *)p, &di) && di.dli_fname) fprintf(stderr, "sphal: %s resolved to %s\n", name, di.dli_fname);
    icd_entry_points(h, name);
}
#endif

static void bootstrap_hal(const char *what) {
#ifdef __ANDROID__
    void *h = sphal_dlopen(what);
#else
    void *h = dlopen(what, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", what, dlerror()); exit(2); }
#endif
    hwvulkan_module_t *hmi = (hwvulkan_module_t *)dlsym(h, HAL_MODULE_INFO_SYM_AS_STR);
    if (!hmi) { fprintf(stderr, "%s: no %s symbol\n", what, HAL_MODULE_INFO_SYM_AS_STR); exit(2); }
    if (!hmi->common.id || strcmp(hmi->common.id, HWVULKAN_HARDWARE_MODULE_ID) != 0) {
        fprintf(stderr, "%s: HMI id '%s', expected '%s'\n", what, hmi->common.id ? hmi->common.id : "(null)", HWVULKAN_HARDWARE_MODULE_ID); exit(2);
    }
    hmi->common.dso = h; /* the loader does this (driver.cpp:209): HMI must be writable */
    hwvulkan_device_t *dev = NULL;
    int rc = hmi->common.methods->open(&hmi->common, HWVULKAN_DEVICE_0, (hw_device_t **)&dev);
    if (rc != 0 || !dev) { fprintf(stderr, "%s: open(\"%s\") -> %d\n", what, HWVULKAN_DEVICE_0, rc); exit(2); }
    if (!dev->GetInstanceProcAddr || !dev->CreateInstance || !dev->EnumerateInstanceExtensionProperties || !dev->common.close) {
        fprintf(stderr, "%s: hwvulkan_device_t has NULL entry points\n", what); exit(2);
    }
    gipa = (PFN_vk_icdGetInstanceProcAddr)dev->GetInstanceProcAddr;
#ifdef __ANDROID__
    Dl_info di;
    if (dladdr((void *)hmi, &di) && di.dli_fname) fprintf(stderr, "sphal: %s resolved to %s\n", what, di.dli_fname);
#endif
    fprintf(stderr, "hal: %s: module '%s' by '%s', device \"%s\" opened\n", what, hmi->common.name,
            hmi->common.author, HWVULKAN_DEVICE_0);
}

int main(int argc, char **argv) {
    uint32_t want_vendor = 0x10de;
    const char *icd = NULL, *sphal = NULL, *hal = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--icd") && i + 1 < argc) icd = argv[++i];
        else if (!strcmp(argv[i], "--sphal") && i + 1 < argc) sphal = argv[++i];
        else if (!strcmp(argv[i], "--hal") && i + 1 < argc) hal = argv[++i];
        else if (!strcmp(argv[i], "--loader")) icd = NULL;
        else want_vendor = (uint32_t)strtoul(argv[i], NULL, 0);
    }
    if (hal) {
        bootstrap_hal(hal);
    } else if (sphal) {
#ifdef __ANDROID__
        bootstrap_sphal(sphal);
#else
        fprintf(stderr, "--sphal is only meaningful inside Android (bionic)\n"); return 2;
#endif
    } else if (icd) bootstrap_icd(icd); else bootstrap_loader();
    p_vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!p_vkCreateInstance) { fprintf(stderr, "no vkCreateInstance\n"); return 2; }

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkcompute", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst; CHECK(p_vkCreateInstance(&ici, NULL, &inst));
    p_vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)gipa(inst, "vkAllocateCommandBuffers"); if (!p_vkAllocateCommandBuffers) { fprintf(stderr, "gipa: %s missing\n", "vkAllocateCommandBuffers"); exit(2); }
    p_vkAllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)gipa(inst, "vkAllocateDescriptorSets"); if (!p_vkAllocateDescriptorSets) { fprintf(stderr, "gipa: %s missing\n", "vkAllocateDescriptorSets"); exit(2); }
    p_vkAllocateMemory = (PFN_vkAllocateMemory)gipa(inst, "vkAllocateMemory"); if (!p_vkAllocateMemory) { fprintf(stderr, "gipa: %s missing\n", "vkAllocateMemory"); exit(2); }
    p_vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)gipa(inst, "vkBeginCommandBuffer"); if (!p_vkBeginCommandBuffer) { fprintf(stderr, "gipa: %s missing\n", "vkBeginCommandBuffer"); exit(2); }
    p_vkBindBufferMemory = (PFN_vkBindBufferMemory)gipa(inst, "vkBindBufferMemory"); if (!p_vkBindBufferMemory) { fprintf(stderr, "gipa: %s missing\n", "vkBindBufferMemory"); exit(2); }
    p_vkCmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)gipa(inst, "vkCmdBindDescriptorSets"); if (!p_vkCmdBindDescriptorSets) { fprintf(stderr, "gipa: %s missing\n", "vkCmdBindDescriptorSets"); exit(2); }
    p_vkCmdBindPipeline = (PFN_vkCmdBindPipeline)gipa(inst, "vkCmdBindPipeline"); if (!p_vkCmdBindPipeline) { fprintf(stderr, "gipa: %s missing\n", "vkCmdBindPipeline"); exit(2); }
    p_vkCmdDispatch = (PFN_vkCmdDispatch)gipa(inst, "vkCmdDispatch"); if (!p_vkCmdDispatch) { fprintf(stderr, "gipa: %s missing\n", "vkCmdDispatch"); exit(2); }
    p_vkCreateBuffer = (PFN_vkCreateBuffer)gipa(inst, "vkCreateBuffer"); if (!p_vkCreateBuffer) { fprintf(stderr, "gipa: %s missing\n", "vkCreateBuffer"); exit(2); }
    p_vkCreateCommandPool = (PFN_vkCreateCommandPool)gipa(inst, "vkCreateCommandPool"); if (!p_vkCreateCommandPool) { fprintf(stderr, "gipa: %s missing\n", "vkCreateCommandPool"); exit(2); }
    p_vkCreateComputePipelines = (PFN_vkCreateComputePipelines)gipa(inst, "vkCreateComputePipelines"); if (!p_vkCreateComputePipelines) { fprintf(stderr, "gipa: %s missing\n", "vkCreateComputePipelines"); exit(2); }
    p_vkCreateDescriptorPool = (PFN_vkCreateDescriptorPool)gipa(inst, "vkCreateDescriptorPool"); if (!p_vkCreateDescriptorPool) { fprintf(stderr, "gipa: %s missing\n", "vkCreateDescriptorPool"); exit(2); }
    p_vkCreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout)gipa(inst, "vkCreateDescriptorSetLayout"); if (!p_vkCreateDescriptorSetLayout) { fprintf(stderr, "gipa: %s missing\n", "vkCreateDescriptorSetLayout"); exit(2); }
    p_vkCreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"); if (!p_vkCreateDevice) { fprintf(stderr, "gipa: %s missing\n", "vkCreateDevice"); exit(2); }
    p_vkCreateFence = (PFN_vkCreateFence)gipa(inst, "vkCreateFence"); if (!p_vkCreateFence) { fprintf(stderr, "gipa: %s missing\n", "vkCreateFence"); exit(2); }
    p_vkCreatePipelineLayout = (PFN_vkCreatePipelineLayout)gipa(inst, "vkCreatePipelineLayout"); if (!p_vkCreatePipelineLayout) { fprintf(stderr, "gipa: %s missing\n", "vkCreatePipelineLayout"); exit(2); }
    p_vkCreateShaderModule = (PFN_vkCreateShaderModule)gipa(inst, "vkCreateShaderModule"); if (!p_vkCreateShaderModule) { fprintf(stderr, "gipa: %s missing\n", "vkCreateShaderModule"); exit(2); }
    p_vkDestroyBuffer = (PFN_vkDestroyBuffer)gipa(inst, "vkDestroyBuffer"); if (!p_vkDestroyBuffer) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyBuffer"); exit(2); }
    p_vkDestroyCommandPool = (PFN_vkDestroyCommandPool)gipa(inst, "vkDestroyCommandPool"); if (!p_vkDestroyCommandPool) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyCommandPool"); exit(2); }
    p_vkDestroyDescriptorPool = (PFN_vkDestroyDescriptorPool)gipa(inst, "vkDestroyDescriptorPool"); if (!p_vkDestroyDescriptorPool) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyDescriptorPool"); exit(2); }
    p_vkDestroyDescriptorSetLayout = (PFN_vkDestroyDescriptorSetLayout)gipa(inst, "vkDestroyDescriptorSetLayout"); if (!p_vkDestroyDescriptorSetLayout) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyDescriptorSetLayout"); exit(2); }
    p_vkDestroyDevice = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice"); if (!p_vkDestroyDevice) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyDevice"); exit(2); }
    p_vkDestroyFence = (PFN_vkDestroyFence)gipa(inst, "vkDestroyFence"); if (!p_vkDestroyFence) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyFence"); exit(2); }
    p_vkDestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"); if (!p_vkDestroyInstance) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyInstance"); exit(2); }
    p_vkDestroyPipeline = (PFN_vkDestroyPipeline)gipa(inst, "vkDestroyPipeline"); if (!p_vkDestroyPipeline) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyPipeline"); exit(2); }
    p_vkDestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)gipa(inst, "vkDestroyPipelineLayout"); if (!p_vkDestroyPipelineLayout) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyPipelineLayout"); exit(2); }
    p_vkDestroyShaderModule = (PFN_vkDestroyShaderModule)gipa(inst, "vkDestroyShaderModule"); if (!p_vkDestroyShaderModule) { fprintf(stderr, "gipa: %s missing\n", "vkDestroyShaderModule"); exit(2); }
    p_vkEndCommandBuffer = (PFN_vkEndCommandBuffer)gipa(inst, "vkEndCommandBuffer"); if (!p_vkEndCommandBuffer) { fprintf(stderr, "gipa: %s missing\n", "vkEndCommandBuffer"); exit(2); }
    p_vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"); if (!p_vkEnumeratePhysicalDevices) { fprintf(stderr, "gipa: %s missing\n", "vkEnumeratePhysicalDevices"); exit(2); }
    p_vkFreeMemory = (PFN_vkFreeMemory)gipa(inst, "vkFreeMemory"); if (!p_vkFreeMemory) { fprintf(stderr, "gipa: %s missing\n", "vkFreeMemory"); exit(2); }
    p_vkGetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)gipa(inst, "vkGetBufferMemoryRequirements"); if (!p_vkGetBufferMemoryRequirements) { fprintf(stderr, "gipa: %s missing\n", "vkGetBufferMemoryRequirements"); exit(2); }
    p_vkGetDeviceQueue = (PFN_vkGetDeviceQueue)gipa(inst, "vkGetDeviceQueue"); if (!p_vkGetDeviceQueue) { fprintf(stderr, "gipa: %s missing\n", "vkGetDeviceQueue"); exit(2); }
    p_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"); if (!p_vkGetPhysicalDeviceMemoryProperties) { fprintf(stderr, "gipa: %s missing\n", "vkGetPhysicalDeviceMemoryProperties"); exit(2); }
    p_vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties"); if (!p_vkGetPhysicalDeviceProperties) { fprintf(stderr, "gipa: %s missing\n", "vkGetPhysicalDeviceProperties"); exit(2); }
    p_vkGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"); if (!p_vkGetPhysicalDeviceQueueFamilyProperties) { fprintf(stderr, "gipa: %s missing\n", "vkGetPhysicalDeviceQueueFamilyProperties"); exit(2); }
    p_vkMapMemory = (PFN_vkMapMemory)gipa(inst, "vkMapMemory"); if (!p_vkMapMemory) { fprintf(stderr, "gipa: %s missing\n", "vkMapMemory"); exit(2); }
    p_vkQueueSubmit = (PFN_vkQueueSubmit)gipa(inst, "vkQueueSubmit"); if (!p_vkQueueSubmit) { fprintf(stderr, "gipa: %s missing\n", "vkQueueSubmit"); exit(2); }
    p_vkUnmapMemory = (PFN_vkUnmapMemory)gipa(inst, "vkUnmapMemory"); if (!p_vkUnmapMemory) { fprintf(stderr, "gipa: %s missing\n", "vkUnmapMemory"); exit(2); }
    p_vkUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)gipa(inst, "vkUpdateDescriptorSets"); if (!p_vkUpdateDescriptorSets) { fprintf(stderr, "gipa: %s missing\n", "vkUpdateDescriptorSets"); exit(2); }
    p_vkWaitForFences = (PFN_vkWaitForFences)gipa(inst, "vkWaitForFences"); if (!p_vkWaitForFences) { fprintf(stderr, "gipa: %s missing\n", "vkWaitForFences"); exit(2); }

    uint32_t n = 0; CHECK(p_vkEnumeratePhysicalDevices(inst, &n, NULL));
    if (!n) { fprintf(stderr, "no physical devices\n"); return 2; }
    VkPhysicalDevice *pds = calloc(n, sizeof *pds); CHECK(p_vkEnumeratePhysicalDevices(inst, &n, pds));
    VkPhysicalDevice pd = VK_NULL_HANDLE; VkPhysicalDeviceProperties props;
    for (uint32_t i = 0; i < n; i++) {
        p_vkGetPhysicalDeviceProperties(pds[i], &props);
        fprintf(stderr, "device %u: %s vendor=0x%04x driver=%u.%u.%u api=%u.%u.%u\n", i, props.deviceName, props.vendorID,
            VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion),
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
        if (pd == VK_NULL_HANDLE && props.vendorID == want_vendor) pd = pds[i];
    }
    if (pd == VK_NULL_HANDLE) { fprintf(stderr, "no device with vendor 0x%04x\n", want_vendor); return 2; }
    p_vkGetPhysicalDeviceProperties(pd, &props);

    uint32_t qn = 0; p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf); p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qi = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qi = i; break; }
    if (qi == UINT32_MAX) { fprintf(stderr, "no compute queue\n"); return 2; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; CHECK(p_vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; p_vkGetDeviceQueue(dev, qi, 0, &q);

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = N * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf; CHECK(p_vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr; p_vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = find_mem(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem; CHECK(p_vkAllocateMemory(dev, &mai, NULL, &mem));
    CHECK(p_vkBindBufferMemory(dev, buf, mem, 0));
    uint32_t *p; CHECK(p_vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p));
    for (uint32_t i = 0; i < N; i++) p[i] = i;

    VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
    VkDescriptorSetLayout dsl; CHECK(p_vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl; CHECK(p_vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof compute_spv, .pCode = (const uint32_t *)compute_spv };
    VkShaderModule sm; CHECK(p_vkCreateShaderModule(dev, &smci, NULL, &sm));
    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" }, .layout = pl };
    VkPipeline pipe; CHECK(p_vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; CHECK(p_vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds; CHECK(p_vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi = { .buffer = buf, .range = VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
    p_vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qi };
    VkCommandPool cp; CHECK(p_vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; CHECK(p_vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(p_vkBeginCommandBuffer(cb, &cbbi));
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    p_vkCmdDispatch(cb, N / 64, 1, 1);
    CHECK(p_vkEndCommandBuffer(cb));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CHECK(p_vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(p_vkQueueSubmit(q, 1, &si, fence));
    CHECK(p_vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));

    uint32_t bad = 0;
    for (uint32_t i = 0; i < N; i++) if (p[i] != i * 2u + 1u) bad++;
    printf("vkcompute: %s, %u elements, %u wrong -> %s\n", props.deviceName, N, bad, bad ? "FAIL" : "OK");

    if (getenv("VKCOMPUTE_HOLD")) { fprintf(stderr, "holding (pid %d), press enter\n", (int)getpid()); getchar(); }
    p_vkDestroyFence(dev, fence, NULL); p_vkDestroyCommandPool(dev, cp, NULL); p_vkDestroyDescriptorPool(dev, dp, NULL);
    p_vkDestroyPipeline(dev, pipe, NULL); p_vkDestroyShaderModule(dev, sm, NULL); p_vkDestroyPipelineLayout(dev, pl, NULL);
    p_vkDestroyDescriptorSetLayout(dev, dsl, NULL); p_vkUnmapMemory(dev, mem); p_vkFreeMemory(dev, mem, NULL); p_vkDestroyBuffer(dev, buf, NULL);
    p_vkDestroyDevice(dev, NULL); p_vkDestroyInstance(inst, NULL);
    return bad ? 1 : 0;
}
