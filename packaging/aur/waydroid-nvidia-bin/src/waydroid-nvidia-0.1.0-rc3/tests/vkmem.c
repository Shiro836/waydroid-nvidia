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
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        printf("type %u: heap %u flags 0x%x%s\n", i, mp.memoryTypes[i].heapIndex,
               mp.memoryTypes[i].propertyFlags,
               (mp.memoryTypes[i].propertyFlags & 1) ? " DEVICE_LOCAL" : "");
    return 0;
}
