// crossimport — hybrid-GPU cross-import presentation probe (GitHub issue #2).
//
// QUESTION IT ANSWERS
//   On a hybrid laptop/desktop (one NVIDIA dGPU + one Intel/AMD iGPU), which
//   kind of buffer allocated *for/on the NVIDIA GPU* can the OTHER GPU's driver
//   import over dma_buf, bind as a plain GL_TEXTURE_2D, sample, and show the
//   correct pixels? That is exactly what a compositor (KWin, Weston, Mutter)
//   must do to present a Waydroid window whose frames come off the NVIDIA side
//   while the display is driven by the iGPU. The answer decides the
//   presentation-buffer strategy for hybrid support.
//
//   Two source buffer types are tested, both 256x256 ARGB8888 (BGRA byte
//   order), both DRM_FORMAT_MOD_LINEAR:
//     nvidia-linear  — a real NVIDIA VkImage (VK_EXT_image_drm_format_modifier
//                      explicit LINEAR, exportable dma_buf; falls back to
//                      VK_IMAGE_TILING_LINEAR if the modifier path is refused).
//     udmabuf-linear — a plain CPU buffer (memfd + /dev/udmabuf), the "SW"
//                      buffer shape hwcomposer/gralloc also produces.
//   Each is filled with a deterministic per-pixel pattern and imported on
//   every non-NVIDIA render node via EGL on the GBM platform, twice: once with
//   an explicit LINEAR modifier (EGL_EXT_image_dma_buf_import_modifiers) and
//   once with the legacy pitch/offset-only path (no modifier attributes) —
//   compositor fallback paths differ, so both are recorded separately.
//
// BUILD
//   gcc -O1 -o crossimport tests/crossimport.c -lEGL -lGLESv2 -lgbm -ldl
//   (Vulkan is dlopen'd at runtime — no -lvulkan needed. No -ldrm needed:
//    the few DRM_FORMAT/DRM_FORMAT_MOD constants are defined locally so the
//    build does not depend on libdrm's include dir being on the search path.)
//
// RUN
//   ./crossimport            (selects GPUs by driver, never by node path)
//   Exit 0 = at least one buffer type imports+samples correctly on EVERY
//            non-NVIDIA target. Exit 3 = it does not (reason printed).
//            Exit 2 = broken environment (no Vulkan / no hybrid topology).
//
// INTERPRETING THE FOUR OUTCOMES (per non-NVIDIA target)
//   both work        -> Simplest strategy: hand the iGPU either buffer. Prefer
//                       nvidia-linear (zero-copy from the dGPU allocation).
//   only udmabuf      -> The iGPU cannot import NVIDIA's own dma_buf export;
//                       route presentation through a CPU/udmabuf staging buffer
//                       (NVIDIA writes it, iGPU samples it). Copy cost, but it
//                       works everywhere.
//   only nvidia-linear-> Unusual; the udmabuf path is broken on this host
//                       (check /dev/udmabuf udev access). Use the NVIDIA export
//                       directly.
//   neither           -> No cross-import presentation path over LINEAR; hybrid
//                       support on this pair needs a different mechanism
//                       (GPU-side blit/copy on the NVIDIA context, or a shared
//                       compositor allocator).
//
// stdout carries the PASS/FAIL table + SUMMARY; stderr carries debug detail.

#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/udmabuf.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// --- DRM / EGL constants defined locally (no libdrm include-dir dependency;
//     the same self-contained approach vtest_gpu_alloc.c uses) ---
#define DRM_FORMAT_ARGB8888 0x34325241u /* 'AR24' little-endian B,G,R,A bytes */
#define DRM_FORMAT_MOD_LINEAR 0ULL

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif

#define W 256
#define H 256
#define VK_FMT VK_FORMAT_B8G8R8A8_UNORM
#define NVIDIA_VENDOR_ID 0x10DEu

#define MAX_NODES 16

// ============================ shared helpers ================================

static uint32_t px_expect(int x, int y) {
    // Deterministic per-pixel pattern. Logical channels: R=x, G=y, B=x^y, A=ff.
    // In memory (ARGB8888 / VK_FORMAT_B8G8R8A8_UNORM) bytes are B,G,R,A.
    uint32_t r = (uint32_t)x & 0xff, g = (uint32_t)y & 0xff,
             b = (uint32_t)(x ^ y) & 0xff;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void fill_pattern(void *base, uint32_t offset, uint32_t pitch) {
    for (int y = 0; y < H; y++) {
        uint32_t *row = (uint32_t *)((char *)base + offset + (size_t)y * pitch);
        for (int x = 0; x < W; x++)
            row[x] = px_expect(x, y);
    }
}

static const char *vkresult_str(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "OK";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
    case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
        return "INVALID_DRM_MODIFIER_LAYOUT";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "VkResult(%d)", r);
        return buf;
    }}
}

// ============================ node enumeration ==============================

struct node {
    char path[300];  // /dev/dri/renderDXXX
    char driver[64]; // nvidia / i915 / xe / amdgpu / other
};

// bounded copy that keeps -Wformat-truncation quiet (values are short)
static void set_str(char *out, size_t outsz, const char *src) {
    size_t n = strnlen(src, outsz - 1);
    memcpy(out, src, n);
    out[n] = '\0';
}

// classify a renderDNNN by /sys/class/drm/<name>/device/uevent DRIVER=
static void node_driver(const char *name, char *out, size_t outsz) {
    set_str(out, outsz, "other");
    char sys[400];
    snprintf(sys, sizeof(sys), "/sys/class/drm/%s/device/uevent", name);
    FILE *f = fopen(sys, "r");
    if (!f) { set_str(out, outsz, "unknown"); return; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "DRIVER=", 7) == 0) {
            char *v = line + 7;
            v[strcspn(v, "\r\n")] = '\0';
            set_str(out, outsz, v);
            break;
        }
    }
    fclose(f);
}

static int enum_nodes(struct node *nodes, int max) {
    DIR *d = opendir("/dev/dri");
    if (!d) { perror("opendir /dev/dri"); return 0; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (strncmp(e->d_name, "renderD", 7) != 0) continue;
        char p[300];
        snprintf(p, sizeof(p), "/dev/dri/%s", e->d_name);
        set_str(nodes[n].path, sizeof(nodes[n].path), p);
        node_driver(e->d_name, nodes[n].driver, sizeof(nodes[n].driver));
        n++;
    }
    closedir(d);
    // stable order by path so the table reads the same across runs
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(nodes[j].path, nodes[i].path) < 0) {
                struct node t = nodes[i]; nodes[i] = nodes[j]; nodes[j] = t;
            }
    return n;
}

// ============================ source buffers ================================

struct testbuf {
    const char *name;          // "nvidia-linear" / "udmabuf-linear"
    int fd;                    // dma_buf, -1 if unavailable
    uint32_t pitch;
    uint32_t offset;
    uint64_t size;
    uint32_t fourcc;
    const char *skip_reason;   // non-NULL => not tested
    char detail[160];          // how it was allocated/filled, for the header
};

// --- NVIDIA VkImage, explicit LINEAR modifier, exportable dma_buf ------------
// Kept in file scope so the VkDeviceMemory outlives the probe (the vkMapMemory
// fill fallback needs it mapped; the dma_buf keeps the allocation alive anyway).
static void *g_vk_lib;
static VkInstance g_inst;
static VkDevice g_dev;
static VkDeviceMemory g_nv_mem = VK_NULL_HANDLE;
static VkImage g_nv_img = VK_NULL_HANDLE;

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;

static int build_nvidia_linear(struct testbuf *out) {
    out->name = "nvidia-linear";
    out->fd = -1;
    out->fourcc = DRM_FORMAT_ARGB8888;

    g_vk_lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!g_vk_lib) g_vk_lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!g_vk_lib) { out->skip_reason = "no libvulkan (install vulkan loader)"; return -1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_vk_lib, "vkGetInstanceProcAddr");
    if (!gipa) { out->skip_reason = "vkGetInstanceProcAddr missing"; return -1; }

#define IP(name) PFN_##name name = (PFN_##name)gipa(g_inst, #name)
    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };
    VkResult r = vkCreateInstance(&ici, NULL, &g_inst);
    if (r) { snprintf(out->detail, sizeof(out->detail), "vkCreateInstance %s", vkresult_str(r));
             out->skip_reason = out->detail; return -1; }

    IP(vkEnumeratePhysicalDevices);
    IP(vkGetPhysicalDeviceProperties);
    IP(vkGetPhysicalDeviceMemoryProperties);
    IP(vkEnumerateDeviceExtensionProperties);
    IP(vkCreateDevice);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(g_inst, "vkGetDeviceProcAddr");
#undef IP

    VkPhysicalDevice pds[8]; uint32_t npd = 8;
    vkEnumeratePhysicalDevices(g_inst, &npd, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props;
    for (uint32_t i = 0; i < npd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (props.vendorID == NVIDIA_VENDOR_ID) { pd = pds[i]; break; }
    }
    if (!pd) { out->skip_reason = "no NVIDIA Vulkan device (vendorID 0x10DE)"; return -1; }
    fprintf(stderr, "[nvidia-linear] device: %s\n", props.deviceName);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    // require the dma_buf export extensions; modifier ext is optional (fallback)
    uint32_t next = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &next, NULL);
    VkExtensionProperties *eps = calloc(next ? next : 1, sizeof(*eps));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &next, eps);
    int has_fd = 0, has_dmabuf = 0, has_mod = 0;
    for (uint32_t j = 0; j < next; j++) {
        if (!strcmp(eps[j].extensionName, "VK_KHR_external_memory_fd")) has_fd = 1;
        if (!strcmp(eps[j].extensionName, "VK_EXT_external_memory_dma_buf")) has_dmabuf = 1;
        if (!strcmp(eps[j].extensionName, "VK_EXT_image_drm_format_modifier")) has_mod = 1;
    }
    free(eps);
    if (!has_dmabuf || !has_fd) {
        out->skip_reason = "driver lacks VK_EXT_external_memory_dma_buf "
                           "(closed KM or nvidia-drm.modeset=0)";
        return -1;
    }

    const char *dev_exts[3]; uint32_t ndev = 0;
    dev_exts[ndev++] = "VK_KHR_external_memory_fd";
    dev_exts[ndev++] = "VK_EXT_external_memory_dma_buf";
    if (has_mod) dev_exts[ndev++] = "VK_EXT_image_drm_format_modifier";

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = 0, .queueCount = 1,
                                    .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = ndev,
                               .ppEnabledExtensionNames = dev_exts };
    r = vkCreateDevice(pd, &dci, NULL, &g_dev);
    if (r) { snprintf(out->detail, sizeof(out->detail), "vkCreateDevice %s", vkresult_str(r));
             out->skip_reason = out->detail; return -1; }

#define DP(name) PFN_##name name = (PFN_##name)gdpa(g_dev, #name)
    DP(vkCreateImage); DP(vkGetImageMemoryRequirements);
    DP(vkAllocateMemory); DP(vkBindImageMemory);
    DP(vkGetImageSubresourceLayout); DP(vkGetMemoryFdKHR);
    DP(vkMapMemory); DP(vkFlushMappedMemoryRanges);
#undef DP

    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Attempt 1: explicit DRM_FORMAT_MOD_LINEAR via VK_EXT_image_drm_format_modifier
    int via_tiling = 0;
    VkImage img = VK_NULL_HANDLE;
    if (has_mod) {
        uint64_t mod = DRM_FORMAT_MOD_LINEAR;
        VkExternalMemoryImageCreateInfo emi = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkImageDrmFormatModifierListCreateInfoEXT mlci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
            .pNext = &emi, .drmFormatModifierCount = 1, .pDrmFormatModifiers = &mod };
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &mlci, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FMT,
            .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        r = vkCreateImage(g_dev, &ci, NULL, &img);
        if (r) {
            fprintf(stderr, "[nvidia-linear] modifier-LINEAR CreateImage failed: %s "
                            "-> falling back to VK_IMAGE_TILING_LINEAR\n", vkresult_str(r));
            img = VK_NULL_HANDLE;
        }
    }
    // Attempt 2 (fallback): VK_IMAGE_TILING_LINEAR
    if (img == VK_NULL_HANDLE) {
        via_tiling = 1;
        VkExternalMemoryImageCreateInfo emi = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FMT,
            .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
            .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        r = vkCreateImage(g_dev, &ci, NULL, &img);
        if (r) { snprintf(out->detail, sizeof(out->detail),
                          "CreateImage (both LINEAR paths) %s", vkresult_str(r));
                 out->skip_reason = out->detail; return -1; }
    }
    g_nv_img = img;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, img, &req);
    // prefer a HOST_VISIBLE type so vkMapMemory can serve as a fill fallback;
    // otherwise first eligible type (dma_buf mmap still works on NVIDIA 610).
    uint32_t mtype = UINT32_MAX; int host_visible = 0, host_coherent = 0;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        if (!((req.memoryTypeBits >> t) & 1)) continue;
        VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
        if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            mtype = t; host_visible = 1;
            host_coherent = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 1 : 0;
            break;
        }
    }
    if (mtype == UINT32_MAX)
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
            if ((req.memoryTypeBits >> t) & 1) { mtype = t; break; }
    if (mtype == UINT32_MAX) { out->skip_reason = "no memory type for image"; return -1; }

    VkExportMemoryAllocateInfo exp = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &exp, .image = img };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded, .allocationSize = req.size, .memoryTypeIndex = mtype };
    r = vkAllocateMemory(g_dev, &mai, NULL, &g_nv_mem);
    if (r) { snprintf(out->detail, sizeof(out->detail), "vkAllocateMemory %s", vkresult_str(r));
             out->skip_reason = out->detail; return -1; }
    r = vkBindImageMemory(g_dev, img, g_nv_mem, 0);
    if (r) { snprintf(out->detail, sizeof(out->detail), "vkBindImageMemory %s", vkresult_str(r));
             out->skip_reason = out->detail; return -1; }

    // row pitch: memory-plane-0 aspect for the modifier path, color aspect for
    // tiling-linear.
    VkImageSubresource subres = {
        .aspectMask = via_tiling ? VK_IMAGE_ASPECT_COLOR_BIT
                                 : VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(g_dev, img, &subres, &layout);

    VkMemoryGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = g_nv_mem, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    int fd = -1;
    r = vkGetMemoryFdKHR(g_dev, &gfi, &fd);
    if (r || fd < 0) {
        snprintf(out->detail, sizeof(out->detail),
                 "vkGetMemoryFdKHR %s (open kernel module required)", vkresult_str(r));
        out->skip_reason = out->detail; return -1;
    }

    out->fd = fd;
    out->pitch = (uint32_t)layout.rowPitch;
    out->offset = (uint32_t)layout.offset;
    out->size = req.size;

    // fill: try mmap of the exported dma_buf first (works on NVIDIA 610),
    // then vkMapMemory if the memory is host-visible.
    const char *fill = NULL;
    void *map = mmap(NULL, out->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map != MAP_FAILED) {
        fill_pattern(map, out->offset, out->pitch);
        munmap(map, out->size);
        fill = "dma_buf-mmap";
    } else {
        int mmap_errno = errno;
        if (host_visible) {
            void *vm = NULL;
            r = vkMapMemory(g_dev, g_nv_mem, 0, VK_WHOLE_SIZE, 0, &vm);
            if (r == VK_SUCCESS && vm) {
                fill_pattern(vm, out->offset, out->pitch);
                if (!host_coherent) {
                    VkMappedMemoryRange mr = {
                        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                        .memory = g_nv_mem, .offset = 0, .size = VK_WHOLE_SIZE };
                    vkFlushMappedMemoryRanges(g_dev, 1, &mr);
                }
                // leave mapped; memory persists for the run
                fill = "vkMapMemory";
            }
        }
        if (!fill) {
            snprintf(out->detail, sizeof(out->detail),
                     "cannot fill: dma_buf mmap failed (%s) and memory not "
                     "host-visible", strerror(mmap_errno));
            out->skip_reason = out->detail;
            return -1;
        }
    }

    snprintf(out->detail, sizeof(out->detail),
             "256x256 ARGB8888 pitch=%u off=%u mod=LINEAR(%s) fill=%s fd=%d",
             out->pitch, out->offset, via_tiling ? "tiling" : "explicit", fill, fd);
    return 0;
}

// --- udmabuf: memfd + F_SEAL_SHRINK + UDMABUF_CREATE, tightly packed ---------
static int build_udmabuf_linear(struct testbuf *out) {
    out->name = "udmabuf-linear";
    out->fd = -1;
    out->fourcc = DRM_FORMAT_ARGB8888;
    out->pitch = W * 4;
    out->offset = 0;

    const uint64_t size = (uint64_t)out->pitch * H; // 256*4*256 = 256 KiB, page-aligned
    out->size = size;

    int memfd = memfd_create("crossimport-udmabuf", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (memfd < 0) { snprintf(out->detail, sizeof(out->detail),
                              "memfd_create: %s", strerror(errno));
                     out->skip_reason = out->detail; return -1; }
    if (ftruncate(memfd, (off_t)size) < 0) {
        snprintf(out->detail, sizeof(out->detail), "ftruncate: %s", strerror(errno));
        out->skip_reason = out->detail; close(memfd); return -1;
    }
    if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
        snprintf(out->detail, sizeof(out->detail), "F_SEAL_SHRINK: %s", strerror(errno));
        out->skip_reason = out->detail; close(memfd); return -1;
    }

    int ufd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ufd < 0) {
        snprintf(out->detail, sizeof(out->detail),
                 "/dev/udmabuf: %s (install 70-waydroid-nvidia.rules / grant access)",
                 strerror(errno));
        out->skip_reason = out->detail; close(memfd); return -1;
    }
    struct udmabuf_create create = { .memfd = (uint32_t)memfd,
        .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = size };
    int dmabuf = ioctl(ufd, UDMABUF_CREATE, &create);
    int cr_errno = errno;
    close(ufd);
    if (dmabuf < 0) {
        snprintf(out->detail, sizeof(out->detail), "UDMABUF_CREATE: %s", strerror(cr_errno));
        out->skip_reason = out->detail; close(memfd); return -1;
    }

    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf, 0);
    if (map == MAP_FAILED) {
        snprintf(out->detail, sizeof(out->detail), "mmap udmabuf: %s", strerror(errno));
        out->skip_reason = out->detail; close(dmabuf); close(memfd); return -1;
    }
    fill_pattern(map, 0, out->pitch);
    munmap(map, size);
    close(memfd); // dma_buf keeps the pages alive

    out->fd = dmabuf;
    snprintf(out->detail, sizeof(out->detail),
             "256x256 ARGB8888 pitch=%u mod=LINEAR fill=udmabuf-mmap fd=%d",
             out->pitch, dmabuf);
    return 0;
}

// ============================ EGL import + sample ===========================

static PFNEGLCREATEIMAGEKHRPROC pCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTex2D;

static const char *vs_src =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";

// Bind an EGLImage to `target`, render 1:1 into an FBO, read back probe pixels.
// Returns: 0 = bound + content OK, 1 = bound but content BAD, -1 = bind/setup fail.
static int sample_and_verify(EGLImageKHR img, GLenum target,
                             const char *sampler_type) {
    while (glGetError() != GL_NO_ERROR);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(target, tex);
    pImageTargetTex2D(target, (GLeglImageOES)img);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "    bind %s: glError 0x%x\n",
                target == GL_TEXTURE_2D ? "TEXTURE_2D" : "TEXTURE_EXTERNAL_OES", err);
        glDeleteTextures(1, &tex);
        return -1;
    }
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLuint dst, fbo;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "    fbo incomplete\n");
        glDeleteTextures(1, &tex); glDeleteTextures(1, &dst);
        glDeleteFramebuffers(1, &fbo);
        return -1;
    }

    char fs_src[256];
    snprintf(fs_src, sizeof(fs_src),
        "#extension GL_OES_EGL_image_external : enable\n"
        "precision mediump float;\n"
        "varying vec2 uv;\n"
        "uniform %s tex;\n"
        "void main() { gl_FragColor = texture2D(tex, uv); }\n", sampler_type);
    const char *fsp = fs_src;
    GLuint vsh = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vsh, 1, &vs_src, NULL); glCompileShader(vsh);
    GLuint fsh = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fsh, 1, &fsp, NULL); glCompileShader(fsh);
    GLint cok;
    glGetShaderiv(fsh, GL_COMPILE_STATUS, &cok);
    if (!cok) {
        char log[512]; glGetShaderInfoLog(fsh, sizeof(log), NULL, log);
        fprintf(stderr, "    fragment shader (%s): %s\n", sampler_type, log);
        glDeleteShader(vsh); glDeleteShader(fsh);
        glDeleteTextures(1, &tex); glDeleteTextures(1, &dst);
        glDeleteFramebuffers(1, &fbo);
        return -1;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vsh); glAttachShader(prog, fsh);
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    GLint lok; glGetProgramiv(prog, GL_LINK_STATUS, &lok);
    if (!lok) {
        char log[512]; glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "    link (%s): %s\n", sampler_type, log);
        glDeleteProgram(prog); glDeleteShader(vsh); glDeleteShader(fsh);
        glDeleteTextures(1, &tex); glDeleteTextures(1, &dst);
        glDeleteFramebuffers(1, &fbo);
        return -1;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(target, tex);
    static const float quad[] = {-1,-1, 3,-1, -1,3};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    // probe pixels: 4 corners + center (texel == FBO pixel, no net Y-flip)
    static const int probes[][2] = { {0,0}, {W-1,0}, {0,H-1}, {W-1,H-1}, {W/2,H/2} };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        int x = probes[i][0], y = probes[i][1];
        unsigned char px[4] = {0};
        glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int er = x & 0xff, eg = y & 0xff, eb = (x ^ y) & 0xff;
        int dr = abs((int)px[0] - er), dg = abs((int)px[1] - eg), db = abs((int)px[2] - eb);
        if (dr > 6 || dg > 6 || db > 6) {
            fprintf(stderr, "    probe (%3d,%3d): got %02x %02x %02x expect %02x %02x %02x\n",
                    x, y, px[0], px[1], px[2], er, eg, eb);
            bad = 1;
        }
    }
    glDeleteProgram(prog); glDeleteShader(vsh); glDeleteShader(fsh);
    glDeleteTextures(1, &tex); glDeleteTextures(1, &dst);
    glDeleteFramebuffers(1, &fbo);
    return bad ? 1 : 0;
}

// result of one (target, buffer, variant) cell
struct cell {
    int import_ok;   // eglCreateImageKHR succeeded
    int tex2d;       // 0 not attempted, 1 OK, -1 bind failed
    int content;     // -1 n/a, 0 OK, 1 BAD (only meaningful when tex2d==1)
    int ext_oes;     // 0 not attempted, 1 OK, -1 failed (cross-check, non-fatal)
    EGLint egl_err;
};

// import one buffer with/without the explicit LINEAR modifier
static struct cell import_cell(EGLDisplay dpy, const struct testbuf *b, int with_mod) {
    struct cell c = { 0, 0, -1, 0, EGL_SUCCESS };
    EGLint attrs[32]; int a = 0;
    attrs[a++] = EGL_WIDTH;  attrs[a++] = W;
    attrs[a++] = EGL_HEIGHT; attrs[a++] = H;
    attrs[a++] = EGL_LINUX_DRM_FOURCC_EXT;     attrs[a++] = (EGLint)b->fourcc;
    attrs[a++] = EGL_DMA_BUF_PLANE0_FD_EXT;    attrs[a++] = b->fd;
    attrs[a++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[a++] = (EGLint)b->offset;
    attrs[a++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attrs[a++] = (EGLint)b->pitch;
    if (with_mod) {
        attrs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attrs[a++] = 0; /* LINEAR */
        attrs[a++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attrs[a++] = 0;
    }
    attrs[a++] = EGL_NONE;

    EGLImageKHR img = pCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (img == EGL_NO_IMAGE_KHR) { c.egl_err = eglGetError(); return c; }
    c.import_ok = 1;

    int t = sample_and_verify(img, GL_TEXTURE_2D, "sampler2D");
    if (t < 0) { c.tex2d = -1; }
    else { c.tex2d = 1; c.content = t; } // 0 OK, 1 BAD

    // cross-check the known NVIDIA external-only signature (non-fatal)
    int e = sample_and_verify(img, GL_TEXTURE_EXTERNAL_OES, "samplerExternalOES");
    c.ext_oes = (e < 0) ? -1 : 1;

    pDestroyImage(dpy, img);
    return c;
}

// ================================ main ======================================

int main(void) {
    printf("== crossimport: NVIDIA -> other-GPU dma_buf presentation probe (issue #2) ==\n");

    struct node nodes[MAX_NODES];
    int nn = enum_nodes(nodes, MAX_NODES);
    if (nn == 0) { printf("FAIL: no /dev/dri/renderD* nodes\n"); return 2; }

    int nv_count = 0, tgt_idx[MAX_NODES], ntgt = 0;
    const char *nv_path = NULL;
    for (int i = 0; i < nn; i++) {
        fprintf(stderr, "node %s driver=%s\n", nodes[i].path, nodes[i].driver);
        if (!strcmp(nodes[i].driver, "nvidia")) { nv_count++; nv_path = nodes[i].path; }
        else tgt_idx[ntgt++] = i;
    }
    if (nv_count == 0) {
        printf("== UNTESTABLE: no NVIDIA render node found — this probe needs a "
               "hybrid box (NVIDIA dGPU + iGPU).\n");
        return 3;
    }
    if (nv_count > 1)
        fprintf(stderr, "warning: %d nvidia nodes; Vulkan selects by vendorID 0x10DE\n", nv_count);
    if (ntgt == 0) {
        printf("== UNTESTABLE: only NVIDIA render node(s) present — no other-GPU "
               "target to import into. Not a hybrid box.\n");
        return 3;
    }
    printf("nvidia node: %s\n", nv_path);
    printf("targets:");
    for (int i = 0; i < ntgt; i++)
        printf(" %s(%s)", nodes[tgt_idx[i]].path, nodes[tgt_idx[i]].driver);
    printf("\n");

    // --- prepare the two source buffers ---
    struct testbuf bufs[2];
    memset(bufs, 0, sizeof(bufs));
    build_nvidia_linear(&bufs[0]);
    build_udmabuf_linear(&bufs[1]);
    const int NBUF = 2;

    printf("\n[source buffers]\n");
    for (int b = 0; b < NBUF; b++) {
        if (bufs[b].skip_reason)
            printf("  %-14s SKIP: %s\n", bufs[b].name, bufs[b].skip_reason);
        else
            printf("  %-14s %s\n", bufs[b].name, bufs[b].detail);
    }
    int any_source = 0;
    for (int b = 0; b < NBUF; b++) if (!bufs[b].skip_reason) any_source = 1;
    if (!any_source) {
        printf("\n== UNTESTABLE: no source buffer could be prepared (see reasons above).\n");
        return 3;
    }

    // works[t][b] = buffer b binds as TEXTURE_2D with correct content on target t
    int works[MAX_NODES][2];
    // richer per-cell bookkeeping for the summary
    int reached[MAX_NODES][2], tex2d_any[MAX_NODES][2],
        content_bad[MAX_NODES][2], ext_any[MAX_NODES][2];
    memset(works, 0, sizeof(works));
    memset(reached, 0, sizeof(reached));
    memset(tex2d_any, 0, sizeof(tex2d_any));
    memset(content_bad, 0, sizeof(content_bad));
    memset(ext_any, 0, sizeof(ext_any));

    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC qmods =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    pCreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    pDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    pImageTargetTex2D =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!getPlatDpy || !pCreateImage || !pImageTargetTex2D) {
        printf("== BROKEN: missing EGL entrypoints (eglGetPlatformDisplayEXT / "
               "eglCreateImageKHR / glEGLImageTargetTexture2DOES)\n");
        return 2;
    }

    printf("\n%-10s %-14s %-9s %6s %-6s %-6s %-11s %-8s\n",
           "TARGET", "BUFFER", "VARIANT", "PITCH", "IMPORT", "TEX2D", "CONTENT", "EXT_OES");

    // --- per target ---
    for (int ti = 0; ti < ntgt; ti++) {
        struct node *tn = &nodes[tgt_idx[ti]];

        int fd = open(tn->path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "open %s: %s\n", tn->path, strerror(errno));
            printf("%-10s (open failed: %s)\n", tn->driver, strerror(errno));
            for (int b = 0; b < NBUF; b++) works[ti][b] = 0;
            continue;
        }
        struct gbm_device *gbm = gbm_create_device(fd);
        if (!gbm) {
            fprintf(stderr, "gbm_create_device(%s) failed\n", tn->path);
            printf("%-10s (gbm_create_device failed)\n", tn->driver);
            close(fd);
            continue;
        }
        EGLDisplay dpy = getPlatDpy(EGL_PLATFORM_GBM_KHR, gbm, NULL);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
            fprintf(stderr, "eglInitialize(%s) failed 0x%x\n", tn->path, eglGetError());
            printf("%-10s (eglInitialize failed)\n", tn->driver);
            gbm_device_destroy(gbm); close(fd);
            continue;
        }

        const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
        int has_import_mod = ext && strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers");

        eglBindAPI(EGL_OPENGL_ES_API);
        static const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, 0,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
        EGLConfig cfg; EGLint ncfg = 0;
        eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg);
        static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLContext ctx = eglCreateContext(dpy, ncfg ? cfg : EGL_NO_CONFIG_KHR,
                                          EGL_NO_CONTEXT, ctx_attr);
        if (ctx == EGL_NO_CONTEXT) {
            fprintf(stderr, "eglCreateContext(%s) failed 0x%x\n", tn->path, eglGetError());
            printf("%-10s (eglCreateContext failed)\n", tn->driver);
            eglTerminate(dpy); gbm_device_destroy(gbm); close(fd);
            continue;
        }
        if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            fprintf(stderr, "eglMakeCurrent(%s) failed 0x%x\n", tn->path, eglGetError());
            printf("%-10s (eglMakeCurrent failed; surfaceless unsupported?)\n", tn->driver);
            eglDestroyContext(dpy, ctx); eglTerminate(dpy);
            gbm_device_destroy(gbm); close(fd);
            continue;
        }

        printf("--- target %s (%s) ---\n", tn->path, tn->driver);
        printf("  EGL: %s / %s\n", eglQueryString(dpy, EGL_VENDOR),
               eglQueryString(dpy, EGL_VERSION));
        printf("  GL:  %s / %s\n", (const char *)glGetString(GL_VENDOR),
               (const char *)glGetString(GL_RENDERER));

        int linear_in_list = -1; // -1 unknown, 0 no, 1 yes
        if (qmods && has_import_mod) {
            EGLuint64KHR ms[128]; EGLBoolean ex[128]; EGLint n = 0;
            if (qmods(dpy, (EGLint)DRM_FORMAT_ARGB8888, 128, ms, ex, &n)) {
                linear_in_list = 0;
                for (int i = 0; i < n; i++)
                    if (ms[i] == DRM_FORMAT_MOD_LINEAR) { linear_in_list = 1; break; }
            }
        }
        printf("  import_modifiers ext: %s   LINEAR in AR24 list: %s\n",
               has_import_mod ? "yes" : "no",
               linear_in_list < 0 ? "unknown" :
               linear_in_list ? "yes" : "no");

        // --- per buffer x variant ---
        for (int b = 0; b < NBUF; b++) {
            if (bufs[b].skip_reason) {
                printf("%-10s %-14s %-9s %6s %-6s %-6s %-11s %-8s\n",
                       tn->driver, bufs[b].name, "-", "-",
                       "SKIP", "-", "-", "-");
                works[ti][b] = 0;
                continue;
            }
            for (int v = 0; v < 2; v++) {
                int with_mod = (v == 0);
                const char *vlabel = with_mod ? "with-mod" : "no-mod";
                if (with_mod && !has_import_mod) {
                    printf("%-10s %-14s %-9s %6u %-6s %-6s %-11s %-8s\n",
                           tn->driver, bufs[b].name, vlabel, bufs[b].pitch,
                           "N/A", "-", "-", "-");
                    continue;
                }
                struct cell c = import_cell(dpy, &bufs[b], with_mod);

                const char *s_import = c.import_ok ? "OK" : "FAIL";
                char import_col[16];
                if (!c.import_ok) snprintf(import_col, sizeof(import_col), "FAIL(0x%x)", c.egl_err);
                else snprintf(import_col, sizeof(import_col), "%s", s_import);

                const char *s_tex2d = c.tex2d == 1 ? "OK" : c.tex2d == -1 ? "FAIL" : "-";
                const char *s_content = c.tex2d != 1 ? "-" :
                    c.content == 0 ? "CONTENT-OK" : "CONTENT-BAD";
                const char *s_ext = c.ext_oes == 1 ? "OK" : c.ext_oes == -1 ? "FAIL" : "-";

                printf("%-10s %-14s %-9s %6u %-6s %-6s %-11s %-8s\n",
                       tn->driver, bufs[b].name, vlabel, bufs[b].pitch,
                       import_col, s_tex2d, s_content, s_ext);

                if (c.import_ok) reached[ti][b] = 1;
                if (c.tex2d == 1) {
                    tex2d_any[ti][b] = 1;
                    if (c.content == 0) works[ti][b] = 1;
                    else content_bad[ti][b] = 1;
                }
                if (c.ext_oes == 1) ext_any[ti][b] = 1;
            }
        }

        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        gbm_device_destroy(gbm);
        close(fd);
    }

    // ---------------------------- summary ----------------------------
    printf("\n=== SUMMARY ===\n");
    for (int ti = 0; ti < ntgt; ti++) {
        const char *tdrv = nodes[tgt_idx[ti]].driver;
        for (int b = 0; b < NBUF; b++) {
            const char *verdict;
            if (bufs[b].skip_reason)              verdict = "SKIPPED (source unavailable)";
            else if (works[ti][b])                verdict = "IMPORT+SAMPLE OK";
            else if (tex2d_any[ti][b] && content_bad[ti][b])
                                                  verdict = "TEXTURE_2D binds but CONTENT-BAD";
            else if (ext_any[ti][b])              verdict = "external-only (no TEXTURE_2D) — not directly displayable";
            else if (reached[ti][b])              verdict = "imported but no usable bind";
            else                                  verdict = "IMPORT FAIL";
            printf("%-14s -> %-7s: %s\n", bufs[b].name, tdrv, verdict);
        }
    }

    // exit 0 iff some buffer type works on EVERY non-nvidia target
    int good_buf = -1;
    for (int b = 0; b < NBUF; b++) {
        if (bufs[b].skip_reason) continue;
        int all = 1;
        for (int ti = 0; ti < ntgt; ti++) if (!works[ti][b]) { all = 0; break; }
        if (all) { good_buf = b; break; }
    }

    if (good_buf >= 0) {
        // report every buffer type that works everywhere
        printf("\nRESULT:");
        for (int b = 0; b < NBUF; b++) {
            if (bufs[b].skip_reason) continue;
            int all = 1;
            for (int ti = 0; ti < ntgt; ti++) if (!works[ti][b]) { all = 0; break; }
            if (all) printf(" %s", bufs[b].name);
        }
        printf(" import+sample correctly on every non-NVIDIA target -> exit 0\n");
        return 0;
    }

    printf("\nRESULT: no source buffer type binds as GL_TEXTURE_2D with correct "
           "content on every non-NVIDIA target -> exit 3\n");
    printf("  (see per-target verdicts above; if only udmabuf works route "
           "presentation through a CPU staging buffer; if nothing works, "
           "LINEAR cross-import is unavailable on this pair)\n");
    return 3;
}
