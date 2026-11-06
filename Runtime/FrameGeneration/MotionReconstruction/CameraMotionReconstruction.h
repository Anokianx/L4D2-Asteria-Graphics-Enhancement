#pragma once
#include "NativeViewportTransform.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace asteria_camera_motion
{
inline constexpr const char *Author = "Anokianx";
inline constexpr const char *MotionConvention = "previous_uv_minus_current_uv_normalized_top_left_y_down";
inline constexpr const char *DepthConvention = "D3D_device_depth_zero_to_one_forward_clear_one";
inline constexpr const char *MatrixConvention = "row_major_matrix_times_column_vector";
inline constexpr const char *ConfidenceMeaning = "static_world_reprojection_and_optional_previous_depth_consistency_only";

struct alignas(16) Parameters
{
    std::array<float, 16> inverseCurrentViewProjection{};
    std::array<float, 16> previousViewProjection{};
    std::array<uint32_t, 4> extentFlags{};
    std::array<float, 4> depthLimits{0.0f, 1.0f, 1.0f, 1e-6f};
    std::array<float, 4> previousDepthControls{0.002f, 0.01f, 0, 0};
    std::array<float, 4> currentViewportRectangle{}, previousViewportRectangle{};
    std::array<float, 4> viewportDepthRanges{0, 1, 0, 1};
    std::array<uint32_t, 4> viewportModes{};
};
static_assert(sizeof(Parameters) == 240, "motion reconstruction uniform ABI");

inline void SetNativeViewports(Parameters &parameters, const NativeViewport &current, const NativeViewport &previous)
{
    parameters.currentViewportRectangle = current.rectangle;
    parameters.previousViewportRectangle = previous.rectangle;
    parameters.viewportDepthRanges = {current.depthRange[0], current.depthRange[1], previous.depthRange[0], previous.depthRange[1]};
    parameters.viewportModes = {uint32_t(ViewportMode::NativeVulkanZeroToOne), uint32_t(ViewportMode::NativeVulkanZeroToOne),
                               current.known ? 1u : 0u, previous.known ? 1u : 0u};
}

inline bool ValidViewportParameters(const Parameters &parameters, bool previous)
{
    const uint32_t index = previous ? 1u : 0u;
    const auto mode = parameters.viewportModes[index];
    if (mode == uint32_t(ViewportMode::DefaultTopLeft)) return true;
    if (mode != uint32_t(ViewportMode::NativeVulkanZeroToOne)) return false;
    NativeViewport viewport;
    viewport.rectangle = previous ? parameters.previousViewportRectangle : parameters.currentViewportRectangle;
    viewport.depthRange = {parameters.viewportDepthRanges[index * 2], parameters.viewportDepthRanges[index * 2 + 1]};
    viewport.known = parameters.viewportModes[index + 2] == 1;
    return ValidNativeViewport(viewport);
}

inline bool ValidMatrix(const std::array<float, 16> &matrix)
{
    double rows[4][4]{};
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t column = 0; column < 4; ++column)
        {
            if (!std::isfinite(matrix[row * 4 + column])) return false;
            rows[row][column] = matrix[row * 4 + column];
        }
    for (uint32_t column = 0; column < 4; ++column)
    {
        uint32_t pivot = column;
        for (uint32_t row = column + 1; row < 4; ++row)
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column])) pivot = row;
        if (std::abs(rows[pivot][column]) <= 1e-10) return false;
        for (uint32_t index = column; index < 4; ++index) std::swap(rows[pivot][index], rows[column][index]);
        for (uint32_t row = column + 1; row < 4; ++row)
        {
            const double ratio = rows[row][column] / rows[column][column];
            for (uint32_t index = column; index < 4; ++index) rows[row][index] -= ratio * rows[column][index];
        }
    }
    return true;
}

inline Parameters ValidatedParameters(Parameters parameters)
{
    bool valid = ValidMatrix(parameters.inverseCurrentViewProjection) && ValidMatrix(parameters.previousViewProjection);
    for (float value : parameters.depthLimits) valid &= std::isfinite(value);
    for (float value : parameters.previousDepthControls) valid &= std::isfinite(value);
    valid &= parameters.depthLimits[0] >= 0 && parameters.depthLimits[1] <= 1 && parameters.depthLimits[0] < parameters.depthLimits[1];
    valid &= parameters.depthLimits[3] >= 0 && parameters.previousDepthControls[0] >= 0 && parameters.previousDepthControls[1] >= 0;
    valid &= ValidViewportParameters(parameters, false) && ValidViewportParameters(parameters, true);
    if (!valid) parameters.extentFlags[2] &= ~1u;
    return parameters;
}

struct Views
{
    VkImageView currentDepth = VK_NULL_HANDLE;
    VkImageView previousDepth = VK_NULL_HANDLE;
    VkImageView motion = VK_NULL_HANDLE;
    VkImageView confidence = VK_NULL_HANDLE;
};

struct DispatchLifetime
{
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkBuffer uniform = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ~DispatchLifetime()
    {
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        if (uniform) vkDestroyBuffer(device, uniform, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
};

class Pipeline
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    static void Require(VkResult value, const char *operation)
    {
        if (value != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(value));
    }
public:
    Pipeline() = default;
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;
    ~Pipeline() { DestroyAfterGpuIdle(); }
    void DestroyAfterGpuIdle()
    {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (descriptors) vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
        pipeline = VK_NULL_HANDLE; layout = VK_NULL_HANDLE; descriptors = VK_NULL_HANDLE; device = VK_NULL_HANDLE; physical = VK_NULL_HANDLE;
    }
    void Initialize(VkDevice owner, VkPhysicalDevice adapter, const std::vector<uint32_t> &spirv)
    {
        if (device || !owner || !adapter || spirv.size() < 5 || spirv[0] != 0x07230203) throw std::runtime_error("invalid reconstruction pipeline initialization");
        device = owner; physical = adapter;
        VkDescriptorSetLayoutBinding bindings[5]{};
        for (uint32_t index = 0; index < 5; ++index)
        {
            bindings[index].binding = index; bindings[index].descriptorCount = 1; bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[index].descriptorType = index == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 5; setInfo.pBindings = bindings;
        Require(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &descriptors), "vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &descriptors;
        Require(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = spirv.size() * sizeof(uint32_t); moduleInfo.pCode = spirv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        Require(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.layout = layout; compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; compute.stage.module = module; compute.stage.pName = "main";
        const VkResult created = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        Require(created, "vkCreateComputePipelines");
    }
    std::shared_ptr<DispatchLifetime> Record(VkCommandBuffer command, Views views, Parameters parameters)
    {
        if (!pipeline || !command || !views.currentDepth || !views.motion || !views.confidence || !parameters.extentFlags[0] || !parameters.extentFlags[1])
            throw std::runtime_error("invalid reconstruction dispatch bindings");
        if ((parameters.extentFlags[2] & 4u) && !views.previousDepth) throw std::runtime_error("previous depth verification requested without previous depth");
        if (!views.previousDepth) views.previousDepth = views.currentDepth;
        parameters = ValidatedParameters(parameters);
        auto lifetime = std::make_shared<DispatchLifetime>(); lifetime->device = device;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = sizeof(parameters); bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        Require(vkCreateBuffer(device, &bufferInfo, nullptr, &lifetime->uniform), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, lifetime->uniform, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        uint32_t memoryType = UINT32_MAX;
        for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
            if ((requirements.memoryTypeBits & (1u << index)) &&
                (memoryProperties.memoryTypes[index].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { memoryType = index; break; }
        if (memoryType == UINT32_MAX) throw std::runtime_error("reconstruction requires coherent host-visible uniform memory");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memoryType;
        Require(vkAllocateMemory(device, &allocation, nullptr, &lifetime->memory), "vkAllocateMemory");
        Require(vkBindBufferMemory(device, lifetime->uniform, lifetime->memory, 0), "vkBindBufferMemory");
        void *mapped = nullptr;
        Require(vkMapMemory(device, lifetime->memory, 0, sizeof(parameters), 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, &parameters, sizeof(parameters)); vkUnmapMemory(device, lifetime->memory);
        const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1; poolInfo.poolSizeCount = 2; poolInfo.pPoolSizes = sizes;
        Require(vkCreateDescriptorPool(device, &poolInfo, nullptr, &lifetime->pool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = lifetime->pool; setInfo.descriptorSetCount = 1; setInfo.pSetLayouts = &descriptors;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Require(vkAllocateDescriptorSets(device, &setInfo, &set), "vkAllocateDescriptorSets");
        const VkImageView imageViews[]{views.currentDepth, views.previousDepth, views.motion, views.confidence};
        VkDescriptorImageInfo images[4]{}; VkWriteDescriptorSet writes[5]{};
        for (uint32_t index = 0; index < 5; ++index)
        {
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[index].dstSet = set;
            writes[index].dstBinding = index; writes[index].descriptorCount = 1;
            writes[index].descriptorType = index == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (index < 4) { images[index].imageView = imageViews[index]; images[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL; writes[index].pImageInfo = &images[index]; }
        }
        VkDescriptorBufferInfo uniform{lifetime->uniform, 0, sizeof(parameters)}; writes[4].pBufferInfo = &uniform;
        vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(command, (parameters.extentFlags[0] + 7) / 8, (parameters.extentFlags[1] + 7) / 8, 1);
        return lifetime;
    }
};
}
