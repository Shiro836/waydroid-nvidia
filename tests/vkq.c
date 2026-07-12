// Query ImageFormatProperties2 with AHB handle type at OPTIMAL and LINEAR
// tilings - verifies the patched venus driver accepts LINEAR.
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <stdio.h>
int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t n = 1; VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) return 1;
    for (int t = 0; t < 2; t++) {
        VkPhysicalDeviceExternalImageFormatInfo ext = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
        VkPhysicalDeviceImageFormatInfo2 info = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext,
            VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
            t ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0};
        VkExternalImageFormatProperties extp = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkAndroidHardwareBufferUsageANDROID ahbu = {
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID, &extp};
        VkImageFormatProperties2 props = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ahbu};
        VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &info, &props);
        printf("%s: r=%d ahbUsage=0x%llx\n", t ? "LINEAR" : "OPTIMAL", r,
               (unsigned long long)ahbu.androidHardwareBufferUsage);
    }
    return 0;
}
