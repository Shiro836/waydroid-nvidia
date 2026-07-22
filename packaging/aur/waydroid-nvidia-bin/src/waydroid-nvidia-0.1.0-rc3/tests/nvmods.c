// Print NVIDIA Vulkan's VkDrmFormatModifierPropertiesListEXT for RGBA8/BGRA8:
// does the driver list DRM_FORMAT_MOD_LINEAR (0x0) at all, and with which
// features? Venus's AHB import creates images with
// VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, so absent modifiers = unimportable.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t n = 8; VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (strstr(p.deviceName, "NVIDIA")) { pd = pds[i]; printf("device: %s\n", p.deviceName); break; }
    }
    if (!pd) return 1;

    const VkFormat fmts[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    const char *names[] = {"R8G8B8A8", "B8G8R8A8"};
    for (int f = 0; f < 2; f++) {
        VkDrmFormatModifierPropertiesEXT props[128];
        VkDrmFormatModifierPropertiesListEXT list = {
            VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
        VkFormatProperties2 fp = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
        vkGetPhysicalDeviceFormatProperties2(pd, fmts[f], &fp);
        list.drmFormatModifierCount = list.drmFormatModifierCount > 128 ? 128 : list.drmFormatModifierCount;
        list.pDrmFormatModifierProperties = props;
        vkGetPhysicalDeviceFormatProperties2(pd, fmts[f], &fp);
        printf("%s: %u modifiers\n", names[f], list.drmFormatModifierCount);
        for (uint32_t i = 0; i < list.drmFormatModifierCount; i++)
            printf("  0x%016llx planes=%u features=0x%x%s\n",
                   (unsigned long long)props[i].drmFormatModifier,
                   props[i].drmFormatModifierPlaneCount,
                   props[i].drmFormatModifierTilingFeatures,
                   props[i].drmFormatModifier == 0 ? "  <-- LINEAR" : "");
    }
    return 0;
}
