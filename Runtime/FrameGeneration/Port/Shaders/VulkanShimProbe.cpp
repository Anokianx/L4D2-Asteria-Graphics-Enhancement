#include "AsteriaVulkanShim.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

static_assert(sizeof(VkInstance) == 4);
static_assert(sizeof(VkDevice) == 4);
static_assert(sizeof(VkBuffer) == 8);

int main()
{
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Asteria Vulkan x86 Shim Probe";
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
        return 1;
    uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS || !device_count)
        return 2;
    std::vector<VkPhysicalDevice> devices(device_count);
    if (vkEnumeratePhysicalDevices(instance, &device_count, devices.data()) != VK_SUCCESS)
        return 3;
    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features11.pNext = &features12;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features11;
    vkGetPhysicalDeviceFeatures2(devices[0], &features);
    VkPhysicalDeviceProperties2 device_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    vkGetPhysicalDeviceProperties2(devices[0], &device_properties);
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[0], &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(devices[0], &queue_count, queues.data());
    uint32_t queue_index = 0;
    while (queue_index < queue_count && !(queues[queue_index].queueFlags & VK_QUEUE_COMPUTE_BIT))
        ++queue_index;
    if (queue_index == queue_count)
        return 4;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_index;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pNext = &features11;
    device_info.pEnabledFeatures = &features.features;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(devices[0], &device_info, nullptr, &device) != VK_SUCCESS)
        return 5;
    AsteriaSetVulkanDispatch(instance, device);
    if (!AsteriaResolveVulkanFunction("vkCreateBuffer")
        || AsteriaResolveVulkanFunction("vkAsteriaUnsupportedMissingFunction"))
        return 6;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = 4096;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
        return 7;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(devices[0], &properties);
    uint32_t memory_index = 0;
    while (memory_index < properties.memoryTypeCount
           && (!(requirements.memoryTypeBits & (1u << memory_index))
               || !(properties.memoryTypes[memory_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
        ++memory_index;
    if (memory_index == properties.memoryTypeCount)
        return 8;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_index;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS)
        return 9;
    if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS)
        return 10;
    void* mapped = nullptr;
    if (vkMapMemory(device, memory, 0, 4096, 0, &mapped) != VK_SUCCESS)
        return 11;
    std::memset(mapped, 0x2a, 4096);
    vkUnmapMemory(device, memory);
    uint64_t buffer_handle = 0;
    std::memcpy(&buffer_handle, &buffer, sizeof(buffer));
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    if (vkDeviceWaitIdle(device) != VK_SUCCESS)
        return 12;
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    AsteriaSetVulkanDispatch(VK_NULL_HANDLE, VK_NULL_HANDLE);
    std::cout << "{\"architecture\":\"x86\",\"real_vulkan_device\":true,"
                 "\"non_dispatchable_handle_bytes\":8,\"buffer_handle\":" << buffer_handle
              << ",\"host_memory_write\":true,\"dispatch_reset\":true,\"features2_query\":true,"
                 "\"vulkan12_features_enabled\":true,\"shader_float16\":"
              << (features12.shaderFloat16 ? "true" : "false") << ",\"storage_buffer16_bit_access\":"
              << (features11.storageBuffer16BitAccess ? "true" : "false")
              << ",\"device_api_version\":" << device_properties.properties.apiVersion << "}" << '\n';
    return 0;
}
