#include <vulkan/vulkan.h>
#include <stdio.h>
int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t n = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) return 1;
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    printf("%s RGBA8: linear=0x%x optimal=0x%x\n", p.deviceName,
           fp.linearTilingFeatures, fp.optimalTilingFeatures);
    return 0;
}
