#pragma once

#include <windows.h>
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <cstdint>

struct AsteriaNativePresentStats
{
    uint64_t attempts;
    uint64_t successful_calls;
    uint64_t failed_calls;
    uint64_t successful_swapchains;
};

void AsteriaSetVulkanDispatch(VkInstance instance, VkDevice device);
void AsteriaSetVulkanPrototypeFallback(bool enabled);
bool AsteriaRegisterVulkanInstance(VkInstance instance, PFN_vkGetInstanceProcAddr next);
bool AsteriaRegisterVulkanPhysicalDevice(VkInstance instance, VkPhysicalDevice physical);
bool AsteriaRegisterVulkanDevice(VkInstance instance, VkPhysicalDevice physical,
                                VkDevice device, PFN_vkGetDeviceProcAddr next);
bool AsteriaRegisterVulkanQueue(VkDevice device, VkQueue queue);
bool AsteriaRegisterVulkanCommandBuffer(VkDevice device, VkCommandBuffer command,
                                       VkCommandPool pool = VK_NULL_HANDLE);
void AsteriaUnregisterVulkanCommandBuffer(VkCommandBuffer command);
void AsteriaUnregisterVulkanCommandPool(VkDevice device, VkCommandPool pool);
void AsteriaUnregisterVulkanDevice(VkDevice device);
void AsteriaUnregisterVulkanInstance(VkInstance instance);
PFN_vkVoidFunction AsteriaResolveVulkanInstanceFunction(VkInstance instance, const char* name);
PFN_vkVoidFunction AsteriaResolveVulkanDeviceFunction(VkDevice device, const char* name);
PFN_vkVoidFunction AsteriaResolveVulkanFunction(const char* name);
AsteriaNativePresentStats AsteriaGetNativePresentStats();
void AsteriaResetNativePresentStats();
using AsteriaSwapchainImagesReplacementFn = VkResult (VKAPI_PTR *)(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
using AsteriaSwapchainTokenPredicateFn = bool (*)(VkSwapchainKHR);
void AsteriaRegisterSwapchainImagesReplacement(AsteriaSwapchainImagesReplacementFn replacement,
                                               AsteriaSwapchainTokenPredicateFn predicate);
