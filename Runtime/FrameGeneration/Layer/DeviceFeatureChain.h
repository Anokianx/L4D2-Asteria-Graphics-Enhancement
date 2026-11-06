#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <algorithm>
#include <memory>
#include <vector>

namespace asteria_device_features
{
class OwnedChain
{
    std::vector<std::shared_ptr<void>> nodes;
    VkBaseOutStructure *head = nullptr;

    template<typename T> void CopyNode(const VkBaseInStructure *source)
    {
        nodes.push_back(std::make_shared<T>(*reinterpret_cast<const T *>(source)));
    }

public:
    VkBaseOutStructure *Head() { return head; }

    bool Copy(const void *source)
    {
        nodes.clear();
        head = nullptr;
        std::vector<const VkBaseInStructure *> observed;
        for (auto node = static_cast<const VkBaseInStructure *>(source); node; node = node->pNext)
        {
            if (observed.size() >= 64 || std::find(observed.begin(), observed.end(), node) != observed.end())
            {
                nodes.clear();
                return false;
            }
            observed.push_back(node);
            switch (node->sType)
            {
            case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO: CopyNode<VkLayerDeviceCreateInfo>(node); break;
            case VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO: CopyNode<VkDevicePrivateDataCreateInfo>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: CopyNode<VkPhysicalDeviceFeatures2>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: CopyNode<VkPhysicalDeviceVulkan11Features>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: CopyNode<VkPhysicalDeviceVulkan12Features>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: CopyNode<VkPhysicalDeviceVulkan13Features>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: CopyNode<VkPhysicalDevice16BitStorageFeatures>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: CopyNode<VkPhysicalDeviceShaderFloat16Int8Features>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: CopyNode<VkPhysicalDeviceTimelineSemaphoreFeatures>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: CopyNode<VkPhysicalDeviceSubgroupSizeControlFeatures>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES: CopyNode<VkPhysicalDevicePrivateDataFeatures>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: CopyNode<VkPhysicalDeviceDynamicRenderingFeatures>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES: CopyNode<VkPhysicalDeviceHostQueryResetFeatures>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT: CopyNode<VkPhysicalDeviceExtendedDynamicStateFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT: CopyNode<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT: CopyNode<VkPhysicalDeviceHostImageCopyFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAW_ACCESS_CHAINS_FEATURES_NV: CopyNode<VkPhysicalDeviceRawAccessChainsFeaturesNV>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR: CopyNode<VkPhysicalDeviceMaintenance5FeaturesKHR>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR: CopyNode<VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: CopyNode<VkPhysicalDeviceTransformFeedbackFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: CopyNode<VkPhysicalDeviceRobustness2FeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT: CopyNode<VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT: CopyNode<VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT: CopyNode<VkPhysicalDeviceMemoryPriorityFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_KHR: CopyNode<VkPhysicalDeviceLineRasterizationFeaturesKHR>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT: CopyNode<VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT: CopyNode<VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT: CopyNode<VkPhysicalDeviceDepthBiasControlFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT: CopyNode<VkPhysicalDeviceDepthClipEnableFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: CopyNode<VkPhysicalDeviceCustomBorderColorFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT: CopyNode<VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR: CopyNode<VkPhysicalDevicePresentIdFeaturesKHR>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR: CopyNode<VkPhysicalDevicePresentWaitFeaturesKHR>(node); break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT: CopyNode<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT>(node); break;
            default:
                nodes.clear();
                return false;
            }
        }
        for (size_t index = 0; index < nodes.size(); ++index)
        {
            auto *node = static_cast<VkBaseOutStructure *>(nodes[index].get());
            node->pNext = index + 1 < nodes.size() ? static_cast<VkBaseOutStructure *>(nodes[index + 1].get()) : nullptr;
        }
        if (!nodes.empty()) head = static_cast<VkBaseOutStructure *>(nodes.front().get());
        return true;
    }
};
}
