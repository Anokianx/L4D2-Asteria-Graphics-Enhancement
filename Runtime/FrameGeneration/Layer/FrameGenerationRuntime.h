#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

#include <FidelityFX/host/ffx_assert.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include "../Port/include/AsteriaVulkanShim.h"
#include "FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.h"
#include "FrameInterpolationSwapchain/FrameInterpolationSwapchainVK_UiComposition.h"
#include "AsteriaVkHandles.h"
#include "FrameGenerationInputs.h"
#include "FrameInputBridge.h"
#include "LiveSceneMotionRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#define ASTERIA_FG_CAMERA_API_VERSION_1 1u

struct AsteriaFrameGenerationCameraV1
{
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t valid;
    uint32_t in_game;
    uint32_t reset;
    float camera_near;
    float camera_far;
    float camera_fov_vertical_radians;
    float camera_position[3];
    float camera_up[3];
    float camera_right[3];
    float camera_forward[3];
};

namespace asteria_fg_runtime
{

struct GpuImage
{
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    FfxResourceDescription description{};
};

struct FgCameraState
{
    std::mutex mutex;
    bool valid = false;
    bool inGame = false;
    bool reset = false;
    float nearPlane = 7.0f;
    float farPlane = 5000.0f;
    float fovVerticalRadians = 1.3089969f;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 0.0f, 1.0f};
    float right[3] = {1.0f, 0.0f, 0.0f};
    float forward[3] = {0.0f, 1.0f, 0.0f};
    uint64_t sampleCount = 0;
};

struct FgDeviceQueues
{
    uint32_t graphicsFamily = UINT32_MAX;
    VkQueue gameQueue = VK_NULL_HANDLE;
    VkQueue asyncComputeQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkQueue imageAcquireQueue = VK_NULL_HANDLE;
    bool available = false;
};

struct FgSwapchainState
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr nextGdpa = nullptr;
    VkSwapchainKHR proxySwapchain = VK_NULL_HANDLE;
    VkSwapchainKHR nativeSwapchain = VK_NULL_HANDLE;
    bool nativeCreateAttempted = false;
    bool retiredForAcquisition = false;
    FfxSwapchain swapchainBox = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t frameID = 0;
    std::vector<uint8_t> scratch;
    FfxInterface backend{};
    std::unique_ptr<FfxFrameInterpolationContext> interpolation;
    std::unique_ptr<FfxOpticalflowContext> opticalFlow;
    bool interpolationCreated = false;
    bool opticalFlowCreated = false;
    GpuImage reconstructed{};
    GpuImage dilatedDepth{};
    GpuImage dilatedMotion{};
    GpuImage vectors{};
    GpuImage estimatedMotion{};
    GpuImage scd{};
    GpuImage internalDepth{};
    bool estimatedInternalInputs = false;
    FgCameraState *cameraState = nullptr;
    VkDeviceContext deviceContext{};
    std::shared_ptr<FrameInputSession> inputSession;
    std::shared_ptr<const FrameInputs> currentInputs;
    std::atomic<bool> historyReset{true};
    std::atomic<uint64_t> interpolationDispatches{0};
};

inline bool IsFrameGenerationRequestedByEnv()
{
    wchar_t value[64]{};
    const DWORD length = GetEnvironmentVariableW(L"ASTERIA_FG_ENABLE", value, 64);
    if (!length || length >= 64) return false;
    return std::wcscmp(value, L"1") == 0 || _wcsicmp(value, L"true") == 0;
}
inline bool IsEstimatedInternalInputsRequested()
{
    wchar_t value[64]{};
    const DWORD length = GetEnvironmentVariableW(L"ASTERIA_FG_ESTIMATED_INTERNAL", value, 64);
    return length && length < 64 && (std::wcscmp(value, L"1") == 0 || _wcsicmp(value, L"true") == 0);
}


inline VkFormat ToVkFormat(FfxSurfaceFormat format)
{
    switch (format)
    {
    case FFX_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case FFX_SURFACE_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

inline FfxResourceDescription MakeImageDescription(FfxSurfaceFormat format, uint32_t width, uint32_t height)
{
    FfxResourceDescription desc{};
    desc.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.depth = 1;
    desc.mipCount = 1;
    desc.usage = static_cast<FfxResourceUsage>(FFX_RESOURCE_USAGE_READ_ONLY | FFX_RESOURCE_USAGE_UAV);
    return desc;
}

inline bool AllocateGpuImage(
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    const FfxResourceDescription &desc,
    GpuImage &outImage)
{
    outImage.description = desc;
    outImage.description.mipCount = 1;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = ToVkFormat(desc.format);
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(device, &info, nullptr, &outImage.handle) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, outImage.handle, &req);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    uint32_t memIndex = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
    {
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            memIndex = i;
            break;
        }
    }
    if (memIndex == UINT32_MAX)
    {
        vkDestroyImage(device, outImage.handle, nullptr);
        outImage.handle = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memIndex;
    if (vkAllocateMemory(device, &alloc, nullptr, &outImage.memory) != VK_SUCCESS)
    {
        vkDestroyImage(device, outImage.handle, nullptr);
        outImage.handle = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(device, outImage.handle, outImage.memory, 0) != VK_SUCCESS)
    {
        vkFreeMemory(device, outImage.memory, nullptr);
        vkDestroyImage(device, outImage.handle, nullptr);
        outImage.memory = VK_NULL_HANDLE;
        outImage.handle = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

inline void DestroyGpuImage(VkDevice device, GpuImage &img)
{
    if (img.handle != VK_NULL_HANDLE)
    {
        asteria_vk::Retire(asteria_vk::Image(device, img.handle));
        vkDestroyImage(device, img.handle, nullptr);
        img.handle = VK_NULL_HANDLE;
    }
    if (img.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, img.memory, nullptr);
        img.memory = VK_NULL_HANDLE;
    }
}

inline FfxResource WrapGpuImage(
    VkDevice device,
    const GpuImage &img,
    const wchar_t *name,
    FfxResourceStates state = FFX_RESOURCE_STATE_COMPUTE_READ)
{
    return ffxGetResourceVK(asteria_vk::Image(device, img.handle), img.description, const_cast<wchar_t *>(name), state);
}

struct InternalInputLease
{
    void *depthBox = nullptr;
    void *motionBox = nullptr;
    ~InternalInputLease()
    {
        if (depthBox) asteria_vk::Retire(depthBox);
        if (motionBox) asteria_vk::Retire(motionBox);
    }
};

inline std::shared_ptr<const FrameInputs> MakeEstimatedInternalInputs(FgSwapchainState &state, uint64_t frame)
{
    auto lease = std::make_shared<InternalInputLease>();
    lease->depthBox = asteria_vk::Image(state.device, state.internalDepth.handle);
    lease->motionBox = asteria_vk::Image(state.device, state.estimatedMotion.handle);
    if (!lease->depthBox || !lease->motionBox) return nullptr;
    asteria_live_scene::GlobalRuntime().EnsureInitialized(
        state.device, state.physicalDevice, state.nextGdpa,
        state.width, state.height, state.internalDepth.handle, state.estimatedMotion.handle);
    const auto committed = asteria_live_scene::GlobalRuntime().CommitFrameOnPresent(
        state.device, state.width, state.height, frame);
    auto input = std::make_shared<FrameInputs>();
    input->device = reinterpret_cast<uintptr_t>(state.device); input->frameID = frame;
    input->width = state.width; input->height = state.height; input->deltaMilliseconds = 16.6667f;
    if (committed.valid)
    {
        input->nearPlane = committed.nearPlane;
        input->farPlane = committed.farPlane;
        input->verticalFovRadians = committed.verticalFovRadians;
        input->metersPerUnit = 0.0254f;
        std::memcpy(input->position, committed.position, sizeof(input->position));
        std::memcpy(input->up, committed.up, sizeof(input->up));
        std::memcpy(input->right, committed.right, sizeof(input->right));
        std::memcpy(input->forward, committed.forward, sizeof(input->forward));
        input->historyReset = (frame == 0 || !committed.hasPreviousCamera);
    }
    else
    {
        input->nearPlane = 0.1f; input->farPlane = 5000.0f; input->verticalFovRadians = 1.0f; input->metersPerUnit = 1.0f;
        input->up[1] = 1.0f; input->right[0] = 1.0f; input->forward[2] = 1.0f;
        input->historyReset = frame == 0;
    }
    input->motionScale = {float(state.width), float(state.height)};
    input->motionSource = MotionInputSource::Estimated; input->sceneDepth = true; input->synchronized = true;
    input->hudCompositionVerified = true; input->lifetime = lease;
    input->depth.resource = lease->depthBox; input->depth.description = state.internalDepth.description; input->depth.state = FFX_RESOURCE_STATE_COMPUTE_READ;
    input->motion.resource = lease->motionBox; input->motion.description = state.estimatedMotion.description; input->motion.state = FFX_RESOURCE_STATE_COMPUTE_READ;
    return input;
}

inline void RecordImageBarrier(
    VkCommandBuffer cmd,
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier);
}

inline FfxErrorCode LayerFrameGenerationCallback(const FfxFrameGenerationDispatchDescription *desc, void *userCtx)
{
    if (!desc || !userCtx) return FFX_ERROR_INVALID_ARGUMENT;
    auto &state = *static_cast<FgSwapchainState *>(userCtx);
    VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(desc->commandList);
    if (!cmd) return FFX_ERROR_INVALID_ARGUMENT;

    const auto input = std::atomic_load_explicit(&state.currentInputs, std::memory_order_acquire);
    if (!input || !ValidFrameInputs(*input, reinterpret_cast<uintptr_t>(state.device), desc->frameID, state.width, state.height))
    {
        state.historyReset.store(true);
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    struct ResetOnFailure
    {
        FgSwapchainState &state;
        bool successful = false;
        ~ResetOnFailure() { if (!successful) state.historyReset.store(true); }
    } recovery{state};
    const uint64_t currentFrameId = input->frameID;
    const bool pendingReset = state.historyReset.exchange(false);
    const bool reset = desc->reset || input->historyReset || pendingReset;
    if (asteria_vk::Decode<VkImage>(input->depth.resource, state.device, asteria_vk::HandleKind::Image) == VK_NULL_HANDLE ||
        asteria_vk::Decode<VkImage>(input->motion.resource, state.device, asteria_vk::HandleKind::Image) == VK_NULL_HANDLE)
        return FFX_ERROR_INVALID_ARGUMENT;
    if (currentFrameId == 0 || reset)
        for (GpuImage *image : {&state.reconstructed, &state.dilatedDepth, &state.dilatedMotion, &state.vectors, &state.scd})
            RecordImageBarrier(cmd, image->handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    if (state.estimatedInternalInputs && state.internalDepth.handle && state.estimatedMotion.handle)
    {
        if (!asteria_live_scene::GlobalRuntime().RecordResolveAndMotion(
                state.device, cmd, state.internalDepth.handle, state.estimatedMotion.handle, reset))
        {
            RecordImageBarrier(cmd, state.internalDepth.handle, currentFrameId == 0 || reset ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            VkClearColorValue clear{}; clear.float32[0] = 1.0f;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, state.internalDepth.handle, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
            RecordImageBarrier(cmd, state.internalDepth.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            RecordImageBarrier(cmd, state.estimatedMotion.handle, currentFrameId == 0 || reset ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            VkClearColorValue motionClear{};
            VkImageSubresourceRange motionRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, state.estimatedMotion.handle, VK_IMAGE_LAYOUT_GENERAL, &motionClear, 1, &motionRange);
            RecordImageBarrier(cmd, state.estimatedMotion.handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    FfxOpticalflowDispatchDescription flow{};
    flow.commandList = desc->commandList;
    flow.color = desc->presentColor;
    flow.opticalFlowVector = WrapGpuImage(state.device, state.vectors, L"AsteriaFG_OpticalFlowVector", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    flow.opticalFlowSCD = WrapGpuImage(state.device, state.scd, L"AsteriaFG_OpticalFlowSCD", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    flow.reset = reset;
    flow.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    flow.minMaxLuminance = {0.0f, 1.0f};
    if (asteria_live_scene::EnvFlagDefaultTrue("ASTERIA_FG_OPTICAL_FLOW"))
    {
        const FfxErrorCode flowErr = ffxOpticalflowContextDispatch(state.opticalFlow.get(), &flow);
        if (flowErr != FFX_OK) return flowErr;
    }

    FfxFrameInterpolationPrepareDescription prepare{};
    prepare.commandList = desc->commandList;
    prepare.renderSize = {state.width, state.height};
    prepare.motionVectorScale = input->motionScale;
    prepare.frameTimeDelta = input->deltaMilliseconds;
    prepare.cameraNear = input->nearPlane;
    prepare.cameraFar = input->farPlane;
    prepare.cameraFovAngleVertical = input->verticalFovRadians;
    prepare.viewSpaceToMetersFactor = input->metersPerUnit;
    prepare.depth = input->depth;
    prepare.motionVectors = input->motion;
    prepare.frameID = currentFrameId;
    prepare.dilatedDepth = WrapGpuImage(state.device, state.dilatedDepth, L"AsteriaFG_DilatedDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    prepare.dilatedMotionVectors = WrapGpuImage(state.device, state.dilatedMotion, L"AsteriaFG_DilatedMotion", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    prepare.reconstructedPrevDepth = WrapGpuImage(state.device, state.reconstructed, L"AsteriaFG_ReconstructedDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    std::memcpy(prepare.cameraPosition, input->position, sizeof(prepare.cameraPosition));
    std::memcpy(prepare.cameraUp, input->up, sizeof(prepare.cameraUp));
    std::memcpy(prepare.cameraRight, input->right, sizeof(prepare.cameraRight));
    std::memcpy(prepare.cameraForward, input->forward, sizeof(prepare.cameraForward));
    const FfxErrorCode prepErr = ffxFrameInterpolationPrepare(state.interpolation.get(), &prepare);
    if (prepErr != FFX_OK) return prepErr;

    FfxFrameInterpolationDispatchDescription dispatch{};
    dispatch.commandList = desc->commandList;
    dispatch.displaySize = {state.width, state.height};
    dispatch.renderSize = {state.width, state.height};
    dispatch.currentBackBuffer = desc->presentColor;
    dispatch.output = desc->outputs[0];
    dispatch.interpolationRect = {0, 0, int32_t(state.width), int32_t(state.height)};
    dispatch.opticalFlowVector = flow.opticalFlowVector;
    dispatch.opticalFlowSceneChangeDetection = flow.opticalFlowSCD;
    dispatch.opticalFlowBufferSize = {state.vectors.description.width, state.vectors.description.height};
    dispatch.opticalFlowScale = {1.0f / float(state.width), 1.0f / float(state.height)};
    dispatch.opticalFlowBlockSize = 8;
    dispatch.cameraNear = prepare.cameraNear;
    dispatch.cameraFar = prepare.cameraFar;
    dispatch.cameraFovAngleVertical = prepare.cameraFovAngleVertical;
    dispatch.viewSpaceToMetersFactor = prepare.viewSpaceToMetersFactor;
    dispatch.frameTimeDelta = prepare.frameTimeDelta;
    dispatch.reset = reset;
    dispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    dispatch.minMaxLuminance[0] = 0.0f;
    dispatch.minMaxLuminance[1] = 1.0f;
    dispatch.frameID = currentFrameId;
    dispatch.dilatedDepth = prepare.dilatedDepth;
    dispatch.dilatedMotionVectors = prepare.dilatedMotionVectors;
    dispatch.reconstructedPrevDepth = prepare.reconstructedPrevDepth;
    const auto result = ffxFrameInterpolationDispatch(state.interpolation.get(), &dispatch);
    recovery.successful = result == FFX_OK;
    if (recovery.successful) ++state.interpolationDispatches;
    return result;
}

inline void DestroySwapchainResources(FgSwapchainState &state)
{
    if (state.proxySwapchain != VK_NULL_HANDLE && state.swapchainBox != nullptr)
    {
        ffxWaitForPresents(state.swapchainBox);
        FfxFrameGenerationConfig config{};
        config.swapChain = state.swapchainBox;
        ffxSetFrameGenerationConfigToSwapchainVK(&config);
        ffxWaitForPresents(state.swapchainBox);
        if (state.device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(state.device);
            vkDestroySwapchainFFX(state.device, state.proxySwapchain, nullptr);
        }
        state.proxySwapchain = VK_NULL_HANDLE;
        state.swapchainBox = nullptr;
    }
    else if (state.device != VK_NULL_HANDLE && state.inputSession)
        vkDeviceWaitIdle(state.device);
    std::atomic_store_explicit(&state.currentInputs, std::shared_ptr<const FrameInputs>{}, std::memory_order_release);
    if (state.inputSession)
    {
        try { state.inputSession->RetireAfterGpuIdle(); } catch (...) {}
        state.inputSession.reset();
    }
    state.historyReset.store(true);
    if (state.interpolationCreated && state.interpolation)
    {
        ffxFrameInterpolationContextDestroy(state.interpolation.get());
        state.interpolationCreated = false;
    }
    if (state.opticalFlowCreated && state.opticalFlow)
    {
        ffxOpticalflowContextDestroy(state.opticalFlow.get());
        state.opticalFlowCreated = false;
    }
    state.interpolation.reset();
    state.opticalFlow.reset();
    if (state.device != VK_NULL_HANDLE)
    {
        DestroyGpuImage(state.device, state.reconstructed);
        DestroyGpuImage(state.device, state.dilatedDepth);
        DestroyGpuImage(state.device, state.dilatedMotion);
        DestroyGpuImage(state.device, state.vectors);
        DestroyGpuImage(state.device, state.estimatedMotion);
        DestroyGpuImage(state.device, state.scd);
        DestroyGpuImage(state.device, state.internalDepth);
    }
}

inline VkResult InitializeSwapchainResources(
    FgSwapchainState &state,
    VkDevice device,
    VkPhysicalDevice physicalDevice,
    PFN_vkGetDeviceProcAddr nextGdpa,
    const FgDeviceQueues &queues,
    const VkSwapchainCreateInfoKHR *createInfo,
    const VkAllocationCallbacks *allocator,
    FgCameraState *cameraState)
{
    auto provider = CurrentFrameInputProvider();
    if (!provider && IsEstimatedInternalInputsRequested()) provider = EstimatedProvider();
    if (!provider || !createInfo) return VK_ERROR_FEATURE_NOT_PRESENT;
    state.device = device;
    try { state.inputSession = provider->Open(reinterpret_cast<uintptr_t>(device), createInfo->imageExtent.width, createInfo->imageExtent.height); }
    catch (...) { return VK_ERROR_INITIALIZATION_FAILED; }
    if (!state.inputSession) return VK_ERROR_FEATURE_NOT_PRESENT;
    state.physicalDevice = physicalDevice;
    state.nextGdpa = nextGdpa;
    state.width = createInfo->imageExtent.width;
    state.height = createInfo->imageExtent.height;
    state.format = createInfo->imageFormat;
    state.cameraState = cameraState;
    state.frameID = 0;

    VkFrameInterpolationInfoFFX fgInfo{};
    fgInfo.device = device;
    fgInfo.physicalDevice = physicalDevice;
    fgInfo.gameQueue = {queues.gameQueue, queues.graphicsFamily, nullptr};
    fgInfo.asyncComputeQueue = {queues.asyncComputeQueue, queues.graphicsFamily, nullptr};
    fgInfo.presentQueue = {queues.presentQueue, queues.graphicsFamily, nullptr};
    fgInfo.imageAcquireQueue = {queues.imageAcquireQueue, queues.graphicsFamily, nullptr};
    fgInfo.compositionMode = VK_COMPOSITION_MODE_PRESENT_QUEUE_FFX;

    state.deviceContext = {device, physicalDevice, nextGdpa};
    FfxDevice ffxDevice = ffxGetDeviceVK(&state.deviceContext);
    FfxSwapchainReplacementFunctions replacement{};
    if (ffxGetSwapchainReplacementFunctionsVK(ffxDevice, &replacement) != FFX_OK || !replacement.createSwapchainFFX)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkSwapchainCreateInfoKHR ffxCreateInfo = *createInfo;
    if (physicalDevice != VK_NULL_HANDLE && ffxCreateInfo.surface != VK_NULL_HANDLE &&
        (ffxCreateInfo.presentMode == VK_PRESENT_MODE_FIFO_KHR || ffxCreateInfo.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR))
    {
        uint32_t modeCount = 0;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, ffxCreateInfo.surface, &modeCount, nullptr) == VK_SUCCESS && modeCount > 0)
        {
            std::vector<VkPresentModeKHR> modes(modeCount);
            if (vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, ffxCreateInfo.surface, &modeCount, modes.data()) == VK_SUCCESS)
            {
                bool hasImmediate = false;
                bool hasMailbox = false;
                for (VkPresentModeKHR m : modes)
                {
                    if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) hasImmediate = true;
                    else if (m == VK_PRESENT_MODE_MAILBOX_KHR) hasMailbox = true;
                }
                if (hasImmediate) ffxCreateInfo.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
                else if (hasMailbox) ffxCreateInfo.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
        }
    }
    const VkResult scResult = replacement.createSwapchainFFX(device, &ffxCreateInfo, allocator, &state.proxySwapchain, &fgInfo);
    state.nativeCreateAttempted = AsteriaNativeSwapchainCreateAttempted();
    if (scResult != VK_SUCCESS || state.proxySwapchain == VK_NULL_HANDLE)
        return scResult != VK_SUCCESS ? scResult : VK_ERROR_INITIALIZATION_FAILED;

    state.swapchainBox = asteria_vk::ProxyBox(device, state.proxySwapchain);
    const auto *wrapper = static_cast<FrameInterpolationSwapChainVK *>(asteria_vk::ProxyObject(device, state.proxySwapchain));
    if (wrapper) state.nativeSwapchain = wrapper->nativeSwapchain();
    if (state.nativeSwapchain) state.inputSession->BindNativeSwapchain(asteria_vk::Bits(state.nativeSwapchain));
    if (!state.swapchainBox)
    {
        vkDestroySwapchainFFX(device, state.proxySwapchain, allocator);
        state.proxySwapchain = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    state.inputSession->BindSwapchain(asteria_vk::Bits(state.proxySwapchain));

    state.scratch.resize(ffxGetScratchMemorySizeVK(physicalDevice, 2));
    if (ffxGetInterfaceVK(&state.backend, ffxDevice, state.scratch.data(), state.scratch.size(), 2) != FFX_OK)
    {
        DestroySwapchainResources(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    state.interpolation = std::make_unique<FfxFrameInterpolationContext>();
    state.opticalFlow = std::make_unique<FfxOpticalflowContext>();

    FfxFrameInterpolationContextDescription fiDesc{};
    fiDesc.maxRenderSize = {state.width, state.height};
    fiDesc.displaySize = {state.width, state.height};
    fiDesc.backBufferFormat = ffxGetSurfaceFormatVK(state.format);
    if (fiDesc.backBufferFormat == FFX_SURFACE_FORMAT_UNKNOWN)
        fiDesc.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    fiDesc.previousInterpolationSourceFormat = fiDesc.backBufferFormat;
    fiDesc.backendInterface = state.backend;
    if (ffxFrameInterpolationContextCreate(state.interpolation.get(), &fiDesc) != FFX_OK)
    {
        DestroySwapchainResources(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    state.interpolationCreated = true;

    FfxOpticalflowContextDescription ofDesc{};
    ofDesc.resolution = {state.width, state.height};
    ofDesc.backendInterface = state.backend;
    if (ffxOpticalflowContextCreate(state.opticalFlow.get(), &ofDesc) != FFX_OK)
    {
        DestroySwapchainResources(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    state.opticalFlowCreated = true;

    FfxFrameInterpolationSharedResourceDescriptions fiShared{};
    FfxOpticalflowSharedResourceDescriptions ofShared{};
    if (ffxFrameInterpolationGetSharedResourceDescriptions(state.interpolation.get(), &fiShared) != FFX_OK ||
        ffxOpticalflowGetSharedResourceDescriptions(state.opticalFlow.get(), &ofShared) != FFX_OK)
    {
        DestroySwapchainResources(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    state.estimatedInternalInputs = IsEstimatedInternalInputsRequested();
    if (!AllocateGpuImage(device, physicalDevice, fiShared.reconstructedPrevNearestDepth.resourceDescription, state.reconstructed) ||
        !AllocateGpuImage(device, physicalDevice, fiShared.dilatedDepth.resourceDescription, state.dilatedDepth) ||
        !AllocateGpuImage(device, physicalDevice, fiShared.dilatedMotionVectors.resourceDescription, state.dilatedMotion) ||
        !AllocateGpuImage(device, physicalDevice, ofShared.opticalFlowVector.resourceDescription, state.vectors) ||
        !AllocateGpuImage(device, physicalDevice, ofShared.opticalFlowSCD.resourceDescription, state.scd))
    {
        DestroySwapchainResources(state);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    FfxFrameGenerationConfig config{};
    config.swapChain = state.swapchainBox;
    config.frameGenerationEnabled = false;
    config.frameGenerationCallback = &LayerFrameGenerationCallback;
    config.frameGenerationCallbackContext = &state;
    config.allowAsyncWorkloads = false;
    config.frameID = 0;
    config.interpolationRect = {0, 0, int32_t(state.width), int32_t(state.height)};
    if (ffxSetFrameGenerationConfigToSwapchainVK(&config) != FFX_OK)
    {
        DestroySwapchainResources(state);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return VK_SUCCESS;
}

inline bool WaitForSwapchainIdle(FgSwapchainState &state)
{
    if (state.swapchainBox && ffxWaitForPresents(state.swapchainBox) != FFX_OK) return false;
    return RetireCurrentFrameInputs(state.currentInputs, [] { return true; });
}

inline bool AdvanceSwapchainFrameConfig(FgSwapchainState &state, uint32_t imageIndex)
{
    if (!state.swapchainBox || !state.inputSession) return false;
    const bool internalLifetime = state.currentInputs && state.currentInputs->lifetime.get() == &state;
    if (!RetireCurrentFrameInputs(state.currentInputs, [&state, internalLifetime] {
            return internalLifetime || ffxWaitForPresents(state.swapchainBox) == FFX_OK;
        }))
    {
        state.historyReset.store(true);
        return false;
    }
    const uint64_t frame = state.frameID++;
    std::shared_ptr<const FrameInputs> input;
    if (!state.estimatedInternalInputs && IsEstimatedInternalInputsRequested())
        state.estimatedInternalInputs = true;
    if (state.estimatedInternalInputs && !state.internalDepth.handle &&
        !AllocateGpuImage(state.device, state.physicalDevice, MakeImageDescription(FFX_SURFACE_FORMAT_R32_FLOAT, state.width, state.height), state.internalDepth))
        state.estimatedInternalInputs = false;
    if (state.estimatedInternalInputs && !state.estimatedMotion.handle &&
        !AllocateGpuImage(state.device, state.physicalDevice, MakeImageDescription(FFX_SURFACE_FORMAT_R32G32_FLOAT, state.width, state.height), state.estimatedMotion))
        state.estimatedInternalInputs = false;
    try { state.inputSession->BeginPresent(imageIndex); input = state.inputSession->Acquire(frame); } catch (...) {}
    bool valid = input && ValidFrameInputs(*input, reinterpret_cast<uintptr_t>(state.device), frame, state.width, state.height) &&
        asteria_vk::Decode<VkImage>(input->depth.resource, state.device, asteria_vk::HandleKind::Image) != VK_NULL_HANDLE &&
        asteria_vk::Decode<VkImage>(input->motion.resource, state.device, asteria_vk::HandleKind::Image) != VK_NULL_HANDLE;
    if (!valid && state.estimatedInternalInputs)
    {
        input = MakeEstimatedInternalInputs(state, frame);
        valid = input && ValidFrameInputs(*input, reinterpret_cast<uintptr_t>(state.device), frame, state.width, state.height) &&
            asteria_vk::Decode<VkImage>(input->depth.resource, state.device, asteria_vk::HandleKind::Image) != VK_NULL_HANDLE &&
            asteria_vk::Decode<VkImage>(input->motion.resource, state.device, asteria_vk::HandleKind::Image) != VK_NULL_HANDLE;
    }
    if (!valid) { input.reset(); state.historyReset.store(true); }
    std::atomic_store_explicit(&state.currentInputs, std::move(input), std::memory_order_release);
    FfxFrameGenerationConfig config{};
    config.swapChain = state.swapchainBox;
    config.frameGenerationEnabled = valid;
    config.frameGenerationCallback = &LayerFrameGenerationCallback;
    config.frameGenerationCallbackContext = &state;
    config.allowAsyncWorkloads = false;
    config.frameID = frame;
    config.interpolationRect = {0, 0, int32_t(state.width), int32_t(state.height)};
    if (ffxSetFrameGenerationConfigToSwapchainVK(&config) == FFX_OK) return true;
    state.historyReset.store(true);
    std::atomic_store_explicit(&state.currentInputs, std::shared_ptr<const FrameInputs>{}, std::memory_order_release);
    return false;
}

}
