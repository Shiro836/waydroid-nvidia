/* vkexts: which device extensions can be enabled at vkCreateDevice?
 * Same bootstraps as vkcompute (--loader | --icd PATH | --sphal NAME | --hal PATH|NAME).
 * Modes: --list, --all, --each (default), --bisect, --with NAME[,NAME...],
 *        --hwui, --hwui-bisect; modifier --global-priority {low,medium,high,realtime}.
 * --hwui replays what HWUI's VulkanManager::setupDevice (android13-release) does: ALL instance
 *   extensions enabled at vkCreateInstance (apiVersion 1.1), first queue family with GRAPHICS,
 *   one queue of priority 0.0, all advertised device extensions, VkPhysicalDeviceFeatures2 as
 *   pNext of VkDeviceCreateInfo (filled by vkGetPhysicalDeviceFeatures2 with a chain of
 *   BlendOperationAdvancedFeaturesEXT (only if that ext spec>=2) + SamplerYcbcrConversionFeatures,
 *   robustBufferAccess cleared), pEnabledFeatures = NULL. Prints the raw VkResult + name.
 * --hwui-bisect additionally isolates the failing ingredient (features2 pNext, core features,
 *   each chained struct, individual core features, global priority) and prints a "cause:" line.
 * --global-priority adds VkDeviceQueueGlobalPriorityCreateInfoEXT to the queue create info of any
 *   mode (HWUI only does so when Properties::contextPriority != 0 and the ext is present).
 * Exit: 0 = --all/--hwui ok / no offenders, 1 = failures found, 2 = bootstrap failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#include <hardware/hwvulkan.h> /* pinned AOSP copy: the driver repository's hal/include/android */

typedef PFN_vkVoidFunction (*PFN_vk_icdGetInstanceProcAddr)(VkInstance, const char *);
typedef VkResult (*PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(uint32_t *);
static PFN_vk_icdGetInstanceProcAddr gipa;
static PFN_vkEnumerateInstanceExtensionProperties p_EnumInstExt;
static PFN_vkCreateInstance p_CreateInstance;
static PFN_vkDestroyInstance p_DestroyInstance;
static PFN_vkEnumeratePhysicalDevices p_EnumPD;
static PFN_vkGetPhysicalDeviceProperties p_GetPDProps;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_GetQF;
static PFN_vkEnumerateDeviceExtensionProperties p_EnumDevExt;
static PFN_vkCreateDevice p_CreateDevice;
static PFN_vkDestroyDevice p_DestroyDevice;

static PFN_vkGetPhysicalDeviceFeatures2 p_GetFeatures2;
static VkPhysicalDevice pd;
static int g_prio; /* 0 = none, else a VkQueueGlobalPriorityEXT value */
static const char *g_prio_name = "none";
static uint32_t *specs; /* specVersion per names[] entry */
static uint32_t qfam;
static char (*names)[VK_MAX_EXTENSION_NAME_SIZE];
static uint32_t nexts;
static unsigned long ncreate;

/* ---- bootstraps (copied from vkcompute.c) ---- */
static void bootstrap_loader(void) {
    /* glibc hosts ship libvulkan.so.1; Android's loader is libvulkan.so (no .1). Try both, report both failures. */
    void *h = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!h) {
        char e1[512]; const char *d = dlerror(); snprintf(e1, sizeof e1, "%s", d ? d : "?");
        h = dlopen("libvulkan.so", RTLD_NOW);
        if (!h) { d = dlerror(); fprintf(stderr, "dlopen libvulkan.so.1: %s\ndlopen libvulkan.so: %s\n", e1, d ? d : "?"); exit(2); }
    }
    gipa = (PFN_vk_icdGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) { fprintf(stderr, "no vkGetInstanceProcAddr in loader\n"); exit(2); }
}
static void icd_entry_points(void *h, const char *what) {
    PFN_vk_icdNegotiateLoaderICDInterfaceVersion neg = (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    gipa = (PFN_vk_icdGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!neg || !gipa) { fprintf(stderr, "%s: vk_icd* entry points missing\n", what); exit(2); }
    uint32_t ver = 5; VkResult r = neg(&ver);
    if (r != VK_SUCCESS) { fprintf(stderr, "ICD interface negotiation failed: %d\n", r); exit(2); }
    fprintf(stderr, "icd: %s, loader interface version %u\n", what, ver);
}
static void bootstrap_icd(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); exit(2); }
    icd_entry_points(h, path);
}
#ifdef __ANDROID__
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
    hmi->common.dso = h;
    hwvulkan_device_t *dev = NULL;
    int rc = hmi->common.methods->open(&hmi->common, HWVULKAN_DEVICE_0, (hw_device_t **)&dev);
    if (rc != 0 || !dev) { fprintf(stderr, "%s: open(\"%s\") -> %d\n", what, HWVULKAN_DEVICE_0, rc); exit(2); }
    if (!dev->GetInstanceProcAddr || !dev->CreateInstance || !dev->EnumerateInstanceExtensionProperties || !dev->common.close) {
        fprintf(stderr, "%s: hwvulkan_device_t has NULL entry points\n", what); exit(2);
    }
    gipa = (PFN_vk_icdGetInstanceProcAddr)dev->GetInstanceProcAddr;
    fprintf(stderr, "hal: %s: module '%s' by '%s', device \"%s\" opened\n", what, hmi->common.name, hmi->common.author, HWVULKAN_DEVICE_0);
}

/* ---- VkResult names ---- */
static const char *rname(int r) {
    switch (r) {
#define N(x) case x: return #x;
    N(VK_SUCCESS) N(VK_NOT_READY) N(VK_TIMEOUT) N(VK_INCOMPLETE)
    N(VK_ERROR_OUT_OF_HOST_MEMORY) N(VK_ERROR_OUT_OF_DEVICE_MEMORY) N(VK_ERROR_INITIALIZATION_FAILED)
    N(VK_ERROR_DEVICE_LOST) N(VK_ERROR_MEMORY_MAP_FAILED) N(VK_ERROR_LAYER_NOT_PRESENT)
    N(VK_ERROR_EXTENSION_NOT_PRESENT) N(VK_ERROR_FEATURE_NOT_PRESENT) N(VK_ERROR_INCOMPATIBLE_DRIVER)
    N(VK_ERROR_TOO_MANY_OBJECTS) N(VK_ERROR_FORMAT_NOT_SUPPORTED) N(VK_ERROR_FRAGMENTED_POOL)
    N(VK_ERROR_UNKNOWN) N(VK_ERROR_INVALID_EXTERNAL_HANDLE) N(VK_ERROR_OUT_OF_POOL_MEMORY)
    N(VK_ERROR_SURFACE_LOST_KHR) N(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) N(VK_ERROR_OUT_OF_DATE_KHR)
    N(VK_ERROR_NOT_PERMITTED_KHR) N(VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT)
    N(VK_ERROR_VALIDATION_FAILED_EXT) N(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
#undef N
    default: return "(unnamed VkResult)";
    }
}

/* ---- device creation ---- */
static VkResult try_create(const char *const *list, uint32_t n) {
    float prio = 1.0f;
    VkDeviceQueueGlobalPriorityCreateInfoEXT gp = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT, .globalPriority = (VkQueueGlobalPriorityEXT)g_prio };
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .pNext = g_prio ? &gp : NULL, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = n, .ppEnabledExtensionNames = list };
    VkDevice d = VK_NULL_HANDLE; ncreate++;
    VkResult r = p_CreateDevice(pd, &dci, NULL, &d);
    if (r == VK_SUCCESS && d) p_DestroyDevice(d, NULL);
    return r;
}

/* sets are arrays of indices into names[] */
static VkResult try_idx(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb) {
    const char **l = malloc((na + nb + 1) * sizeof *l);
    for (uint32_t i = 0; i < na; i++) l[i] = names[a[i]];
    for (uint32_t i = 0; i < nb; i++) l[na + i] = names[b[i]];
    VkResult r = try_create(l, na + nb); free(l); return r;
}

/* Minimal subset M of set such that fixed+M fails (precondition: fixed+set fails). Returns |M| in out. */
static uint32_t min_fail(const uint32_t *set, uint32_t n, const uint32_t *fixed, uint32_t nf, uint32_t *out) {
    if (n == 1) { out[0] = set[0]; return 1; }
    if (n == 0) return 0;
    uint32_t h = n / 2;
    const uint32_t *A = set, *B = set + h; uint32_t na = h, nb = n - h;
    if (try_idx(fixed, nf, A, na) != VK_SUCCESS) return min_fail(A, na, fixed, nf, out);
    if (try_idx(fixed, nf, B, nb) != VK_SUCCESS) return min_fail(B, nb, fixed, nf, out);
    /* neither half fails alone: a combination. Minimise A with B fixed, then B with A' fixed. */
    uint32_t *fb = malloc((nf + nb + n) * sizeof *fb), *ma = out, *mb = malloc(n * sizeof *mb);
    memcpy(fb, fixed, nf * sizeof *fb); memcpy(fb + nf, B, nb * sizeof *fb);
    uint32_t ka = min_fail(A, na, fb, nf + nb, ma);
    memcpy(fb, fixed, nf * sizeof *fb); memcpy(fb + nf, ma, ka * sizeof *fb);
    uint32_t kb = min_fail(B, nb, fb, nf + ka, mb);
    memcpy(out + ka, mb, kb * sizeof *mb);
    free(fb); free(mb); return ka + kb;
}

static int bisect(void) {
    uint32_t *rem = malloc(nexts * sizeof *rem), *m = malloc(nexts * sizeof *m); uint32_t nr = nexts, noff = 0;
    for (uint32_t i = 0; i < nexts; i++) rem[i] = i;
    VkResult r = try_idx(rem, nr, NULL, 0);
    printf("bisect: full list (%u) -> %d\n", nr, r);
    while (nr && try_idx(rem, nr, NULL, 0) != VK_SUCCESS) {
        uint32_t k = min_fail(rem, nr, NULL, 0, m);
        VkResult rr = try_idx(m, k, NULL, 0);
        if (k == 1) printf("offender %s %d\n", names[m[0]], rr);
        else {
            printf("offender-set (%u, minimal failing set found, %d):", k, rr);
            for (uint32_t i = 0; i < k; i++) printf(" %s", names[m[i]]);
            printf("\n");
        }
        noff++;
        uint32_t w = 0; /* drop the first member and look for further offenders */
        for (uint32_t i = 0; i < nr; i++) if (rem[i] != m[0]) rem[w++] = rem[i];
        nr = w;
    }
    printf("bisect: %u offender group(s); remaining %u extensions create ok; %lu vkCreateDevice calls\n", noff, nr, ncreate);
    free(rem); free(m); return noff ? 1 : 0;
}


/* ---- HWUI replay (VulkanManager.cpp:116-319) ---- */
#define NCORE ((int)(sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32)))
static const char *const core_names[] = { "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend",
    "geometryShader", "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect",
    "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines", "largePoints",
    "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2", "textureCompressionASTC_LDR",
    "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery", "vertexPipelineStoresAndAtomics",
    "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended",
    "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample", "shaderStorageImageReadWithoutFormat",
    "shaderStorageImageWriteWithoutFormat", "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance", "shaderCullDistance",
    "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency", "shaderResourceMinLod", "sparseBinding",
    "sparseResidencyBuffer", "sparseResidencyImage2D", "sparseResidencyImage3D", "sparseResidency2Samples",
    "sparseResidency4Samples", "sparseResidency8Samples", "sparseResidency16Samples", "sparseResidencyAliased",
    "variableMultisampleRate", "inheritedQueries" };
enum { CH_BLEND = 1, CH_YCBCR = 2 };
static uint32_t hw_qfam;
static VkPhysicalDeviceFeatures q_core;
static VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT q_blend;
static VkPhysicalDeviceSamplerYcbcrConversionFeatures q_ycbcr;
static unsigned q_avail; /* CH_* structs HWUI would chain on this device */
static const char **hw_exts; static uint32_t hw_next;
static int has_ext(const char *nm, uint32_t minspec) {
    for (uint32_t i = 0; i < nexts; i++) if (!strcmp(names[i], nm)) return specs[i] >= minspec;
    return 0;
}
static int count_true(const VkBool32 *b, int n) { int c = 0; for (int i = 0; i < n; i++) c += b[i] != 0; return c; }

/* One vkCreateDevice as HWUI builds it. f2: 0 = no features2 pNext (pEnabledFeatures NULL);
 * 1 = features2 with `core` and the chain structs in `chain` (queried values). prio: 0 = no struct. */
static VkResult hw_try(int f2, unsigned chain, const VkBool32 *core, int prio, int use_exts) {
    float qprio[1] = { 0.0f };
    VkDeviceQueueGlobalPriorityCreateInfoEXT gp = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT, .globalPriority = (VkQueueGlobalPriorityEXT)prio };
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .pNext = prio ? &gp : NULL,
        .queueFamilyIndex = hw_qfam, .queueCount = 1, .pQueuePriorities = qprio };
    VkPhysicalDeviceFeatures2 f = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT bl = q_blend;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures yc = q_ycbcr;
    bl.pNext = NULL; yc.pNext = NULL;
    void **tail = &f.pNext;
    if (chain & CH_BLEND) { *tail = &bl; tail = &bl.pNext; }
    if (chain & CH_YCBCR) { *tail = &yc; tail = &yc.pNext; }
    if (core) memcpy(&f.features, core, sizeof f.features);
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = f2 ? &f : NULL,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = use_exts ? hw_next : 0, .ppEnabledExtensionNames = hw_exts, .pEnabledFeatures = NULL };
    VkDevice d = VK_NULL_HANDLE; ncreate++;
    VkResult r = p_CreateDevice(pd, &dci, NULL, &d);
    if (r == VK_SUCCESS && d) p_DestroyDevice(d, NULL);
    return r;
}

static void hw_attempt(const char *desc, VkResult r) { printf("attempt %s -> %d %s\n", desc, (int)r, rname(r)); }

static const struct { const char *nm; int v; } prios[] = { {"low", VK_QUEUE_GLOBAL_PRIORITY_LOW_EXT}, {"medium", VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT},
    {"high", VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT}, {"realtime", VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT} };

static int core_with(const unsigned char *keep, VkBool32 *out) { /* queried core AND keep[]; returns #TRUE */
    const VkBool32 *q = (const VkBool32 *)&q_core; int c = 0;
    for (int i = 0; i < NCORE; i++) { out[i] = (q[i] && keep[i]) ? VK_TRUE : VK_FALSE; c += out[i] != 0; }
    return c;
}
static VkResult core_try(const unsigned char *keep, unsigned chain, int prio) {
    VkBool32 c[NCORE]; core_with(keep, c); return hw_try(1, chain, c, prio, 1);
}

static int hwui(int bisect_mode) {
    hw_qfam = UINT32_MAX;
    uint32_t qn = 0; p_GetQF(pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn + 1, sizeof *qf); p_GetQF(pd, &qn, qf);
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { hw_qfam = i; break; }
    free(qf);
    if (hw_qfam == UINT32_MAX) { fflush(stdout); fprintf(stderr, "hwui: no GRAPHICS queue family (HWUI would abort)\n"); return 2; }
    /* HWUI aborts without these (VulkanManager.cpp:227,234); warn only: on the host the Android loader's
     * swapchain layer is absent, and the measurement of vkCreateDevice is still wanted. */
    if (!has_ext(VK_KHR_SWAPCHAIN_EXTENSION_NAME, 0)) fprintf(stderr, "hwui: WARNING no VK_KHR_swapchain advertised (HWUI would abort; expected on a bare HAL without the Android loader)\n");
    if (!has_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, 1)) fprintf(stderr, "hwui: WARNING no VK_KHR_external_semaphore_fd (HWUI would abort)\n");
    if (!p_GetFeatures2) { fflush(stdout); fprintf(stderr, "hwui: vkGetPhysicalDeviceFeatures2 not returned by vkGetInstanceProcAddr (HWUI needs it, VulkanManager.cpp:236)\n"); return 2; }
    hw_exts = malloc((nexts + 1) * sizeof *hw_exts);
    for (uint32_t i = 0; i < nexts; i++) hw_exts[i] = names[i];
    hw_next = nexts;

    /* VulkanManager.cpp:236-267 */
    VkPhysicalDeviceFeatures2 f = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    memset(&q_blend, 0, sizeof q_blend); memset(&q_ycbcr, 0, sizeof q_ycbcr);
    q_blend.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT;
    q_ycbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    void **tail = &f.pNext;
    if (has_ext(VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME, 2)) { q_avail |= CH_BLEND; *tail = &q_blend; tail = &q_blend.pNext; }
    q_avail |= CH_YCBCR; *tail = &q_ycbcr; tail = &q_ycbcr.pNext;
    p_GetFeatures2(pd, &f);
    f.features.robustBufferAccess = VK_FALSE;
    q_core = f.features;
    int prio_ext = has_ext(VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME, 2);

    printf("hwui: queue family %u (first with GRAPHICS), 1 queue, priority 0.0; %u device extensions (all advertised)\n", hw_qfam, hw_next);
    printf("hwui: chained VkPhysicalDeviceFeatures2 sType=%d core: %d of %d VkBool32 TRUE (robustBufferAccess forced FALSE):", (int)f.sType, count_true((const VkBool32 *)&q_core, NCORE), NCORE);
    for (int i = 0; i < NCORE; i++) if (((const VkBool32 *)&q_core)[i]) printf(" %s", core_names[i]);
    printf("\n");
    if (q_avail & CH_BLEND) printf("hwui: chained VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT sType=%d TRUE=%d of 1\n", (int)q_blend.sType, count_true(&q_blend.advancedBlendCoherentOperations, 1));
    else printf("hwui: VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT NOT chained (VK_EXT_blend_operation_advanced spec<2 or absent)\n");
    printf("hwui: chained VkPhysicalDeviceSamplerYcbcrConversionFeatures sType=%d TRUE=%d of 1\n", (int)q_ycbcr.sType, count_true(&q_ycbcr.samplerYcbcrConversion, 1));
    printf("hwui: VK_EXT_global_priority spec>=2: %s; global priority struct: %s\n", prio_ext ? "yes" : "no", g_prio ? g_prio_name : "not chained (Properties::contextPriority==0 equivalent)");

    VkBool32 core[NCORE]; memcpy(core, &q_core, sizeof core);
    VkResult full = hw_try(1, q_avail, core, g_prio, 1);
    printf("hwui: vkCreateDevice -> %d %s\n", (int)full, rname(full));
    if (!bisect_mode) return full == VK_SUCCESS ? 0 : 1;

    unsigned char all[NCORE]; memset(all, 1, sizeof all);
    char desc[256];
    int fail_a = 0, fail_b = 0, fail_empty = 0, fail_combo = 0; unsigned bad_chain = 0; VkResult ra, rb;
    /* (a) no features2 pNext */
    ra = hw_try(0, 0, NULL, g_prio, 1); hw_attempt("a: no features2 pNext (pEnabledFeatures=NULL), all exts", ra); fail_a = ra != VK_SUCCESS;
    /* (b) core features only, empty chain */
    rb = hw_try(1, 0, core, g_prio, 1); hw_attempt("b: features2 core only (queried), empty pNext chain", rb); fail_b = rb != VK_SUCCESS;
    /* (c) each chained struct alone (queried core + that struct) */
    unsigned order[2] = { CH_BLEND, CH_YCBCR };
    for (int k = 0; k < 2; k++) {
        if (!(q_avail & order[k])) continue;
        VkResult r = hw_try(1, order[k], core, g_prio, 1);
        snprintf(desc, sizeof desc, "c: features2 core + only %s", order[k] == CH_BLEND ? "BlendOperationAdvancedFeaturesEXT" : "SamplerYcbcrConversionFeatures");
        hw_attempt(desc, r); if (r != VK_SUCCESS) bad_chain |= order[k];
    }
    if (full != VK_SUCCESS && !fail_b && !bad_chain && !fail_a) fail_combo = 1;
    /* (d) individual core features */
    int found[NCORE], nfound = 0;
    if (fail_b) {
        unsigned char keep[NCORE]; memset(keep, 0, sizeof keep);
        VkResult r0 = core_try(keep, 0, g_prio);
        hw_attempt("d: features2 with ALL core features FALSE", r0);
        if (r0 != VK_SUCCESS) fail_empty = 1;
        else {
            /* candidate = TRUE features; halve while one half alone still fails, then 1-minimal removal */
            int cand[NCORE], nc = 0;
            for (int i = 0; i < NCORE; i++) if (((VkBool32 *)&q_core)[i]) cand[nc++] = i;
            for (;;) {
                if (nc <= 1) break;
                int h = nc / 2, took = 0;
                for (int side = 0; side < 2 && !took; side++) {
                    int *s = side ? cand + h : cand, n = side ? nc - h : h;
                    memset(keep, 0, sizeof keep); for (int i = 0; i < n; i++) keep[s[i]] = 1;
                    VkResult r = core_try(keep, 0, g_prio);
                    snprintf(desc, sizeof desc, "d: core features %s half only (%d enabled)", side ? "second" : "first", n);
                    hw_attempt(desc, r);
                    if (r != VK_SUCCESS) { memmove(cand, s, n * sizeof *cand); nc = n; took = 1; }
                }
                if (!took) break;
            }
            for (int i = 0; i < nc; ) { /* 1-minimal: drop each member if still failing without it */
                memset(keep, 0, sizeof keep); for (int j = 0; j < nc; j++) if (j != i) keep[cand[j]] = 1;
                if (nc > 1 && core_try(keep, 0, g_prio) != VK_SUCCESS) { memmove(cand + i, cand + i + 1, (nc - i - 1) * sizeof *cand); nc--; }
                else i++;
            }
            memset(keep, 0, sizeof keep); for (int j = 0; j < nc; j++) keep[cand[j]] = 1;
            VkResult r = core_try(keep, 0, g_prio);
            snprintf(desc, sizeof desc, "d: minimal core set (%d)", nc); hw_attempt(desc, r);
            if (r != VK_SUCCESS) { for (int j = 0; j < nc; j++) found[nfound++] = cand[j]; }
            else fail_combo = 1; /* the search lost the failure: non-monotonic */
        }
    }
    /* (e) global priority, on the full HWUI config and on config (a) */
    char pfail[512] = ""; int npf = 0;
    for (unsigned i = 0; i < sizeof prios / sizeof *prios; i++) {
        VkResult r1 = hw_try(1, q_avail, core, prios[i].v, 1), r2 = hw_try(0, 0, NULL, prios[i].v, 1);
        snprintf(desc, sizeof desc, "e: full HWUI config + global priority %s", prios[i].nm); hw_attempt(desc, r1);
        snprintf(desc, sizeof desc, "e: no-features2 config + global priority %s", prios[i].nm); hw_attempt(desc, r2);
        if (r1 != VK_SUCCESS || r2 != VK_SUCCESS) {
            int n = (int)strlen(pfail);
            snprintf(pfail + n, sizeof pfail - n, "%s%s=%d %s", npf++ ? ", " : "", prios[i].nm, (int)(r1 != VK_SUCCESS ? r1 : r2), rname(r1 != VK_SUCCESS ? r1 : r2));
        }
    }
    /* summary */
    char cause[1024]; int n = 0;
    cause[0] = 0;
    if (full == VK_SUCCESS) n += snprintf(cause + n, sizeof cause - n, "full HWUI create succeeded (priority %s)", g_prio_name);
    else if (fail_a) n += snprintf(cause + n, sizeof cause - n, "fails even without features2 pNext (%d %s): extensions/queue/instance, not features", (int)ra, rname(ra));
    else if (fail_empty) n += snprintf(cause + n, sizeof cause - n, "features2 pNext with all core features FALSE already fails (%d %s): the features2 pNext struct itself", (int)rb, rname(rb));
    else if (nfound) {
        n += snprintf(cause + n, sizeof cause - n, "core feature(s) enabled via features2 (%d %s):", (int)rb, rname(rb));
        for (int j = 0; j < nfound; j++) n += snprintf(cause + n, sizeof cause - n, " %s", core_names[found[j]]);
    } else if (bad_chain) {
        n += snprintf(cause + n, sizeof cause - n, "chained struct(s):%s%s", (bad_chain & CH_BLEND) ? " BlendOperationAdvancedFeaturesEXT" : "", (bad_chain & CH_YCBCR) ? " SamplerYcbcrConversionFeatures" : "");
    } else if (fail_combo) n += snprintf(cause + n, sizeof cause - n, "only the full combination fails (each part alone passes): chain combination");
    else n += snprintf(cause + n, sizeof cause - n, "full config failed (%d %s) but no single ingredient reproduces it", (int)full, rname(full));
    if (npf) n += snprintf(cause + n, sizeof cause - n, "; global priority failures: %s", pfail);
    else n += snprintf(cause + n, sizeof cause - n, "; global priority low/medium/high/realtime all create ok");
    printf("cause: %s\n", cause);
    return full == VK_SUCCESS ? 0 : 1;
}

int main(int argc, char **argv) {
    uint32_t want_vendor = 0x10de;
    const char *icd = NULL, *sphal = NULL, *hal = NULL, *with = NULL;
    int list = 0, all = 0, each = 0, bis = 0, hw = 0, hwb = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--icd") && i + 1 < argc) icd = argv[++i];
        else if (!strcmp(argv[i], "--sphal") && i + 1 < argc) sphal = argv[++i];
        else if (!strcmp(argv[i], "--hal") && i + 1 < argc) hal = argv[++i];
        else if (!strcmp(argv[i], "--with") && i + 1 < argc) with = argv[++i];
        else if (!strcmp(argv[i], "--loader")) icd = NULL;
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--all")) all = 1;
        else if (!strcmp(argv[i], "--each")) each = 1;
        else if (!strcmp(argv[i], "--bisect")) bis = 1;
        else if (!strcmp(argv[i], "--hwui")) hw = 1;
        else if (!strcmp(argv[i], "--hwui-bisect")) hw = hwb = 1;
        else if (!strcmp(argv[i], "--global-priority") && i + 1 < argc) {
            const char *v = argv[++i]; unsigned k;
            for (k = 0; k < sizeof prios / sizeof *prios; k++) if (!strcmp(v, prios[k].nm)) { g_prio = prios[k].v; g_prio_name = prios[k].nm; break; }
            if (k == sizeof prios / sizeof *prios) { fprintf(stderr, "--global-priority: want low|medium|high|realtime, got '%s'\n", v); return 2; }
        }
        else if (argv[i][0] != '-') want_vendor = (uint32_t)strtoul(argv[i], NULL, 0);
        else { fprintf(stderr, "usage: vkexts [--loader|--icd P|--sphal N|--hal X] [--list] [--all] [--each] [--bisect] [--with A,B] [--hwui] [--hwui-bisect] [--global-priority low|medium|high|realtime]\n"); return 2; }
    }
    if (!list && !all && !each && !bis && !with && !hw) each = 1;

    if (hal) bootstrap_hal(hal);
    else if (sphal) {
#ifdef __ANDROID__
        bootstrap_sphal(sphal);
#else
        fprintf(stderr, "--sphal is only meaningful inside Android (bionic)\n"); return 2;
#endif
    } else if (icd) bootstrap_icd(icd); else bootstrap_loader();

    p_CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    p_EnumInstExt = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!p_CreateInstance) { fprintf(stderr, "no vkCreateInstance\n"); return 2; }
    const char *iext[512]; uint32_t nie = 0;
    if (p_EnumInstExt) {
        uint32_t c = 0; p_EnumInstExt(NULL, &c, NULL);
        VkExtensionProperties *ep = calloc(c + 1, sizeof *ep);
        if (c && p_EnumInstExt(NULL, &c, ep) >= 0)
            for (uint32_t i = 0; i < c && nie < 512; i++) {
                if (hw) { /* HWUI enables every advertised instance extension (VulkanManager.cpp:141-161) */
                    char *cp = malloc(VK_MAX_EXTENSION_NAME_SIZE); memcpy(cp, ep[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE); iext[nie++] = cp;
                } else if (!strcmp(ep[i].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) iext[nie++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
            }
        free(ep);
    }
    if (hw) { printf("instance extensions enabled (all advertised): %u:", nie); for (uint32_t i = 0; i < nie; i++) printf(" %s", iext[i]); printf("\n"); }
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkexts", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = nie, .ppEnabledExtensionNames = iext };
    VkInstance inst; VkResult r = p_CreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance -> %d\n", r); return 2; }
#define LOAD(var, nm) do { var = (void *)gipa(inst, nm); if (!var) { fprintf(stderr, "gipa: %s missing\n", nm); return 2; } } while (0)
    LOAD(p_DestroyInstance, "vkDestroyInstance"); LOAD(p_EnumPD, "vkEnumeratePhysicalDevices");
    LOAD(p_GetPDProps, "vkGetPhysicalDeviceProperties"); LOAD(p_GetQF, "vkGetPhysicalDeviceQueueFamilyProperties");
    LOAD(p_EnumDevExt, "vkEnumerateDeviceExtensionProperties"); LOAD(p_CreateDevice, "vkCreateDevice"); LOAD(p_DestroyDevice, "vkDestroyDevice");
    p_GetFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");

    uint32_t n = 0; r = p_EnumPD(inst, &n, NULL);
    if (r != VK_SUCCESS || !n) { fprintf(stderr, "no physical devices (%d)\n", r); return 2; }
    VkPhysicalDevice *pds = calloc(n, sizeof *pds); p_EnumPD(inst, &n, pds);
    VkPhysicalDeviceProperties props;
    for (uint32_t i = 0; i < n && !pd; i++) { p_GetPDProps(pds[i], &props); if (props.vendorID == want_vendor) pd = pds[i]; }
    if (!pd) { fprintf(stderr, "no device with vendor 0x%04x\n", want_vendor); return 2; }
    p_GetPDProps(pd, &props);
    printf("device: %s vendor=0x%04x driverVersion=%u.%u.%u (0x%x) apiVersion=%u.%u.%u\n", props.deviceName, props.vendorID,
        VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion), props.driverVersion,
        VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t qn = 0; p_GetQF(pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn + 1, sizeof *qf); p_GetQF(pd, &qn, qf);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn && qfam == UINT32_MAX; i++) if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) qfam = i;
    for (uint32_t i = 0; i < qn && qfam == UINT32_MAX; i++) if (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) qfam = i;
    if (qfam == UINT32_MAX) { fprintf(stderr, "no graphics/compute queue family\n"); return 2; }
    printf("queue family: %u\n", qfam);

    r = p_EnumDevExt(pd, NULL, &nexts, NULL);
    if (r != VK_SUCCESS) { fprintf(stderr, "vkEnumerateDeviceExtensionProperties -> %d\n", r); return 2; }
    VkExtensionProperties *dp = calloc(nexts + 1, sizeof *dp);
    r = p_EnumDevExt(pd, NULL, &nexts, dp);
    if (r < 0) { fprintf(stderr, "vkEnumerateDeviceExtensionProperties -> %d\n", r); return 2; }
    names = calloc(nexts + 1, sizeof *names); specs = calloc(nexts + 1, sizeof *specs);
    for (uint32_t i = 0; i < nexts; i++) { memcpy(names[i], dp[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE); specs[i] = dp[i].specVersion; }
    printf("device extensions: %u\n", nexts);
    if (list) for (uint32_t i = 0; i < nexts; i++) printf("ext %s spec=%u\n", names[i], dp[i].specVersion);

    int rc = 0;
    if (hw) {
        rc = hwui(hwb);
        fflush(stdout);
        if (rc == 2) fprintf(stderr, "hwui: cannot proceed, exit 2 (reason line above)\n");
        p_DestroyInstance(inst, NULL);
        return rc;
    }
    r = try_create(NULL, 0);
    printf("baseline (0 extensions): %d %s\n", (int)r, rname(r));
    if (r != VK_SUCCESS) { printf("baseline failed, extension results below are meaningless\n"); rc = 1; }

    if (all || bis) {
        const char **l = malloc((nexts + 1) * sizeof *l);
        for (uint32_t i = 0; i < nexts; i++) l[i] = names[i];
        r = try_create(l, nexts); free(l);
        printf("all (%u extensions): %d %s\n", nexts, (int)r, rname(r));
        if (r != VK_SUCCESS) { rc = 1; fprintf(stderr, "hint: run with NVGLIBC_TRACE=1 to see the shims' dlopen trace\n"); }
        else if (all && !bis) rc = 0;
    }
    if (with) {
        char *dup = strdup(with), *sp = NULL; const char *l[512]; uint32_t k = 0;
        for (char *t = strtok_r(dup, ",", &sp); t && k < 512; t = strtok_r(NULL, ",", &sp)) l[k++] = t;
        r = try_create(l, k);
        printf("with (%u extensions: %s): %d\n", k, with, r);
        if (r != VK_SUCCESS) rc = 1;
    }
    if (each) {
        uint32_t nf = 0;
        for (uint32_t i = 0; i < nexts; i++) {
            const char *l[1] = { names[i] };
            r = try_create(l, 1);
            if (r == VK_SUCCESS) printf("ok %s\n", names[i]); else { printf("FAIL %s %d\n", names[i], r); nf++; }
        }
        printf("each: %u of %u failed\n", nf, nexts);
        if (nf) { rc = 1; fprintf(stderr, "hint: run with NVGLIBC_TRACE=1 to see the shims' dlopen trace\n"); }
    }
    if (bis) { if (bisect()) { rc = 1; fprintf(stderr, "hint: run with NVGLIBC_TRACE=1 to see the shims' dlopen trace\n"); } }
    fflush(stdout);
    p_DestroyInstance(inst, NULL);
    return rc;
}
