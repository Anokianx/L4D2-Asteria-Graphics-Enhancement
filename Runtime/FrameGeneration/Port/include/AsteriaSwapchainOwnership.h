#pragma once
#include <vulkan/vulkan.h>

inline VkResult AsteriaCreateOwnedSwapchain(PFN_vkCreateSwapchainKHR create, VkDevice device,
    const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator, VkSwapchainKHR &owned)
{
    VkSwapchainKHR output = VK_NULL_HANDLE;
    const VkResult result = create(device, info, allocator, &output);
    if (result == VK_SUCCESS) owned = output;
    return result;
}
