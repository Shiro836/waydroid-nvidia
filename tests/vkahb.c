// Guest-side: import an AHardwareBuffer into Vulkan (venus) directly,
// printing every VkResult — isolates the venus AHB path from ANGLE.
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/hardware_buffer.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { VkResult _r = (x); \
    printf("  %s = %d\n", #x, _r); if (_r != VK_SUCCESS) goto next; } while (0)

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("no instance\n"); return 1; }
    uint32_t n = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no device\n"); return 1; }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char *exts[] = {
        "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_KHR_sampler_ycbcr_conversion",
        "VK_KHR_external_memory",
        "VK_EXT_queue_family_foreign",
        "VK_KHR_dedicated_allocation",
    };
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 5; dci.ppEnabledExtensionNames = exts;
    VkDevice dev;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("vkCreateDevice = %d\n", r);
    if (r) return 1;

    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getProps =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)
        vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");

    for (int pass = 0; pass < 2; pass++) {
        const char *label = pass == 0 ? "GPU-only" : "CPU";
        AHardwareBuffer_Desc desc = {
            .width = 512, .height = 512, .layers = 1,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
            .usage = pass == 0
                ? AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                : AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
        };
        AHardwareBuffer *ahb = NULL;
        if (AHardwareBuffer_allocate(&desc, &ahb)) {
            printf("[%s] AHB alloc failed\n", label);
            continue;
        }
        printf("[%s] AHB ok\n", label);

        VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
        VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fmt};
        CHECK(getProps(dev, ahb, &ahb_props));
        printf("  size=%llu memTypes=0x%x vkFormat=%d externalFormat=%llu\n",
               (unsigned long long)ahb_props.allocationSize, ahb_props.memoryTypeBits,
               fmt.format, (unsigned long long)fmt.externalFormat);

        {
            VkExternalMemoryImageCreateInfo emi = {
                VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
            VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
            ic.imageType = VK_IMAGE_TYPE_2D;
            ic.format = fmt.format;
            ic.extent = (VkExtent3D){512, 512, 1};
            ic.mipLevels = 1; ic.arrayLayers = 1;
            ic.samples = VK_SAMPLE_COUNT_1_BIT;
            ic.tiling = VK_IMAGE_TILING_OPTIMAL;
            ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImage img;
            CHECK(vkCreateImage(dev, &ic, NULL, &img));

            VkImportAndroidHardwareBufferInfoANDROID imp = {
                VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, ahb};
            VkMemoryDedicatedAllocateInfo ded = {
                VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
            ded.image = img;
            uint32_t mt = 0;
            while (mt < 32 && !(ahb_props.memoryTypeBits & (1u << mt))) mt++;
            VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded,
                                        ahb_props.allocationSize, mt};
            VkDeviceMemory mem;
            CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
            CHECK(vkBindImageMemory(dev, img, mem, 0));
            printf("[%s] FULL IMPORT OK\n", label);
        }
next:;
    }
    printf("DONE\n");
    return 0;
}
