#pragma once
#include "VulkanFunctions.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace asteria_depth_capture
{
struct Source
{
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = 0;
    uint32_t width = 0, height = 0;
    uint64_t frameID = 0;
    uint64_t sceneDrawCalls = 0;
    bool scenePassConfirmed = false;
    bool insideRenderPass = true;
    bool reversedZ = false;
};

struct Output
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0, height = 0;
    uint64_t frameID = 0;
    bool reversedZ = false;
    bool recorded = false;
    bool gpuCompleted = false;
};

class Capture
{
    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    } snapshot, resolved;
    DeviceFunctions api;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    Output output;
    Source snapshotSource;
    bool snapshotRecorded = false;
    std::string error;

    bool Allocate(Image &target, VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void Barrier(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, VkImageLayout before, VkImageLayout after,
                 VkAccessFlags sourceAccess, VkAccessFlags destinationAccess);
    void Free(Image &target);
public:
    Capture() = default;
    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;
    ~Capture();
    bool Initialize(VkDevice device, PFN_vkGetDeviceProcAddr resolver, const VkPhysicalDeviceMemoryProperties &memory,
                    uint32_t width, uint32_t height, const std::vector<uint32_t> &resolveShader);
    bool RecordSnapshotBeforeClear(VkCommandBuffer commands, const Source &source);
    bool RecordResolve(VkCommandBuffer commands);
    bool RecordBeforeClear(VkCommandBuffer commands, const Source &source);
    VkResult WaitAndMarkComplete();
    void ReleaseAfterGpuIdle();
    const Output &Current() const { return output; }
    const std::string &Error() const { return error; }
};
}
