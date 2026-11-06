#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdexcept>

namespace asteria_depth_capture
{
struct DeviceFunctions
{
#define ASTERIA_DEPTH_FUNCTION(name) PFN_##name name = nullptr;
    ASTERIA_DEPTH_FUNCTION(vkCreateImage)
    ASTERIA_DEPTH_FUNCTION(vkDestroyImage)
    ASTERIA_DEPTH_FUNCTION(vkGetImageMemoryRequirements)
    ASTERIA_DEPTH_FUNCTION(vkAllocateMemory)
    ASTERIA_DEPTH_FUNCTION(vkFreeMemory)
    ASTERIA_DEPTH_FUNCTION(vkBindImageMemory)
    ASTERIA_DEPTH_FUNCTION(vkCreateImageView)
    ASTERIA_DEPTH_FUNCTION(vkDestroyImageView)
    ASTERIA_DEPTH_FUNCTION(vkCreateShaderModule)
    ASTERIA_DEPTH_FUNCTION(vkDestroyShaderModule)
    ASTERIA_DEPTH_FUNCTION(vkCreateDescriptorSetLayout)
    ASTERIA_DEPTH_FUNCTION(vkDestroyDescriptorSetLayout)
    ASTERIA_DEPTH_FUNCTION(vkCreatePipelineLayout)
    ASTERIA_DEPTH_FUNCTION(vkDestroyPipelineLayout)
    ASTERIA_DEPTH_FUNCTION(vkCreateComputePipelines)
    ASTERIA_DEPTH_FUNCTION(vkDestroyPipeline)
    ASTERIA_DEPTH_FUNCTION(vkCreateDescriptorPool)
    ASTERIA_DEPTH_FUNCTION(vkDestroyDescriptorPool)
    ASTERIA_DEPTH_FUNCTION(vkAllocateDescriptorSets)
    ASTERIA_DEPTH_FUNCTION(vkUpdateDescriptorSets)
    ASTERIA_DEPTH_FUNCTION(vkCmdPipelineBarrier)
    ASTERIA_DEPTH_FUNCTION(vkCmdCopyImage)
    ASTERIA_DEPTH_FUNCTION(vkCmdBindPipeline)
    ASTERIA_DEPTH_FUNCTION(vkCmdBindDescriptorSets)
    ASTERIA_DEPTH_FUNCTION(vkCmdPushConstants)
    ASTERIA_DEPTH_FUNCTION(vkCmdDispatch)
    ASTERIA_DEPTH_FUNCTION(vkDeviceWaitIdle)
#undef ASTERIA_DEPTH_FUNCTION

    void Load(VkDevice device, PFN_vkGetDeviceProcAddr resolver)
    {
#define ASTERIA_DEPTH_LOAD(name) name = reinterpret_cast<PFN_##name>(resolver(device, #name)); if (!name) throw std::runtime_error(#name);
        ASTERIA_DEPTH_LOAD(vkCreateImage)
        ASTERIA_DEPTH_LOAD(vkDestroyImage)
        ASTERIA_DEPTH_LOAD(vkGetImageMemoryRequirements)
        ASTERIA_DEPTH_LOAD(vkAllocateMemory)
        ASTERIA_DEPTH_LOAD(vkFreeMemory)
        ASTERIA_DEPTH_LOAD(vkBindImageMemory)
        ASTERIA_DEPTH_LOAD(vkCreateImageView)
        ASTERIA_DEPTH_LOAD(vkDestroyImageView)
        ASTERIA_DEPTH_LOAD(vkCreateShaderModule)
        ASTERIA_DEPTH_LOAD(vkDestroyShaderModule)
        ASTERIA_DEPTH_LOAD(vkCreateDescriptorSetLayout)
        ASTERIA_DEPTH_LOAD(vkDestroyDescriptorSetLayout)
        ASTERIA_DEPTH_LOAD(vkCreatePipelineLayout)
        ASTERIA_DEPTH_LOAD(vkDestroyPipelineLayout)
        ASTERIA_DEPTH_LOAD(vkCreateComputePipelines)
        ASTERIA_DEPTH_LOAD(vkDestroyPipeline)
        ASTERIA_DEPTH_LOAD(vkCreateDescriptorPool)
        ASTERIA_DEPTH_LOAD(vkDestroyDescriptorPool)
        ASTERIA_DEPTH_LOAD(vkAllocateDescriptorSets)
        ASTERIA_DEPTH_LOAD(vkUpdateDescriptorSets)
        ASTERIA_DEPTH_LOAD(vkCmdPipelineBarrier)
        ASTERIA_DEPTH_LOAD(vkCmdCopyImage)
        ASTERIA_DEPTH_LOAD(vkCmdBindPipeline)
        ASTERIA_DEPTH_LOAD(vkCmdBindDescriptorSets)
        ASTERIA_DEPTH_LOAD(vkCmdPushConstants)
        ASTERIA_DEPTH_LOAD(vkCmdDispatch)
        ASTERIA_DEPTH_LOAD(vkDeviceWaitIdle)
#undef ASTERIA_DEPTH_LOAD
    }
};
}
