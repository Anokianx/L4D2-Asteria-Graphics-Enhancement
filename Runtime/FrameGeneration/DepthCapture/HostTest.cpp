#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "DepthCapture.h"
#include "CandidateSelector.h"
#include <json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using Json = nlohmann::json;
namespace fs = std::filesystem;
static uint32_t Checks = 0;
static void Check(bool value, const char *message) { ++Checks; if (!value) throw std::runtime_error(message); }
static void Success(VkResult result, const char *message) { Check(result == VK_SUCCESS, message); }
template<typename T> static T Entry(PFN_vkVoidFunction entry, const char *name)
{
    if (!entry) throw std::runtime_error(name);
    return reinterpret_cast<T>(entry);
}
static std::vector<uint32_t> Shader(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Check(bool(file), "compiled fixture shader exists");
    const auto bytes = static_cast<size_t>(file.tellg());
    Check(bytes > 0 && bytes % 4 == 0, "SPIRV byte count is aligned");
    std::vector<uint32_t> words(bytes / 4);
    file.seekg(0); file.read(reinterpret_cast<char *>(words.data()), bytes);
    return words;
}

int wmain(int argc, wchar_t **argv)
{
    const fs::path root = fs::absolute(argv[0]).parent_path().parent_path();
    const fs::path results = root / L"results";
    fs::create_directories(results);
    Json report = {{"author", "Anokianx"}, {"passed", false}, {"cpu_generated_depth_uploaded", false},
                   {"game_started", false}, {"native_game_depth_validated", false}, {"fg_ready", false}, {"process_bits", sizeof(void *) * 8}};
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage source = VK_NULL_HANDLE;
    VkDeviceMemory sourceMemory = VK_NULL_HANDLE;
    VkImageView sourceView = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    HMODULE vulkan = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    asteria_depth_capture::Capture capture;
#define INSTANCE(name) Entry<PFN_##name>(gipa(instance, #name), #name)
#define DEVICE(name) Entry<PFN_##name>(gdpa(device, #name), #name)
    try
    {
        SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", L"1");
        SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", nullptr);
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        vulkan = LoadLibraryExW((fs::path(system) / L"vulkan-1.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        Check(vulkan != nullptr, "system Vulkan loader is available");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
        Check(gipa != nullptr, "system Vulkan entry is available");
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Asteria isolated GPU depth capture";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &application;
        Success(INSTANCE(vkCreateInstance)(&instanceInfo, nullptr, &instance), "create isolated Vulkan instance");
        uint32_t count = 0;
        Success(INSTANCE(vkEnumeratePhysicalDevices)(instance, &count, nullptr), "enumerate GPU count");
        std::vector<VkPhysicalDevice> physicals(count);
        Success(INSTANCE(vkEnumeratePhysicalDevices)(instance, &count, physicals.data()), "enumerate GPUs");
        Check(!physicals.empty(), "physical GPU exists");
        VkPhysicalDevice physical = physicals.front();
        VkPhysicalDeviceProperties properties{};
        INSTANCE(vkGetPhysicalDeviceProperties)(physical, &properties);
        report["gpu"] = properties.deviceName;
        VkPhysicalDeviceFeatures features{};
        INSTANCE(vkGetPhysicalDeviceFeatures)(physical, &features);
        Check(features.sampleRateShading != VK_FALSE, "GPU supports actual per-sample depth rendering");
        VkImageFormatProperties depthProperties{};
        Success(INSTANCE(vkGetPhysicalDeviceImageFormatProperties)(physical, VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            0, &depthProperties), "GPU supports D24S8 depth source format");
        Check((depthProperties.sampleCounts & VK_SAMPLE_COUNT_4_BIT) != 0, "GPU supports four depth samples");
        uint32_t familyCount = 0;
        INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties)(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties)(physical, &familyCount, families.data());
        uint32_t family = UINT32_MAX;
        for (uint32_t index = 0; index < familyCount; ++index)
            if ((families[index].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            { family = index; break; }
        Check(family != UINT32_MAX, "combined graphics and compute queue exists");
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{}; enabled.sampleRateShading = VK_TRUE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo; deviceInfo.pEnabledFeatures = &enabled;
        Success(INSTANCE(vkCreateDevice)(physical, &deviceInfo, nullptr, &device), "create isolated GPU device");
        gdpa = Entry<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr"), "device resolver");
        VkQueue queue = VK_NULL_HANDLE;
        DEVICE(vkGetDeviceQueue)(device, family, 0, &queue);
        VkPhysicalDeviceMemoryProperties memory{};
        INSTANCE(vkGetPhysicalDeviceMemoryProperties)(physical, &memory);
        const auto memoryType = [&](uint32_t mask, VkMemoryPropertyFlags requested) {
            for (uint32_t index = 0; index < memory.memoryTypeCount; ++index)
                if ((mask & (1u << index)) && (memory.memoryTypes[index].propertyFlags & requested) == requested) return index;
            throw std::runtime_error("required memory type is absent");
        };
        constexpr uint32_t width = 64, height = 48;
        constexpr VkDeviceSize frameBytes = width * height * sizeof(float);
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D; image.format = VK_FORMAT_D24_UNORM_S8_UINT;
        image.extent = {width, height, 1}; image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_4_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Success(DEVICE(vkCreateImage)(device, &image, nullptr, &source), "create actual multisampled depth attachment");
        VkMemoryRequirements requirements{};
        DEVICE(vkGetImageMemoryRequirements)(device, source, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Success(DEVICE(vkAllocateMemory)(device, &allocation, nullptr, &sourceMemory), "allocate actual depth attachment memory");
        Success(DEVICE(vkBindImageMemory)(device, source, sourceMemory, 0), "bind actual depth attachment memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = source; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        Success(DEVICE(vkCreateImageView)(device, &view, nullptr, &sourceView), "create fixture depth attachment view");
        VkAttachmentDescription attachment{};
        attachment.format = image.format; attachment.samples = image.samples;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.pDepthStencilAttachment = &reference;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 1; pass.pAttachments = &attachment; pass.subpassCount = 1; pass.pSubpasses = &subpass;
        Success(DEVICE(vkCreateRenderPass)(device, &pass, nullptr, &renderPass), "create actual depth-writing render pass");
        VkFramebufferCreateInfo target{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        target.renderPass = renderPass; target.attachmentCount = 1; target.pAttachments = &sourceView;
        target.width = width; target.height = height; target.layers = 1;
        Success(DEVICE(vkCreateFramebuffer)(device, &target, nullptr, &framebuffer), "create depth framebuffer");
        const auto makeShader = [&](const fs::path &path, VkShaderModule *module) {
            const auto words = Shader(path);
            VkShaderModuleCreateInfo code{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; code.codeSize = words.size() * 4; code.pCode = words.data();
            Success(DEVICE(vkCreateShaderModule)(device, &code, nullptr, module), "create GPU fixture shader");
        };
        makeShader(root / L"bin/Fixture.vert.spv", &vertex);
        makeShader(root / L"bin/Fixture.frag.spv", &fragment);
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        Success(DEVICE(vkCreatePipelineLayout)(device, &layout, nullptr, &pipelineLayout), "create fixture pipeline layout");
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto &stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment;
        VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, float(width), float(height), 0, 1};
        VkRect2D scissor{{0, 0}, {width, height}};
        VkPipelineViewportStateCreateInfo viewports{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewports.viewportCount = viewports.scissorCount = 1; viewports.pViewports = &viewport; viewports.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT; samples.sampleShadingEnable = VK_TRUE; samples.minSampleShading = 1;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        graphics.stageCount = 2; graphics.pStages = stages; graphics.pVertexInputState = &vertices;
        graphics.pInputAssemblyState = &assembly; graphics.pViewportState = &viewports; graphics.pRasterizationState = &raster;
        graphics.pMultisampleState = &samples; graphics.pDepthStencilState = &depth; graphics.pColorBlendState = &blend;
        graphics.layout = pipelineLayout; graphics.renderPass = renderPass;
        Success(DEVICE(vkCreateGraphicsPipelines)(device, VK_NULL_HANDLE, 1, &graphics, nullptr, &pipeline), "create real depth fixture graphics pipeline");
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; buffer.size = frameBytes * 3; buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Success(DEVICE(vkCreateBuffer)(device, &buffer, nullptr, &readback), "create GPU readback destination");
        DEVICE(vkGetBufferMemoryRequirements)(device, readback, &requirements);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Success(DEVICE(vkAllocateMemory)(device, &allocation, nullptr, &readbackMemory), "allocate GPU-to-CPU readback memory");
        Success(DEVICE(vkBindBufferMemory)(device, readback, readbackMemory, 0), "bind readback memory");
        Check(capture.Initialize(device, gdpa, memory, width, height, Shader(root / L"bin/ResolveDepth.spv")), "initialize real depth snapshot and conversion module");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = family;
        Success(DEVICE(vkCreateCommandPool)(device, &pool, nullptr, &commandPool), "create command pool");
        VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = commandPool; commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; commands.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        Success(DEVICE(vkAllocateCommandBuffers)(device, &commands, &command), "allocate GPU command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Success(DEVICE(vkBeginCommandBuffer)(command, &begin), "begin GPU command recording");
        const auto imageBarrier = [&](VkImage targetImage, VkImageAspectFlags aspect, VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkAccessFlags sourceAccess, VkAccessFlags destAccess) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = oldLayout; barrier.newLayout = newLayout; barrier.srcAccessMask = sourceAccess; barrier.dstAccessMask = destAccess;
            barrier.image = targetImage; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {aspect, 0, 1, 0, 1};
            DEVICE(vkCmdPipelineBarrier)(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        };
        const auto drawDepth = [&] {
            VkClearValue clear{}; clear.depthStencil.depth = 1;
            VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            render.renderPass = renderPass; render.framebuffer = framebuffer; render.renderArea = {{0, 0}, {width, height}};
            render.clearValueCount = 1; render.pClearValues = &clear;
            DEVICE(vkCmdBeginRenderPass)(command, &render, VK_SUBPASS_CONTENTS_INLINE);
            DEVICE(vkCmdBindPipeline)(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            DEVICE(vkCmdDraw)(command, 3, 1, 0, 0);
            DEVICE(vkCmdEndRenderPass)(command);
        };
        asteria_depth_capture::Source candidate{};
        candidate.image = source; candidate.format = image.format; candidate.samples = image.samples;
        candidate.usage = image.usage; candidate.width = width; candidate.height = height;
        candidate.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        candidate.scenePassConfirmed = true; candidate.sceneDrawCalls = 1; candidate.insideRenderPass = false;
        const auto saveOutput = [&](uint32_t index) {
            const auto &output = capture.Current();
            Check(output.recorded && !output.gpuCompleted && output.frameID == candidate.frameID, "recorded output keeps correct frame identity and completion boundary");
            imageBarrier(output.image, VK_IMAGE_ASPECT_COLOR_BIT, output.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{}; copy.bufferOffset = index * frameBytes;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {width, height, 1};
            DEVICE(vkCmdCopyImageToBuffer)(command, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
            imageBarrier(output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.layout,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        };
        drawDepth();
        candidate.frameID = 101;
        Check(capture.RecordSnapshotBeforeClear(command, candidate), "copy actual multisample depth before clear without changing pipeline bindings");
        Check(!capture.Current().recorded, "snapshot copy alone does not advertise a resolved frame");
        imageBarrier(source, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                     candidate.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearDepthStencilValue cleared{0.9375f, 0};
        VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        DEVICE(vkCmdClearDepthStencilImage)(command, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cleared, 1, &clearRange);
        imageBarrier(source, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, candidate.layout, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        Check(capture.RecordResolve(command), "resolve private snapshot after original depth has already been cleared");
        saveOutput(0);
        candidate.frameID = 102;
        Check(capture.RecordBeforeClear(command, candidate), "capture subsequent actual depth clear for control");
        saveOutput(1);
        drawDepth();
        candidate.frameID = 103; candidate.reversedZ = true;
        Check(capture.RecordBeforeClear(command, candidate), "capture actual four samples using reversed-Z closest policy");
        saveOutput(2);
        auto invalid = candidate; invalid.insideRenderPass = true;
        Check(!capture.RecordBeforeClear(command, invalid), "active render pass source fails closed");
        invalid = candidate; invalid.scenePassConfirmed = false;
        Check(!capture.RecordBeforeClear(command, invalid), "unconfirmed scene candidate fails closed");
        invalid = candidate; invalid.usage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Check(!capture.RecordBeforeClear(command, invalid), "noncopyable depth source fails closed");
        VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostBarrier.srcQueueFamilyIndex = hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.buffer = readback; hostBarrier.size = frameBytes * 3;
        DEVICE(vkCmdPipelineBarrier)(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostBarrier, 0, nullptr);
        Success(DEVICE(vkEndCommandBuffer)(command), "finish real GPU commands");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Success(DEVICE(vkCreateFence)(device, &fenceInfo, nullptr, &fence), "create completion fence");
        VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submission.commandBufferCount = 1; submission.pCommandBuffers = &command;
        Success(DEVICE(vkQueueSubmit)(queue, 1, &submission, fence), "submit actual depth rendering capture and conversion");
        Success(DEVICE(vkWaitForFences)(device, 1, &fence, VK_TRUE, 10000000000ull), "wait actual GPU completion");
        Success(capture.WaitAndMarkComplete(), "confirm capture resource GPU completion");
        Check(capture.Current().gpuCompleted, "completion is reported only after actual GPU wait");
        void *mapped = nullptr;
        Success(DEVICE(vkMapMemory)(device, readbackMemory, 0, frameBytes * 3, 0, &mapped), "read actual R32 GPU results");
        const auto values = static_cast<const float *>(mapped);
        double maxError = 0;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const auto offset = y * width + x;
                const double baseDepth = 0.125 + double(x % 8) * 0.0625 + double(y % 4) * 0.03125;
                const double expected[] = {baseDepth, 0.9375, baseDepth + 3.0 * 0.0078125};
                for (uint32_t index = 0; index < 3; ++index)
                {
                    const double value = values[index * width * height + offset];
                    const auto error = std::abs(value - expected[index]);
                    maxError = std::max(maxError, error);
                    Check(std::isfinite(value) && error <= 0.000001, "actual GPU D24MSAA to R32 depth matches per-pixel per-sample expectation");
                }
            }
        std::ofstream saved(results / L"readback_r32_float.bin", std::ios::binary);
        saved.write(static_cast<const char *>(mapped), frameBytes * 3);
        DEVICE(vkUnmapMemory)(device, readbackMemory);
        report["width"] = width; report["height"] = height; report["sample_count"] = 4;
        report["source_format"] = "VK_FORMAT_D24_UNORM_S8_UINT"; report["output_format"] = "VK_FORMAT_R32_SFLOAT";
        report["validated_gpu_pixels"] = width * height * 3;
        report["max_absolute_error"] = maxError;
        report["ordinary_z_min_samples"] = true; report["reversed_z_max_samples"] = true;
        report["copy_before_clear_preserved"] = true; report["subsequent_clear_control_verified"] = true;
        report["source_depth_written_by_gpu_fragment_shader"] = true; report["actual_gpu_depth_capture_validated"] = true;
        asteria_depth_capture::CandidateSelector selector;
        selector.BeginFrame(100, 3840, 2160);
        asteria_depth_capture::DrawObservation observation{};
        observation.image = 0xFEDCBA9876543210ull; observation.frameID = 100;
        observation.width = 3840; observation.height = 2160; observation.samples = 4;
        observation.drawCalls = 100; observation.vertices = 100000;
        observation.formatD24S8 = observation.copySourceAvailable = observation.primaryTargetDimensionsMatch = observation.applicationDrawOutsideEffects = true;
        Check(selector.Observe(observation), "primary-target application depth draw is a candidate");
        Check(selector.Select().image == observation.image && !selector.Select().scenePassConfirmed && selector.Select().candidateOnly,
              "candidate score never claims main-view camera or scene confirmation");
        auto rejected = observation; rejected.width = 960;
        Check(!selector.Observe(rejected), "quarter-size auxiliary depth is excluded");
        rejected = observation; rejected.applicationDrawOutsideEffects = false;
        Check(!selector.Observe(rejected), "ReShade effect depth draw is excluded");
        rejected = observation; rejected.frameID = 99;
        Check(!selector.Observe(rejected), "stale frame depth is excluded");
        rejected = observation; rejected.copySourceAvailable = false;
        Check(!selector.Observe(rejected), "noncopyable observed depth is excluded");
        auto competitor = observation; competitor.image = 0x12345678ABCDEF01ull; competitor.drawCalls = 97;
        Check(selector.Observe(competitor) && selector.Select().ambiguous, "similar main-size candidates remain explicitly ambiguous");
        selector.BeginFrame(101, 3840, 2160);
        Check(selector.Select().image == 0, "new frame discards earlier candidate identities");
        report["candidate_selection_contract"] = {{"cpu_metadata_only", true}, {"never_confirms_scene_from_score", true},
                                                  {"filters_stale_auxiliary_effect_noncopyable", true}, {"ambiguous_candidates_reported", true}};
        report["resolve_after_original_clear_verified"] = true;
        report["passed"] = true;
    }
    catch (const std::exception &failure) { report["error"] = failure.what(); }
    if (device)
    {
        DEVICE(vkDeviceWaitIdle)(device);
        capture.ReleaseAfterGpuIdle();
        if (fence) DEVICE(vkDestroyFence)(device, fence, nullptr);
        if (commandPool) DEVICE(vkDestroyCommandPool)(device, commandPool, nullptr);
        if (pipeline) DEVICE(vkDestroyPipeline)(device, pipeline, nullptr);
        if (pipelineLayout) DEVICE(vkDestroyPipelineLayout)(device, pipelineLayout, nullptr);
        if (vertex) DEVICE(vkDestroyShaderModule)(device, vertex, nullptr);
        if (fragment) DEVICE(vkDestroyShaderModule)(device, fragment, nullptr);
        if (framebuffer) DEVICE(vkDestroyFramebuffer)(device, framebuffer, nullptr);
        if (renderPass) DEVICE(vkDestroyRenderPass)(device, renderPass, nullptr);
        if (sourceView) DEVICE(vkDestroyImageView)(device, sourceView, nullptr);
        if (source) DEVICE(vkDestroyImage)(device, source, nullptr);
        if (sourceMemory) DEVICE(vkFreeMemory)(device, sourceMemory, nullptr);
        if (readback) DEVICE(vkDestroyBuffer)(device, readback, nullptr);
        if (readbackMemory) DEVICE(vkFreeMemory)(device, readbackMemory, nullptr);
        DEVICE(vkDestroyDevice)(device, nullptr);
    }
    if (instance) INSTANCE(vkDestroyInstance)(instance, nullptr);
    if (vulkan) FreeLibrary(vulkan);
    report["checks"] = Checks;
    std::ofstream(results / L"gpu_depth_capture.json", std::ios::binary) << report.dump(2);
    std::cout << report.dump(2) << std::endl;
    return report["passed"].get<bool>() ? 0 : 1;
}
