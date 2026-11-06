#include "AsteriaVulkanShim.h"
#include "AsteriaVkHandles.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using nlohmann::json;
constexpr uint32_t Width = 640;
constexpr uint32_t Height = 360;
constexpr uint32_t Frames = 18;
constexpr int MotionPixels = 12;

void Check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult=" + std::to_string(result));
}

void CheckFfx(FfxErrorCode result, const char* operation)
{
    if (result != FFX_OK)
        throw std::runtime_error(std::string(operation) + ": FfxErrorCode=" + std::to_string(result));
}

struct Image
{
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    FfxResourceDescription description{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct HostBuffer
{
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

VkFormat NativeFormat(FfxSurfaceFormat format)
{
    switch (format)
    {
    case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    default: throw std::runtime_error("Unexpected shared image format " + std::to_string(format));
    }
}

FfxResourceDescription Description(FfxSurfaceFormat format, uint32_t width = Width, uint32_t height = Height)
{
    FfxResourceDescription result{};
    result.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    result.format = format;
    result.width = width;
    result.height = height;
    result.depth = 1;
    result.mipCount = 1;
    result.usage = FFX_RESOURCE_USAGE_UAV;
    return result;
}

class Gpu
{
public:
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceProperties properties{};
    std::vector<Image> images;
    std::vector<HostBuffer> buffers;
    uint32_t family = 0;

    ~Gpu()
    {
        if (device)
        {
            vkDeviceWaitIdle(device);
            for (auto& image : images)
            {
                asteria_vk::Retire(asteria_vk::Image(device, image.handle));
                vkDestroyImage(device, image.handle, nullptr);
                vkFreeMemory(device, image.memory, nullptr);
            }
            for (const auto& buffer : buffers)
            {
                vkDestroyBuffer(device, buffer.handle, nullptr);
                vkFreeMemory(device, buffer.memory, nullptr);
            }
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            asteria_vk::RetireDevice(device);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
        AsteriaSetVulkanDispatch(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }

    void Initialize(bool presentation = false)
    {
        images.reserve(12);
        buffers.reserve(4);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Asteria GPU Interpolation Probe";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        if (presentation)
        {
            instanceInfo.enabledExtensionCount = 2;
            instanceInfo.ppEnabledExtensionNames = instanceExtensions;
        }
        Check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        if (!count) throw std::runtime_error("No Vulkan GPU");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        physical = devices.front();
        for (auto candidate : devices)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { physical = candidate; break; }
        }
        vkGetPhysicalDeviceProperties(physical, &properties);
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        auto it = std::find_if(families.begin(), families.end(), [](const auto& value) {
            return (value.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        });
        if (it == families.end()) throw std::runtime_error("No graphics/compute queue");
        family = uint32_t(it - families.begin());
        if (presentation && it->queueCount < 4) throw std::runtime_error("Presentation probe requires four separate queues");
        uint32_t extensionCount = 0;
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr), "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(extensionCount);
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data()), "vkEnumerateDeviceExtensionProperties");
        std::vector<const char*> enabled;
        if (presentation) enabled.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        for (auto name : {VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME})
            if (std::any_of(extensions.begin(), extensions.end(), [name](const auto& item) { return std::strcmp(item.extensionName, name) == 0; }))
                enabled.push_back(name);
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceSubgroupSizeControlFeatures subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &f11;
        f11.pNext = &f12;
        if (std::find(enabled.begin(), enabled.end(), VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) != enabled.end()) f12.pNext = &subgroup;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        float priorities[] = {1.0f, 1.0f, 1.0f, 1.0f};
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = presentation ? 4 : 1;
        queueInfo.pQueuePriorities = priorities;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = uint32_t(enabled.size());
        deviceInfo.ppEnabledExtensionNames = enabled.data();
        Check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        AsteriaSetVulkanDispatch(instance, device);
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        Check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &allocate, &command), "vkAllocateCommandBuffers");
    }

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags)
    {
        for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
            if ((bits & (1u << index)) && (memoryProperties.memoryTypes[index].propertyFlags & flags) == flags) return index;
        throw std::runtime_error("Required memory type not available");
    }

    Image& CreateImage(FfxResourceDescription desc)
    {
        images.emplace_back();
        auto& result = images.back();
        result.description = desc;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = NativeFormat(desc.format);
        info.extent = {desc.width, desc.height, 1};
        info.mipLevels = desc.mipCount;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateImage(device, &info, nullptr, &result.handle), "vkCreateImage");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, result.handle, &requirements);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory.allocationSize = requirements.size;
        memory.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(device, &memory, nullptr, &result.memory), "vkAllocateMemory(image)");
        Check(vkBindImageMemory(device, result.handle, result.memory, 0), "vkBindImageMemory");
        return result;
    }

    HostBuffer& CreateBuffer()
    {
        buffers.emplace_back();
        auto& result = buffers.back();
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = Width * Height * 4;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateBuffer(device, &info, nullptr, &result.handle), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.handle, &requirements);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory.allocationSize = requirements.size;
        memory.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(device, &memory, nullptr, &result.memory), "vkAllocateMemory(buffer)");
        Check(vkBindBufferMemory(device, result.handle, result.memory, 0), "vkBindBufferMemory");
        return result;
    }

    void Begin()
    {
        Check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    }

    void Submit()
    {
        Check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        Check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
    }

    void Barrier(Image& image, VkImageLayout next)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = image.layout;
        barrier.newLayout = next;
        barrier.srcAccessMask = image.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.handle;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.description.mipCount, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        image.layout = next;
    }

    void Upload(Image& image, HostBuffer& staging, const void* bytes, size_t size)
    {
        void* mapped = nullptr;
        Check(vkMapMemory(device, staging.memory, 0, size, 0, &mapped), "vkMapMemory(upload)");
        std::memcpy(mapped, bytes, size);
        vkUnmapMemory(device, staging.memory);
        Begin();
        Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {image.description.width, image.description.height, 1};
        vkCmdCopyBufferToImage(command, staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Submit();
    }

    std::vector<uint8_t> Readback(Image& image, HostBuffer& staging)
    {
        Begin();
        Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {image.description.width, image.description.height, 1};
        vkCmdCopyImageToBuffer(command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle, 1, &copy);
        Barrier(image, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = staging.handle;
        host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        Submit();
        std::vector<uint8_t> result(Width * Height * 4);
        void* mapped = nullptr;
        Check(vkMapMemory(device, staging.memory, 0, result.size(), 0, &mapped), "vkMapMemory(readback)");
        std::memcpy(result.data(), mapped, result.size());
        vkUnmapMemory(device, staging.memory);
        return result;
    }

    FfxResource Resource(Image& image, const wchar_t* name, FfxResourceStates state = FFX_RESOURCE_STATE_COMPUTE_READ)
    {
        return ffxGetImageResourceVK(device, image.handle, image.description, name, state);
    }
};

struct Effects
{
    std::unique_ptr<FfxFrameInterpolationContext> interpolation = std::make_unique<FfxFrameInterpolationContext>();
    std::unique_ptr<FfxOpticalflowContext> flow = std::make_unique<FfxOpticalflowContext>();
    bool interpolationCreated = false;
    bool flowCreated = false;
    VkDevice device = VK_NULL_HANDLE;
    ~Effects()
    {
        if (device) vkDeviceWaitIdle(device);
        if (interpolationCreated) ffxFrameInterpolationContextDestroy(interpolation.get());
        if (flowCreated) ffxOpticalflowContextDestroy(flow.get());
    }
};

uint16_t Half(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000;
    int exponent = int((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if (exponent <= 0)
    {
        if (exponent < -10) return uint16_t(sign);
        return uint16_t(sign | ((((mantissa | 0x800000) >> (1 - exponent)) + 0x1000) >> 13));
    }
    if (exponent >= 31) return uint16_t(sign | 0x7c00);
    return uint16_t(sign | (uint32_t(exponent) << 10) | ((mantissa + 0x1000) >> 13));
}

std::vector<uint8_t> Scene(uint32_t frame, std::vector<float>& depth, std::vector<uint16_t>& motion)
{
    std::vector<uint8_t> color(Width * Height * 4);
    depth.assign(Width * Height, 0.8f);
    motion.assign(Width * Height * 2, 0);
    int left = 104 + MotionPixels * int(frame);
    for (uint32_t y = 0; y < Height; ++y)
        for (uint32_t x = 0; x < Width; ++x)
        {
            size_t index = size_t(y) * Width + x;
            bool foreground = int(x) >= left && int(x) < left + 72 && y >= 100 && y < 260;
            uint8_t checker = uint8_t(((x / 24 + y / 24) % 2) * 20);
            color[index * 4] = foreground ? 240 : 20 + checker;
            color[index * 4 + 1] = foreground ? uint8_t(100 + ((x - left) / 8 % 2) * 35) : 28 + checker;
            color[index * 4 + 2] = foreground ? 32 : 52 + checker;
            color[index * 4 + 3] = 255;
            if (foreground)
            {
                depth[index] = 0.45f;
                motion[index * 2] = Half(frame ? -float(MotionPixels) / Width : 0.0f);
            }
        }
    return color;
}

double Centroid(const std::vector<uint8_t>& image)
{
    double sum = 0;
    size_t count = 0;
    for (uint32_t y = 100; y < 260; ++y)
        for (uint32_t x = 0; x < Width; ++x)
        {
            size_t index = (size_t(y) * Width + x) * 4;
            if (image[index] > 180 && image[index + 1] > 60 && image[index + 1] < 180 && image[index + 2] < 100)
            {
                sum += x;
                ++count;
            }
        }
    return count ? sum / count : -1.0;
}

size_t DifferentPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    size_t count = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        if (std::memcmp(a.data() + i, b.data() + i, 3)) ++count;
    return count;
}

void SaveImage(const std::filesystem::path& path, const std::vector<uint8_t>& image)
{
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot write image");
    stream << "P6\n" << Width << ' ' << Height << "\n255\n";
    for (size_t i = 0; i < image.size(); i += 4) stream.write(reinterpret_cast<const char*>(image.data() + i), 3);
    stream.flush();
    if (!stream) throw std::runtime_error("Failed to write complete GPU image");
}

#ifndef ASTERIA_INTERPOLATION_PROBE_LIBRARY
int main(int argc, char** argv)
{
    std::filesystem::path output = argc > 1 ? argv[1] : "results/interpolation-probe.json";
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    json report = {{"process_bits", sizeof(void*) * 8}, {"game_frame_generation", false}, {"presented_generated_frames", 0}, {"synthetic_inputs", true}, {"gpu_context_created", false}, {"verified_intermediate_frames", 0}};
    std::string stage = "initialize_vulkan";
    int result = 1;
    try
    {
        Gpu gpu;
        gpu.Initialize();
        report["gpu"] = gpu.properties.deviceName;
        std::cout << "Vulkan initialized: " << gpu.properties.deviceName << std::endl;
        stage = "initialize_backend";
        VkDeviceContext deviceContext{gpu.device, gpu.physical, vkGetDeviceProcAddr};
        size_t scratchSize = ffxGetScratchMemorySizeVK(gpu.physical, 2);
        std::vector<uint8_t> scratch(scratchSize);
        FfxInterface backend{};
        CheckFfx(ffxGetInterfaceVK(&backend, ffxGetDeviceVK(&deviceContext), scratch.data(), scratch.size(), 2), "ffxGetInterfaceVK");
        Effects effects;
        effects.device = gpu.device;
        stage = "create_frame_interpolation_context";
        FfxFrameInterpolationContextDescription fi{};
        fi.maxRenderSize = {Width, Height};
        fi.displaySize = {Width, Height};
        fi.backBufferFormat = fi.previousInterpolationSourceFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
        fi.backendInterface = backend;
        CheckFfx(ffxFrameInterpolationContextCreate(effects.interpolation.get(), &fi), "ffxFrameInterpolationContextCreate");
        effects.interpolationCreated = true;
        std::cout << "Frame Interpolation context created" << std::endl;
        stage = "create_optical_flow_context";
        FfxOpticalflowContextDescription of{};
        of.resolution = {Width, Height};
        of.backendInterface = backend;
        CheckFfx(ffxOpticalflowContextCreate(effects.flow.get(), &of), "ffxOpticalflowContextCreate");
        effects.flowCreated = true;
        std::cout << "Optical Flow context created" << std::endl;
        report["gpu_context_created"] = true;
        report["scratch_bytes"] = scratchSize;
        stage = "create_shared_images";
        FfxFrameInterpolationSharedResourceDescriptions fiShared{};
        FfxOpticalflowSharedResourceDescriptions ofShared{};
        CheckFfx(ffxFrameInterpolationGetSharedResourceDescriptions(effects.interpolation.get(), &fiShared), "ffxFrameInterpolationGetSharedResourceDescriptions");
        CheckFfx(ffxOpticalflowGetSharedResourceDescriptions(effects.flow.get(), &ofShared), "ffxOpticalflowGetSharedResourceDescriptions");
        auto& color = gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM));
        auto& depth = gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT));
        auto& motion = gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R16G16_FLOAT));
        auto& reconstructed = gpu.CreateImage(fiShared.reconstructedPrevNearestDepth.resourceDescription);
        auto& dilatedDepth = gpu.CreateImage(fiShared.dilatedDepth.resourceDescription);
        auto& dilatedMotion = gpu.CreateImage(fiShared.dilatedMotionVectors.resourceDescription);
        auto& vectors = gpu.CreateImage(ofShared.opticalFlowVector.resourceDescription);
        auto& scd = gpu.CreateImage(ofShared.opticalFlowSCD.resourceDescription);
        auto& generated = gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R8G8B8A8_UNORM));
        auto& upload = gpu.CreateBuffer();
        auto& readback = gpu.CreateBuffer();
        gpu.Begin();
        for (auto image : {&reconstructed, &dilatedDepth, &dilatedMotion, &vectors, &scd, &generated}) gpu.Barrier(*image, VK_IMAGE_LAYOUT_GENERAL);
        gpu.Submit();
        std::vector<uint8_t> previous;
        report["frames"] = json::array();
        uint32_t verified = 0;
        for (uint32_t frame = 0; frame < Frames; ++frame)
        {
            stage = "upload_frame_" + std::to_string(frame);
            std::vector<float> depthPixels;
            std::vector<uint16_t> motionPixels;
            auto current = Scene(frame, depthPixels, motionPixels);
            gpu.Upload(color, upload, current.data(), current.size());
            gpu.Upload(depth, upload, depthPixels.data(), depthPixels.size() * sizeof(float));
            gpu.Upload(motion, upload, motionPixels.data(), motionPixels.size() * sizeof(uint16_t));
            gpu.Begin();
            FfxOpticalflowDispatchDescription flowDispatch{};
            flowDispatch.commandList = ffxGetCommandListVK(gpu.command);
            flowDispatch.color = gpu.Resource(color, L"Input color");
            flowDispatch.opticalFlowVector = gpu.Resource(vectors, L"Optical flow", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            flowDispatch.opticalFlowSCD = gpu.Resource(scd, L"Scene change", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            flowDispatch.reset = frame == 0;
            flowDispatch.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
            flowDispatch.minMaxLuminance = {0.0f, 1.0f};
            stage = "optical_flow_dispatch_" + std::to_string(frame);
            CheckFfx(ffxOpticalflowContextDispatch(effects.flow.get(), &flowDispatch), "ffxOpticalflowContextDispatch");
            FfxFrameInterpolationPrepareDescription prepare{};
            prepare.commandList = ffxGetCommandListVK(gpu.command);
            prepare.renderSize = {Width, Height};
            prepare.motionVectorScale = {float(Width), float(Height)};
            prepare.frameTimeDelta = 1000.0f / 60.0f;
            prepare.cameraNear = 0.1f;
            prepare.cameraFar = 100.0f;
            prepare.cameraFovAngleVertical = 1.0f;
            prepare.viewSpaceToMetersFactor = 1.0f;
            prepare.depth = gpu.Resource(depth, L"Scene depth");
            prepare.motionVectors = gpu.Resource(motion, L"Scene motion");
            prepare.frameID = frame;
            prepare.dilatedDepth = gpu.Resource(dilatedDepth, L"Dilated depth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            prepare.dilatedMotionVectors = gpu.Resource(dilatedMotion, L"Dilated motion", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            prepare.reconstructedPrevDepth = gpu.Resource(reconstructed, L"Reconstructed depth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            prepare.cameraUp[1] = 1.0f;
            prepare.cameraRight[0] = 1.0f;
            prepare.cameraForward[2] = 1.0f;
            stage = "prepare_dispatch_" + std::to_string(frame);
            CheckFfx(ffxFrameInterpolationPrepare(effects.interpolation.get(), &prepare), "ffxFrameInterpolationPrepare");
            FfxFrameInterpolationDispatchDescription dispatch{};
            dispatch.commandList = ffxGetCommandListVK(gpu.command);
            dispatch.displaySize = dispatch.renderSize = {Width, Height};
            dispatch.currentBackBuffer = gpu.Resource(color, L"Input color");
            dispatch.output = gpu.Resource(generated, L"Generated color", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.interpolationRect = {0, 0, Width, Height};
            dispatch.opticalFlowVector = gpu.Resource(vectors, L"Optical flow", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.opticalFlowSceneChangeDetection = gpu.Resource(scd, L"Scene change", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.opticalFlowBufferSize = {vectors.description.width, vectors.description.height};
            dispatch.opticalFlowScale = {1.0f / Width, 1.0f / Height};
            dispatch.opticalFlowBlockSize = 8;
            dispatch.cameraNear = prepare.cameraNear;
            dispatch.cameraFar = prepare.cameraFar;
            dispatch.cameraFovAngleVertical = prepare.cameraFovAngleVertical;
            dispatch.viewSpaceToMetersFactor = 1.0f;
            dispatch.frameTimeDelta = prepare.frameTimeDelta;
            dispatch.reset = frame == 0;
            dispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
            dispatch.minMaxLuminance[0] = 0.0f;
            dispatch.minMaxLuminance[1] = 1.0f;
            dispatch.frameID = frame;
            dispatch.dilatedDepth = prepare.dilatedDepth;
            dispatch.dilatedMotionVectors = prepare.dilatedMotionVectors;
            dispatch.reconstructedPrevDepth = prepare.reconstructedPrevDepth;
            stage = "interpolation_dispatch_" + std::to_string(frame);
            CheckFfx(ffxFrameInterpolationDispatch(effects.interpolation.get(), &dispatch), "ffxFrameInterpolationDispatch");
            stage = "gpu_submit_" + std::to_string(frame);
            gpu.Submit();
            auto intermediate = gpu.Readback(generated, readback);
            double nowCenter = Centroid(current);
            double middleCenter = Centroid(intermediate);
            json sample = {{"frame_id", frame}, {"current_center_x", nowCenter}, {"generated_center_x", middleCenter}, {"pixels_different_from_current", DifferentPixels(intermediate, current)}};
            if (frame)
            {
                double prevCenter = Centroid(previous);
                size_t fromPrevious = DifferentPixels(intermediate, previous);
                bool between = middleCenter > prevCenter + 1.0 && middleCenter < nowCenter - 1.0;
                bool distinct = fromPrevious > 500 && DifferentPixels(intermediate, current) > 500;
                sample["previous_center_x"] = prevCenter;
                sample["expected_center_x"] = (prevCenter + nowCenter) * 0.5;
                sample["pixels_different_from_previous"] = fromPrevious;
                sample["intermediate_position_verified"] = between && distinct;
                if (between && distinct) ++verified;
                if (frame == Frames - 1)
                {
                    SaveImage(output.parent_path() / "previous.ppm", previous);
                    SaveImage(output.parent_path() / "generated.ppm", intermediate);
                    SaveImage(output.parent_path() / "current.ppm", current);
                }
            }
            report["frames"].push_back(sample);
            previous = std::move(current);
            std::cout << "Frame " << frame << ": current=" << nowCenter << " generated=" << middleCenter << std::endl;
        }
        report["dispatch_frames"] = Frames;
        report["verified_intermediate_frames"] = verified;
        report["actual_gpu_interpolation_verified"] = verified == Frames - 1;
        report["success"] = verified == Frames - 1;
        if (!report["success"].get<bool>()) throw std::runtime_error("GPU output did not pass intermediate-position validation");
        stage = "complete";
        result = 0;
    }
    catch (const std::exception& error)
    {
        report["success"] = false;
        report["error"] = error.what();
        std::cerr << stage << ": " << error.what() << std::endl;
    }
    report["stage"] = stage;
    std::ofstream stream(output);
    stream << report.dump(2) << '\n';
    stream.flush();
    if (!stream)
    {
        std::cerr << "Failed to write complete verification report" << std::endl;
        return 1;
    }
    std::cout << "Report: " << output.string() << std::endl;
    return result;
}
#endif
