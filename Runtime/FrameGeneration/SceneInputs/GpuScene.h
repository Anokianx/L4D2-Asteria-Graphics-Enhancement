#pragma once
#include "../DepthCapture/DepthCapture.h"

class GpuScene
{
    struct Functions
    {
#define ASTERIA_SCENE_FUNCTION(name) PFN_##name name = nullptr;
        ASTERIA_SCENE_FUNCTION(vkCreateImage)
        ASTERIA_SCENE_FUNCTION(vkDestroyImage)
        ASTERIA_SCENE_FUNCTION(vkGetImageMemoryRequirements)
        ASTERIA_SCENE_FUNCTION(vkAllocateMemory)
        ASTERIA_SCENE_FUNCTION(vkFreeMemory)
        ASTERIA_SCENE_FUNCTION(vkBindImageMemory)
        ASTERIA_SCENE_FUNCTION(vkCreateImageView)
        ASTERIA_SCENE_FUNCTION(vkDestroyImageView)
        ASTERIA_SCENE_FUNCTION(vkCreateRenderPass)
        ASTERIA_SCENE_FUNCTION(vkDestroyRenderPass)
        ASTERIA_SCENE_FUNCTION(vkCreateFramebuffer)
        ASTERIA_SCENE_FUNCTION(vkDestroyFramebuffer)
        ASTERIA_SCENE_FUNCTION(vkCreatePipelineLayout)
        ASTERIA_SCENE_FUNCTION(vkDestroyPipelineLayout)
        ASTERIA_SCENE_FUNCTION(vkCreateShaderModule)
        ASTERIA_SCENE_FUNCTION(vkDestroyShaderModule)
        ASTERIA_SCENE_FUNCTION(vkCreateGraphicsPipelines)
        ASTERIA_SCENE_FUNCTION(vkDestroyPipeline)
        ASTERIA_SCENE_FUNCTION(vkCmdBeginRenderPass)
        ASTERIA_SCENE_FUNCTION(vkCmdEndRenderPass)
        ASTERIA_SCENE_FUNCTION(vkCmdBindPipeline)
        ASTERIA_SCENE_FUNCTION(vkCmdPushConstants)
        ASTERIA_SCENE_FUNCTION(vkCmdDraw)
        ASTERIA_SCENE_FUNCTION(vkCmdResolveImage)
        ASTERIA_SCENE_FUNCTION(vkCmdPipelineBarrier)
        ASTERIA_SCENE_FUNCTION(vkCmdClearDepthStencilImage)
        ASTERIA_SCENE_FUNCTION(vkCmdCopyImage)
        ASTERIA_SCENE_FUNCTION(vkDeviceWaitIdle)
#undef ASTERIA_SCENE_FUNCTION

        void Load(VkDevice device)
        {
#define ASTERIA_SCENE_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); Require(name != nullptr, #name);
            ASTERIA_SCENE_LOAD(vkCreateImage)
            ASTERIA_SCENE_LOAD(vkDestroyImage)
            ASTERIA_SCENE_LOAD(vkGetImageMemoryRequirements)
            ASTERIA_SCENE_LOAD(vkAllocateMemory)
            ASTERIA_SCENE_LOAD(vkFreeMemory)
            ASTERIA_SCENE_LOAD(vkBindImageMemory)
            ASTERIA_SCENE_LOAD(vkCreateImageView)
            ASTERIA_SCENE_LOAD(vkDestroyImageView)
            ASTERIA_SCENE_LOAD(vkCreateRenderPass)
            ASTERIA_SCENE_LOAD(vkDestroyRenderPass)
            ASTERIA_SCENE_LOAD(vkCreateFramebuffer)
            ASTERIA_SCENE_LOAD(vkDestroyFramebuffer)
            ASTERIA_SCENE_LOAD(vkCreatePipelineLayout)
            ASTERIA_SCENE_LOAD(vkDestroyPipelineLayout)
            ASTERIA_SCENE_LOAD(vkCreateShaderModule)
            ASTERIA_SCENE_LOAD(vkDestroyShaderModule)
            ASTERIA_SCENE_LOAD(vkCreateGraphicsPipelines)
            ASTERIA_SCENE_LOAD(vkDestroyPipeline)
            ASTERIA_SCENE_LOAD(vkCmdBeginRenderPass)
            ASTERIA_SCENE_LOAD(vkCmdEndRenderPass)
            ASTERIA_SCENE_LOAD(vkCmdBindPipeline)
            ASTERIA_SCENE_LOAD(vkCmdPushConstants)
            ASTERIA_SCENE_LOAD(vkCmdDraw)
            ASTERIA_SCENE_LOAD(vkCmdResolveImage)
            ASTERIA_SCENE_LOAD(vkCmdPipelineBarrier)
            ASTERIA_SCENE_LOAD(vkCmdClearDepthStencilImage)
            ASTERIA_SCENE_LOAD(vkCmdCopyImage)
            ASTERIA_SCENE_LOAD(vkDeviceWaitIdle)
#undef ASTERIA_SCENE_LOAD
        }
    } api;

    struct Attachment
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    } color, depth;

    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    asteria_depth_capture::Capture capture;
    uint64_t frameID = 0;

    void Allocate(Gpu &gpu, Attachment &attachment, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
    {
        const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
            vkGetInstanceProcAddr(gpu.instance, "vkGetPhysicalDeviceImageFormatProperties"));
        Require(query != nullptr, "Scene image format query entry missing");
        VkImageFormatProperties supported{};
        Check(query(gpu.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported),
              "Scene four-sample attachment format support");
        Require((supported.sampleCounts & VK_SAMPLE_COUNT_4_BIT) != 0, "Scene requires four-sample attachment support");
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D; image.format = format;
        image.extent = {Width, Height, 1}; image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_4_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = usage; image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(api.vkCreateImage(device, &image, nullptr, &attachment.image), "Create GPU scene attachment");
        VkMemoryRequirements requirements{};
        api.vkGetImageMemoryRequirements(device, attachment.image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = gpu.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(api.vkAllocateMemory(device, &allocation, nullptr, &attachment.memory), "Allocate GPU scene attachment");
        Check(api.vkBindImageMemory(device, attachment.image, attachment.memory, 0), "Bind GPU scene attachment");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = attachment.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format;
        view.subresourceRange = {aspect, 0, 1, 0, 1};
        Check(api.vkCreateImageView(device, &view, nullptr, &attachment.view), "Create GPU scene attachment view");
    }

    void Barrier(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect,
                 VkImageLayout before, VkImageLayout after, VkAccessFlags source, VkAccessFlags destination)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = before; barrier.newLayout = after;
        barrier.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : source;
        barrier.dstAccessMask = destination;
        barrier.image = image; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        api.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    VkShaderModule Shader(const std::filesystem::path &path)
    {
        const auto words = LoadCode(path);
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize = words.size() * sizeof(uint32_t); shader.pCode = words.data();
        VkShaderModule result = VK_NULL_HANDLE;
        Check(api.vkCreateShaderModule(device, &shader, nullptr, &result), "Create GPU scene shader");
        return result;
    }

    void Release()
    {
        if (!device) return;
        api.vkDeviceWaitIdle(device);
        capture.ReleaseAfterGpuIdle();
        if (pipeline) api.vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) api.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (vertex) api.vkDestroyShaderModule(device, vertex, nullptr);
        if (fragment) api.vkDestroyShaderModule(device, fragment, nullptr);
        if (framebuffer) api.vkDestroyFramebuffer(device, framebuffer, nullptr);
        if (renderPass) api.vkDestroyRenderPass(device, renderPass, nullptr);
        for (auto attachment : {&color, &depth})
        {
            if (attachment->view) api.vkDestroyImageView(device, attachment->view, nullptr);
            if (attachment->image) api.vkDestroyImage(device, attachment->image, nullptr);
            if (attachment->memory) api.vkFreeMemory(device, attachment->memory, nullptr);
            *attachment = {};
        }
        device = VK_NULL_HANDLE;
    }

public:
    GpuScene(Gpu &gpu, const std::filesystem::path &sceneRoot, const std::filesystem::path &resolveShader)
    {
        api.Load(gpu.device);
        device = gpu.device;
        try
        {
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            vkGetPhysicalDeviceFeatures2(gpu.physical, &features);
            Require(features.features.sampleRateShading != VK_FALSE, "GPU scene requires enabled sampleRateShading");
            Allocate(gpu, color, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            Allocate(gpu, depth, VK_FORMAT_D24_UNORM_S8_UINT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
            VkAttachmentDescription attachments[2]{};
            for (auto &attachment : attachments)
            {
                attachment.samples = VK_SAMPLE_COUNT_4_BIT;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
            attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
            attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            attachments[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &colorReference; subpass.pDepthStencilAttachment = &depthReference;
            VkSubpassDependency dependencies[2]{};
            dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass = 0;
            dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dependencies[1].srcSubpass = 0; dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            dependencies[1].srcStageMask = dependencies[0].dstStageMask;
            dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dependencies[1].srcAccessMask = dependencies[0].dstAccessMask;
            dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            pass.attachmentCount = 2; pass.pAttachments = attachments; pass.subpassCount = 1; pass.pSubpasses = &subpass;
            pass.dependencyCount = 2; pass.pDependencies = dependencies;
            Check(api.vkCreateRenderPass(device, &pass, nullptr, &renderPass), "Create GPU scene render pass");
            const VkImageView views[] = {color.view, depth.view};
            VkFramebufferCreateInfo target{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            target.renderPass = renderPass; target.attachmentCount = 2; target.pAttachments = views;
            target.width = Width; target.height = Height; target.layers = 1;
            Check(api.vkCreateFramebuffer(device, &target, nullptr, &framebuffer), "Create GPU scene framebuffer");
            vertex = Shader(sceneRoot / "bin/Scene.vert.spv");
            fragment = Shader(sceneRoot / "bin/Scene.frag.spv");
            VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
            Check(api.vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout), "Create GPU scene pipeline layout");
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (auto &stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment;
            VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport viewport{0, 0, float(Width), float(Height), 0, 1};
            VkRect2D scissor{{0, 0}, {Width, Height}};
            VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewportState.viewportCount = viewportState.scissorCount = 1;
            viewportState.pViewports = &viewport; viewportState.pScissors = &scissor;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT; samples.sampleShadingEnable = VK_TRUE; samples.minSampleShading = 1;
            VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depthState.depthTestEnable = depthState.depthWriteEnable = VK_TRUE; depthState.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            VkPipelineColorBlendAttachmentState colorState{};
            colorState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1; blend.pAttachments = &colorState;
            VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            graphics.stageCount = 2; graphics.pStages = stages; graphics.pVertexInputState = &vertices;
            graphics.pInputAssemblyState = &assembly; graphics.pViewportState = &viewportState; graphics.pRasterizationState = &raster;
            graphics.pMultisampleState = &samples; graphics.pDepthStencilState = &depthState; graphics.pColorBlendState = &blend;
            graphics.layout = pipelineLayout; graphics.renderPass = renderPass;
            Check(api.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphics, nullptr, &pipeline), "Create GPU scene graphics pipeline");
            Require(capture.Initialize(device, vkGetDeviceProcAddr, gpu.memoryProperties, Width, Height, LoadCode(resolveShader)),
                    "Initialize GPU scene depth snapshot and resolve");
        }
        catch (...)
        {
            Release();
            throw;
        }
    }

    GpuScene(const GpuScene &) = delete;
    GpuScene &operator=(const GpuScene &) = delete;
    ~GpuScene() { Release(); }

    void Record(Gpu &gpu, Image &targetColor, Image &targetDepth, int shiftX, int shiftY, uint64_t targetFrameID)
    {
        Require(gpu.device == device && gpu.command != VK_NULL_HANDLE, "GPU scene belongs to a different device");
        Require(targetColor.handle != targetDepth.handle && targetColor.handle && targetDepth.handle,
                "GPU scene needs distinct external color and depth images");
        Require(targetColor.description.width == Width && targetColor.description.height == Height && targetColor.description.mipCount == 1 &&
                targetColor.description.format == FFX_SURFACE_FORMAT_R8G8B8A8_UNORM, "GPU scene color output format or extent mismatch");
        Require(targetDepth.description.width == Width && targetDepth.description.height == Height && targetDepth.description.mipCount == 1 &&
                targetDepth.description.format == FFX_SURFACE_FORMAT_R32_FLOAT, "GPU scene depth output format or extent mismatch");
        VkClearValue clear[2]{}; clear[1].depthStencil.depth = 1;
        VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        render.renderPass = renderPass; render.framebuffer = framebuffer; render.renderArea = {{0, 0}, {Width, Height}};
        render.clearValueCount = 2; render.pClearValues = clear;
        api.vkCmdBeginRenderPass(gpu.command, &render, VK_SUBPASS_CONTENTS_INLINE);
        api.vkCmdBindPipeline(gpu.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const int32_t parameters[] = {shiftX, shiftY};
        api.vkCmdPushConstants(gpu.command, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(parameters), parameters);
        api.vkCmdDraw(gpu.command, 3, 1, 0, 0);
        api.vkCmdEndRenderPass(gpu.command);
        gpu.Barrier(targetColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve colorCopy{};
        colorCopy.srcSubresource = colorCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        colorCopy.extent = {Width, Height, 1};
        api.vkCmdResolveImage(gpu.command, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              targetColor.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &colorCopy);
        gpu.Barrier(targetColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        asteria_depth_capture::Source source;
        source.image = depth.image; source.format = VK_FORMAT_D24_UNORM_S8_UINT;
        source.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; source.samples = VK_SAMPLE_COUNT_4_BIT;
        source.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        source.width = Width; source.height = Height; source.frameID = targetFrameID;
        source.scenePassConfirmed = true; source.sceneDrawCalls = 1; source.insideRenderPass = false; source.reversedZ = false;
        Require(capture.RecordSnapshotBeforeClear(gpu.command, source), "Snapshot GPU scene depth before source clear");
        const auto aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        Barrier(gpu.command, depth.image, aspect, source.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearDepthStencilValue cleared{0.9375f, 0};
        VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
        api.vkCmdClearDepthStencilImage(gpu.command, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cleared, 1, &range);
        Barrier(gpu.command, depth.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, source.layout,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        Require(capture.RecordResolve(gpu.command), "Resolve GPU scene snapshot after source clear");
        const auto &output = capture.Current();
        Require(output.recorded && !output.gpuCompleted && output.frameID == targetFrameID && !output.reversedZ,
                "GPU scene depth capture identity mismatch");
        Barrier(gpu.command, output.image, VK_IMAGE_ASPECT_COLOR_BIT, output.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        gpu.Barrier(targetDepth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy depthCopy{};
        depthCopy.srcSubresource = depthCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        depthCopy.extent = {Width, Height, 1};
        api.vkCmdCopyImage(gpu.command, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           targetDepth.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopy);
        Barrier(gpu.command, output.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.layout,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        gpu.Barrier(targetDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        frameID = targetFrameID;
    }

    uint64_t FrameID() const { return frameID; }
    const asteria_depth_capture::Output &CaptureOutput() const { return capture.Current(); }
};
