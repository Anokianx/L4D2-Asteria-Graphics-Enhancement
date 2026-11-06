#include "DepthCapture.h"
#include <algorithm>
#include <cstring>

namespace asteria_depth_capture
{
Capture::~Capture()
{
    if (device && api.vkDeviceWaitIdle && api.vkDeviceWaitIdle(device) == VK_SUCCESS) ReleaseAfterGpuIdle();
}

bool Capture::Allocate(Image &target, VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (api.vkCreateImage(device, &info, nullptr, &target.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    api.vkGetImageMemoryRequirements(device, target.image, &requirements);
    uint32_t selected = UINT32_MAX;
    for (uint32_t index = 0; index < memory.memoryTypeCount; ++index)
        if ((requirements.memoryTypeBits & (1u << index)) && (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        { selected = index; break; }
    if (selected == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = selected;
    if (api.vkAllocateMemory(device, &allocation, nullptr, &target.memory) != VK_SUCCESS ||
        api.vkBindImageMemory(device, target.image, target.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = target.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    return api.vkCreateImageView(device, &view, nullptr, &target.view) == VK_SUCCESS;
}

void Capture::Free(Image &target)
{
    if (target.view) api.vkDestroyImageView(device, target.view, nullptr);
    if (target.image) api.vkDestroyImage(device, target.image, nullptr);
    if (target.memory) api.vkFreeMemory(device, target.memory, nullptr);
    target = {};
}

void Capture::Barrier(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, VkImageLayout before, VkImageLayout after,
                      VkAccessFlags sourceAccess, VkAccessFlags destinationAccess)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};
    api.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
}

bool Capture::Initialize(VkDevice targetDevice, PFN_vkGetDeviceProcAddr resolver, const VkPhysicalDeviceMemoryProperties &properties,
                         uint32_t targetWidth, uint32_t targetHeight, const std::vector<uint32_t> &resolveShader)
{
    if (device || !targetDevice || !resolver || !targetWidth || !targetHeight || resolveShader.empty()) return false;
    device = targetDevice;
    width = targetWidth;
    height = targetHeight;
    memory = properties;
    try { api.Load(device, resolver); }
    catch (const std::exception &failure) { error = failure.what(); device = VK_NULL_HANDLE; return false; }
    if (!Allocate(snapshot, VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_4_BIT,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !Allocate(resolved, VK_FORMAT_R32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
    { error = "GPU depth snapshot or resolve image allocation failed"; return false; }
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor.bindingCount = 2;
    descriptor.pBindings = bindings;
    if (api.vkCreateDescriptorSetLayout(device, &descriptor, nullptr, &descriptorLayout) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptorLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (api.vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo code{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    code.codeSize = resolveShader.size() * sizeof(uint32_t);
    code.pCode = resolveShader.data();
    if (api.vkCreateShaderModule(device, &code, nullptr, &shader) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute.stage.module = shader;
    compute.stage.pName = "main";
    compute.layout = pipelineLayout;
    if (api.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline) != VK_SUCCESS) return false;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (api.vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set.descriptorPool = descriptorPool;
    set.descriptorSetCount = 1;
    set.pSetLayouts = &descriptorLayout;
    if (api.vkAllocateDescriptorSets(device, &set, &descriptorSet) != VK_SUCCESS) return false;
    VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, snapshot.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                     {VK_NULL_HANDLE, resolved.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t index = 0; index < 2; ++index)
    {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = descriptorSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = index == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[index].pImageInfo = &images[index];
    }
    api.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    return true;
}

bool Capture::RecordSnapshotBeforeClear(VkCommandBuffer commands, const Source &source)
{
    if (!device || !pipeline || !commands || !source.image || source.image == snapshot.image ||
        source.image == resolved.image || source.format != VK_FORMAT_D24_UNORM_S8_UINT || source.samples != VK_SAMPLE_COUNT_4_BIT ||
        source.width != width || source.height != height || !(source.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        source.layout == VK_IMAGE_LAYOUT_UNDEFINED || source.insideRenderPass || !source.scenePassConfirmed || !source.sceneDrawCalls)
    { error = "Depth source is unsupported, unconfirmed, or not safely outside its render pass"; return false; }
    const auto depthStencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    Barrier(commands, source.image, depthStencil, source.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    Barrier(commands, snapshot.image, depthStencil, snapshot.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    api.vkCmdCopyImage(commands, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       snapshot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Barrier(commands, source.image, depthStencil, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.layout,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    Barrier(commands, snapshot.image, depthStencil, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    snapshot.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    snapshotSource = source;
    snapshotRecorded = true;
    output.recorded = false;
    output.gpuCompleted = false;
    error.clear();
    return true;
}

bool Capture::RecordResolve(VkCommandBuffer commands)
{
    if (!device || !pipeline || !commands || !snapshotRecorded)
    { error = "No recorded depth snapshot is available to resolve"; return false; }
    const auto &source = snapshotSource;
    Barrier(commands, resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, resolved.layout, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    api.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    api.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    const uint32_t parameters[] = {width, height, source.reversedZ ? 1u : 0u, 4u};
    api.vkCmdPushConstants(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), parameters);
    api.vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
    Barrier(commands, resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    resolved.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    output = {resolved.image, resolved.view, resolved.layout, width, height, source.frameID, source.reversedZ, true, false};
    error.clear();
    return true;
}

bool Capture::RecordBeforeClear(VkCommandBuffer commands, const Source &source)
{
    return RecordSnapshotBeforeClear(commands, source) && RecordResolve(commands);
}

VkResult Capture::WaitAndMarkComplete()
{
    if (!device || !output.recorded) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = api.vkDeviceWaitIdle(device);
    if (result == VK_SUCCESS) output.gpuCompleted = true;
    return result;
}

void Capture::ReleaseAfterGpuIdle()
{
    if (!device) return;
    if (descriptorPool) api.vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (pipeline) api.vkDestroyPipeline(device, pipeline, nullptr);
    if (shader) api.vkDestroyShaderModule(device, shader, nullptr);
    if (pipelineLayout) api.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (descriptorLayout) api.vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
    Free(resolved);
    Free(snapshot);
    descriptorPool = VK_NULL_HANDLE;
    pipeline = VK_NULL_HANDLE;
    shader = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    descriptorLayout = VK_NULL_HANDLE;
    descriptorSet = VK_NULL_HANDLE;
    output = {};
    snapshotSource = {};
    snapshotRecorded = false;
    device = VK_NULL_HANDLE;
}
}
