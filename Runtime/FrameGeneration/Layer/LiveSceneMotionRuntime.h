#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

#include "../DepthCapture/DepthCapture.h"
#include "../DepthCapture/DepthCapture.cpp"
#include "../FrameAssociation/ObservedFrameAssociation.h"
#include "../MotionReconstruction/CameraMotionReconstruction.h"
#include "../Port/include/AsteriaVulkanShim.h"
#include "DescriptorEvidenceTracker.h"
#include "MappedBufferTracker.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace asteria_live_scene {

struct CommittedSceneFrame {
    bool valid = false;
    bool hasPreviousCamera = false;
    uint64_t frameID = 0;
    uint32_t mainWorldDraws = 0;
    float nearPlane = 7.0f;
    float farPlane = 5000.0f;
    float verticalFovRadians = 1.287f;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 0.0f, 1.0f};
    float right[3] = {1.0f, 0.0f, 0.0f};
    float forward[3] = {0.0f, 1.0f, 0.0f};
    std::array<float, 16> worldToClip = {};
    std::array<float, 16> inverseWorldToClip = {};
    std::array<float, 16> previousWorldToClip = {};
    VkViewport viewport = {};
    VkViewport previousViewport = {};
};

struct Diagnostics {
    uint64_t depthSnapshotsRecorded = 0;
    uint64_t depthResolvesDispatched = 0;
    uint64_t motionReconstructionsDispatched = 0;
    uint64_t cameraFramesCommitted = 0;
    uint64_t lastMainWorldDraws = 0;
    float lastNearPlane = 0.0f;
    float lastFarPlane = 0.0f;
};

inline bool EnvFlagDefaultTrue(const char *name) {
    char value[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, static_cast<DWORD>(sizeof(value)));
    if (length == 0 || length >= sizeof(value))
        return true;
    return std::strcmp(value, "0") != 0 && _stricmp(value, "false") != 0 && _stricmp(value, "off") != 0;
}

inline std::filesystem::path CurrentModuleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&CurrentModuleDirectory),
            &module) || !module)
        return {};
    std::vector<wchar_t> buffer(4096, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size())
        return {};
    return std::filesystem::path(buffer.data()).parent_path();
}

inline std::vector<uint32_t> LoadSpirvRelativeToLayer(const wchar_t *relativeFromFrameGenRoot) {
    const std::filesystem::path moduleDir = CurrentModuleDirectory();
    if (moduleDir.empty())
        return {};
    const std::array<std::filesystem::path, 3> roots = {
        moduleDir.parent_path().parent_path(),
        moduleDir.parent_path(),
        moduleDir
    };
    for (const auto &root : roots) {
        if (root.empty())
            continue;
        std::error_code ec;
        const std::filesystem::path candidate = root / relativeFromFrameGenRoot;
        if (!std::filesystem::is_regular_file(candidate, ec))
            continue;
        std::ifstream file(candidate, std::ios::binary | std::ios::ate);
        if (!file)
            continue;
        const std::streamsize bytes = file.tellg();
        if (bytes <= 0 || (bytes % 4) != 0)
            continue;
        std::vector<uint32_t> words(static_cast<size_t>(bytes / 4));
        file.seekg(0, std::ios::beg);
        if (file.read(reinterpret_cast<char *>(words.data()), bytes))
            return words;
    }
    return {};
}

inline bool InvertMatrix4x4(const std::array<float, 16> &m, std::array<float, 16> &invOut) {
    double inv[16];
    inv[0] = m[5]  * m[10] * m[15] - m[5]  * m[11] * m[14] - m[9]  * m[6]  * m[15] +
             m[9]  * m[7]  * m[14] + m[13] * m[6]  * m[11] - m[13] * m[7]  * m[10];
    inv[4] = -m[4]  * m[10] * m[15] + m[4]  * m[11] * m[14] + m[8]  * m[6]  * m[15] -
              m[8]  * m[7]  * m[14] - m[12] * m[6]  * m[11] + m[12] * m[7]  * m[10];
    inv[8] = m[4]  * m[9] * m[15] - m[4]  * m[11] * m[13] - m[8]  * m[5] * m[15] +
             m[8]  * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4]  * m[9] * m[14] + m[4]  * m[10] * m[13] + m[8]  * m[5] * m[14] -
               m[8]  * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1]  * m[10] * m[15] + m[1]  * m[11] * m[14] + m[9]  * m[2] * m[15] -
              m[9]  * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0]  * m[10] * m[15] - m[0]  * m[11] * m[14] - m[8]  * m[2] * m[15] +
             m[8]  * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0]  * m[9] * m[15] + m[0]  * m[11] * m[13] + m[8]  * m[1] * m[15] -
              m[8]  * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0]  * m[9] * m[14] - m[0]  * m[10] * m[13] - m[8]  * m[1] * m[14] +
              m[8]  * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1]  * m[6] * m[15] - m[1]  * m[7] * m[14] - m[5]  * m[2] * m[15] +
             m[5]  * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0]  * m[6] * m[15] + m[0]  * m[7] * m[14] + m[4]  * m[2] * m[15] -
              m[4]  * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0]  * m[5] * m[15] - m[0]  * m[7] * m[13] - m[4]  * m[1] * m[15] +
              m[4]  * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0]  * m[5] * m[14] + m[0]  * m[6] * m[13] + m[4]  * m[1] * m[14] -
               m[4]  * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
              m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
               m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-12)
        return false;
    const double invDet = 1.0 / det;
    for (size_t i = 0; i < 16; ++i) {
        const double v = inv[i] * invDet;
        if (!std::isfinite(v))
            return false;
        invOut[i] = static_cast<float>(v);
    }
    return true;
}

class Runtime {
public:
    void OnCreateImage(VkDevice device, const VkImageCreateInfo *info, VkImage image) {
        if (!device || !info || image == VK_NULL_HANDLE)
            return;
        if (info->imageType != VK_IMAGE_TYPE_2D || info->mipLevels != 1 || info->arrayLayers != 1)
            return;
        if (info->format != VK_FORMAT_D24_UNORM_S8_UINT || info->samples != VK_SAMPLE_COUNT_4_BIT)
            return;
        if ((info->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto &entry = devices_[Key(device)];
        entry.depthImages[Bits(image)] = DepthImageMeta{
            image, info->format, info->samples, info->extent.width, info->extent.height, info->usage
        };
    }

    void OnDestroyImage(VkDevice device, VkImage image) {
        if (!device || image == VK_NULL_HANDLE)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(Key(device));
        if (it == devices_.end())
            return;
        it->second.depthImages.erase(Bits(image));
    }

    void OnCreateImageView(VkDevice device, const VkImageViewCreateInfo *info, VkImageView view) {
        if (!device || !info || view == VK_NULL_HANDLE || info->image == VK_NULL_HANDLE)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(Key(device));
        if (it == devices_.end())
            return;
        if (it->second.depthImages.find(Bits(info->image)) == it->second.depthImages.end())
            return;
        it->second.depthViews[Bits(view)] = info->image;
    }

    void OnDestroyImageView(VkDevice device, VkImageView view) {
        if (!device || view == VK_NULL_HANDLE)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(Key(device));
        if (it == devices_.end())
            return;
        it->second.depthViews.erase(Bits(view));
    }

    void OnResetCommandBuffer(VkCommandBuffer command) {
        if (!command)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.erase(Key(command));
    }

    void OnFreeCommandBuffers(uint32_t count, const VkCommandBuffer *commands) {
        if (!commands || count == 0)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < count; ++i)
            if (commands[i])
                commands_.erase(Key(commands[i]));
    }

    void OnBeginRendering(
        VkDevice device,
        PFN_vkGetDeviceProcAddr gdpa,
        VkCommandBuffer command,
        const VkRenderingInfo *renderingInfo,
        bool isKhr)
    {
        if (!device || !command || !renderingInfo)
            return;
        SnapshotAction pendingSnapshot{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto devIt = devices_.find(Key(device));
            if (devIt == devices_.end())
                return;
            auto &dev = devIt->second;
            auto &cmd = commands_[Key(command)];
            cmd = CommandState{};
            cmd.device = device;
            cmd.gdpa = gdpa;
            cmd.isKhr = isKhr;

            if (!renderingInfo->pDepthAttachment || renderingInfo->pDepthAttachment->imageView == VK_NULL_HANDLE)
                return;
            const VkImageView depthView = renderingInfo->pDepthAttachment->imageView;
            auto viewIt = dev.depthViews.find(Bits(depthView));
            if (viewIt == dev.depthViews.end())
                return;
            auto imgIt = dev.depthImages.find(Bits(viewIt->second));
            if (imgIt == dev.depthImages.end())
                return;
            const DepthImageMeta &meta = imgIt->second;
            if (dev.targetWidth != 0 && (meta.width != dev.targetWidth || meta.height != dev.targetHeight))
                return;

            cmd.inTargetDepthPass = true;
            cmd.depthImage = meta.image;
            cmd.depthMeta = meta;
            cmd.depthLayout = renderingInfo->pDepthAttachment->imageLayout != VK_IMAGE_LAYOUT_UNDEFINED
                ? renderingInfo->pDepthAttachment->imageLayout
                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            SaveRenderingInfo(cmd, *renderingInfo);

            const bool clearingDepth = renderingInfo->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
            if (clearingDepth && dev.activeDepthImage == meta.image &&
                dev.mainWorldDrawsSinceClear >= 4 &&
                dev.mainWorldDrawsSinceClear >= dev.bestMainWorldDrawsThisFrame &&
                dev.hasCandidateCamera && dev.depthReady)
            {
                pendingSnapshot.shouldSnapshot = true;
                pendingSnapshot.source = MakeDepthSource(meta, cmd.depthLayout);
                dev.bestMainWorldDrawsThisFrame = dev.mainWorldDrawsSinceClear;
                dev.committedCameraThisFrame = dev.candidateCamera;
                dev.committedViewportThisFrame = dev.candidateViewport;
                dev.hasCommittedCameraThisFrame = true;
                dev.snapshotRecordedThisFrame = true;
            }
            if (clearingDepth || dev.activeDepthImage != meta.image) {
                dev.activeDepthImage = meta.image;
                dev.mainWorldDrawsSinceClear = 0;
                dev.hasCandidateCamera = false;
            }
        }
        if (pendingSnapshot.shouldSnapshot)
            RecordSnapshotNow(device, command, pendingSnapshot.source);
    }

    void OnDraw(
        VkDevice device,
        VkCommandBuffer command,
        asteria_descriptor_evidence::Tracker &descriptors,
        asteria_mapped_buffer::Tracker &mappedBuffers)
    {
        if (!device || !command)
            return;
        DepthImageMeta targetMeta{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto cmdIt = commands_.find(Key(command));
            if (cmdIt == commands_.end() || !cmdIt->second.inTargetDepthPass)
                return;
            auto devIt = devices_.find(Key(device));
            if (devIt != devices_.end()) {
                auto &dev = devIt->second;
                if (dev.mainWorldDrawsSinceClear >= 8 && dev.hasCandidateCamera) {
                    ++dev.mainWorldDrawsSinceClear;
                    return;
                }
            }
            targetMeta = cmdIt->second.depthMeta;
        }

        AsteriaNativeViewportEvidenceV1 viewportEv{};
        if (!descriptors.QueryViewport(Key(device), Key(command), viewportEv) ||
            viewportEv.status != ASTERIA_VIEWPORT_EVIDENCE_OK)
            return;
        const VkViewport viewport{
            viewportEv.viewport[0], viewportEv.viewport[1], viewportEv.viewport[2],
            viewportEv.viewport[3], viewportEv.viewport[4], viewportEv.viewport[5]};
        const float absW = std::fabs(viewport.width);
        const float absH = std::fabs(viewport.height);
        if (absW < static_cast<float>(targetMeta.width) * 0.9f ||
            absH < static_cast<float>(targetMeta.height) * 0.9f ||
            std::fabs(viewport.minDepth - 0.0f) > 0.01f ||
            std::fabs(viewport.maxDepth - 1.0f) > 0.01f)
            return;

        AsteriaBoundBufferDescriptorV1 bound{};
        if (!descriptors.Query(Key(device), Key(command), 0u, 2u, 0u, bound)) {
            if (!descriptors.Query(Key(device), Key(command), 2u, 0u, 0u, bound))
                return;
        }
        if (bound.buffer == 0u || bound.range < 448u)
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto devIt = devices_.find(Key(device));
        if (devIt == devices_.end())
            return;
        auto &dev = devIt->second;

        if (dev.lastCheckedBuffer != bound.buffer || dev.lastCheckedOffset != bound.effective_offset) {
            dev.lastCheckedBuffer = bound.buffer;
            dev.lastCheckedOffset = bound.effective_offset;
            dev.lastCheckedCameraValid = false;

            std::array<float, 16> matrix{};
            if (mappedBuffers.Copy(
                    Key(device), bound.buffer, bound.effective_offset + 384u, 64u, matrix.data()))
            {
                asteria_frame_association::Camera recovered{};
                if (asteria_frame_association::RecoverNativePerspective(matrix, recovered) &&
                    recovered.nearPlane >= 3.5f && recovered.nearPlane <= 64.0f &&
                    recovered.farPlane >= 128.0f)
                {
                    dev.lastCheckedCamera = recovered;
                    dev.lastCheckedCameraValid = true;
                }
            }
        }

        if (dev.lastCheckedCameraValid) {
            dev.activeDepthImage = targetMeta.image;
            ++dev.mainWorldDrawsSinceClear;
            dev.candidateCamera = dev.lastCheckedCamera;
            dev.candidateViewport = viewport;
            dev.hasCandidateCamera = true;
        }
    }

    void OnBeforeClearAttachments(
        VkDevice device,
        PFN_vkGetDeviceProcAddr gdpa,
        VkCommandBuffer command,
        uint32_t attachmentCount,
        const VkClearAttachment *pAttachments,
        uint32_t rectCount,
        const VkClearRect *pRects)
    {
        if (!device || !command || !pAttachments || attachmentCount == 0 || !pRects || rectCount == 0)
            return;
        bool clearsDepth = false;
        for (uint32_t i = 0; i < attachmentCount; ++i) {
            if ((pAttachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
                clearsDepth = true;
                break;
            }
        }
        if (!clearsDepth)
            return;

        SnapshotAction action{};
        CommandState savedCmd{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto cmdIt = commands_.find(Key(command));
            if (cmdIt == commands_.end() || !cmdIt->second.inTargetDepthPass)
                return;
            auto devIt = devices_.find(Key(device));
            if (devIt == devices_.end())
                return;
            auto &dev = devIt->second;
            auto &cmd = cmdIt->second;

            bool fullRect = false;
            for (uint32_t i = 0; i < rectCount; ++i) {
                if (pRects[i].rect.extent.width >= cmd.depthMeta.width * 3 / 4 &&
                    pRects[i].rect.extent.height >= cmd.depthMeta.height * 3 / 4)
                {
                    fullRect = true;
                    break;
                }
            }
            if (!fullRect)
                return;

            if (dev.mainWorldDrawsSinceClear >= 4 &&
                dev.mainWorldDrawsSinceClear >= dev.bestMainWorldDrawsThisFrame &&
                dev.hasCandidateCamera && dev.depthReady)
            {
                action.shouldSnapshot = true;
                action.source = MakeDepthSource(cmd.depthMeta, cmd.depthLayout);
                savedCmd = cmd;
                dev.bestMainWorldDrawsThisFrame = dev.mainWorldDrawsSinceClear;
                dev.committedCameraThisFrame = dev.candidateCamera;
                dev.committedViewportThisFrame = dev.candidateViewport;
                dev.hasCommittedCameraThisFrame = true;
                dev.snapshotRecordedThisFrame = true;
            }
            dev.mainWorldDrawsSinceClear = 0;
            dev.hasCandidateCamera = false;
        }

        if (action.shouldSnapshot) {
            EndDynamicRendering(device, gdpa, command, savedCmd.isKhr);
            RecordSnapshotNow(device, command, action.source);
            ResumeDynamicRendering(device, gdpa, command, savedCmd);
        }
    }

    void OnAfterEndRendering(VkDevice device, VkCommandBuffer command) {
        if (!device || !command)
            return;
        SnapshotAction action{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto cmdIt = commands_.find(Key(command));
            if (cmdIt == commands_.end())
                return;
            CommandState ended = cmdIt->second;
            cmdIt->second.inTargetDepthPass = false;
            if (!ended.inTargetDepthPass)
                return;
            auto devIt = devices_.find(Key(device));
            if (devIt == devices_.end())
                return;
            auto &dev = devIt->second;
            if (dev.mainWorldDrawsSinceClear >= 4 &&
                dev.mainWorldDrawsSinceClear >= dev.bestMainWorldDrawsThisFrame &&
                dev.hasCandidateCamera && dev.depthReady)
            {
                action.shouldSnapshot = true;
                action.source = MakeDepthSource(ended.depthMeta, ended.depthLayout);
                dev.bestMainWorldDrawsThisFrame = dev.mainWorldDrawsSinceClear;
                dev.committedCameraThisFrame = dev.candidateCamera;
                dev.committedViewportThisFrame = dev.candidateViewport;
                dev.hasCommittedCameraThisFrame = true;
                dev.snapshotRecordedThisFrame = true;
            }
        }
        if (action.shouldSnapshot)
            RecordSnapshotNow(device, command, action.source);
    }

    bool EnsureInitialized(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        PFN_vkGetDeviceProcAddr gdpa,
        uint32_t width,
        uint32_t height,
        VkImage internalDepthImage,
        VkImage reconstructedMotionImage)
    {
        if (!device || !physicalDevice || !gdpa || width == 0 || height == 0 ||
            internalDepthImage == VK_NULL_HANDLE || reconstructedMotionImage == VK_NULL_HANDLE)
            return false;

        std::lock_guard<std::mutex> lock(mutex_);
        auto &dev = devices_[Key(device)];
        dev.device = device;
        dev.physicalDevice = physicalDevice;
        dev.gdpa = gdpa;
        dev.targetWidth = width;
        dev.targetHeight = height;

        if (dev.depthReady && dev.motionReady &&
            dev.initializedWidth == width && dev.initializedHeight == height &&
            dev.boundInternalDepth == internalDepthImage &&
            dev.boundReconstructedMotion == reconstructedMotionImage)
            return true;

        DestroyDeviceGpuResourcesLocked(dev);

        if (resolveSpirv_.empty())
            resolveSpirv_ = LoadSpirvRelativeToLayer(L"DepthCapture\\build\\ResolveDepth.spv");
        if (reconstructSpirv_.empty())
            reconstructSpirv_ = LoadSpirvRelativeToLayer(L"MotionReconstruction\\build\\ReconstructMotion.spv");
        if (resolveSpirv_.empty() || reconstructSpirv_.empty())
            return false;

        auto getMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            vkGetInstanceProcAddr(nullptr, "vkGetPhysicalDeviceMemoryProperties"));
        if (!getMemProps)
            return false;
        VkPhysicalDeviceMemoryProperties memProps{};
        getMemProps(physicalDevice, &memProps);

        dev.depthReady = dev.depthCapture.Initialize(device, gdpa, memProps, width, height, resolveSpirv_);
        if (!dev.depthReady) {
            DestroyDeviceGpuResourcesLocked(dev);
            return false;
        }

        if (!CreateStorageImageAndView(device, gdpa, memProps, width, height, VK_FORMAT_R32_SFLOAT,
                                       dev.previousDepthImage, dev.previousDepthMemory, dev.previousDepthStorageView) ||
            !CreateStorageImageAndView(device, gdpa, memProps, width, height, VK_FORMAT_R32_SFLOAT,
                                       dev.confidenceImage, dev.confidenceMemory, dev.confidenceStorageView) ||
            !CreateImageViewOnly(device, gdpa, internalDepthImage, VK_FORMAT_R32_SFLOAT, dev.currentDepthStorageView) ||
            !CreateImageViewOnly(device, gdpa, reconstructedMotionImage, VK_FORMAT_R32G32_SFLOAT, dev.motionStorageView) ||
            !InitializeMotionRingLocked(dev, memProps, reconstructSpirv_))
        {
            DestroyDeviceGpuResourcesLocked(dev);
            return false;
        }
        dev.motionReady = true;

        dev.initializedWidth = width;
        dev.initializedHeight = height;
        dev.boundInternalDepth = internalDepthImage;
        dev.boundReconstructedMotion = reconstructedMotionImage;
        dev.previousDepthInitialized = false;
        return true;
    }

    CommittedSceneFrame CommitFrameOnPresent(
        VkDevice device,
        uint32_t width,
        uint32_t height,
        uint64_t frameID)
    {
        CommittedSceneFrame result{};
        if (!device)
            return result;
        std::lock_guard<std::mutex> lock(mutex_);
        auto devIt = devices_.find(Key(device));
        if (devIt == devices_.end())
            return result;
        auto &dev = devIt->second;
        dev.targetWidth = width;
        dev.targetHeight = height;

        if (dev.snapshotRecordedThisFrame && dev.hasCommittedCameraThisFrame) {
            std::array<float, 16> invWorldToClip{};
            if (InvertMatrix4x4(dev.committedCameraThisFrame.worldToClip, invWorldToClip)) {
                result.valid = true;
                result.frameID = frameID;
                result.mainWorldDraws = dev.bestMainWorldDrawsThisFrame;
                result.nearPlane = dev.committedCameraThisFrame.nearPlane;
                result.farPlane = dev.committedCameraThisFrame.farPlane;
                result.verticalFovRadians = dev.committedCameraThisFrame.verticalFovRadians;
                for (int i = 0; i < 3; ++i) {
                    result.position[i] = dev.committedCameraThisFrame.origin[i];
                    result.right[i] = dev.committedCameraThisFrame.right[i];
                    result.up[i] = dev.committedCameraThisFrame.up[i];
                    result.forward[i] = -dev.committedCameraThisFrame.backward[i];
                }
                result.worldToClip = dev.committedCameraThisFrame.worldToClip;
                result.inverseWorldToClip = invWorldToClip;
                result.viewport = dev.committedViewportThisFrame;
                result.hasPreviousCamera = dev.hasPreviousFrameCamera;
                result.previousWorldToClip = dev.hasPreviousFrameCamera
                    ? dev.previousFrameWorldToClip
                    : dev.committedCameraThisFrame.worldToClip;
                result.previousViewport = dev.hasPreviousFrameCamera
                    ? dev.previousFrameViewport
                    : dev.committedViewportThisFrame;

                dev.previousFrameWorldToClip = dev.committedCameraThisFrame.worldToClip;
                dev.previousFrameViewport = dev.committedViewportThisFrame;
                dev.hasPreviousFrameCamera = true;
                dev.latestCommittedFrame = result;

                ++diagnostics_.cameraFramesCommitted;
                diagnostics_.lastMainWorldDraws = result.mainWorldDraws;
                diagnostics_.lastNearPlane = result.nearPlane;
                diagnostics_.lastFarPlane = result.farPlane;
            }
        } else {
            dev.latestCommittedFrame = CommittedSceneFrame{};
        }

        dev.bestMainWorldDrawsThisFrame = 0;
        dev.mainWorldDrawsSinceClear = 0;
        dev.hasCandidateCamera = false;
        dev.hasCommittedCameraThisFrame = false;
        dev.snapshotRecordedThisFrame = false;
        dev.lastCheckedBuffer = 0;
        dev.lastCheckedOffset = 0;
        dev.lastCheckedCameraValid = false;
        return result;
    }

    bool RecordResolveAndMotion(
        VkDevice device,
        VkCommandBuffer command,
        VkImage internalDepthImage,
        VkImage reconstructedMotionImage,
        bool resetHistory)
    {
        if (!device || !command || internalDepthImage == VK_NULL_HANDLE ||
            reconstructedMotionImage == VK_NULL_HANDLE || !EnvFlagDefaultTrue("ASTERIA_FG_TRUE_DEPTH"))
            return false;

        CommittedSceneFrame committed{};
        DeviceEntry *entry = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = devices_.find(Key(device));
            if (it == devices_.end())
                return false;
            entry = &it->second;
            if (!entry->depthReady || entry->boundInternalDepth != internalDepthImage ||
                entry->boundReconstructedMotion != reconstructedMotionImage)
                return false;
            committed = entry->latestCommittedFrame;
            if (!committed.valid)
                return false;
        }

        VkImage resolvedDepthImage = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!entry->depthCapture.RecordResolve(command))
                return false;
            resolvedDepthImage = entry->depthCapture.Current().image;
            ++diagnostics_.depthResolvesDispatched;
        }
        if (resolvedDepthImage == VK_NULL_HANDLE)
            return false;

        const DeviceDispatch fn = ResolveDeviceDispatch(entry->gdpa, device);
        if (!fn.cmdPipelineBarrier || !fn.cmdCopyImage)
            return false;

        TransitionImage(fn, command, resolvedDepthImage,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        TransitionImage(fn, command, internalDepthImage,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);

        VkImageCopy depthCopy{};
        depthCopy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        depthCopy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        depthCopy.extent = {entry->initializedWidth, entry->initializedHeight, 1};
        fn.cmdCopyImage(command, resolvedDepthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        internalDepthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopy);

        TransitionImage(fn, command, resolvedDepthImage,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        TransitionImage(fn, command, internalDepthImage,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);

        const bool motionReconstructionEnabled = EnvFlagDefaultTrue("ASTERIA_FG_MOTION_RECONSTRUCTION");
        if (entry->motionReady && motionReconstructionEnabled) {
            TransitionImage(fn, command, entry->previousDepthImage,
                            entry->previousDepthInitialized ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_GENERAL,
                            entry->previousDepthInitialized ? VK_ACCESS_TRANSFER_WRITE_BIT : 0,
                            VK_ACCESS_SHADER_READ_BIT,
                            entry->previousDepthInitialized ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);
            TransitionImage(fn, command, reconstructedMotionImage,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            0, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);
            TransitionImage(fn, command, entry->confidenceImage,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                            0, VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);

            asteria_camera_motion::Views views{};
            views.currentDepth = entry->currentDepthStorageView;
            views.previousDepth = entry->previousDepthStorageView;
            views.motion = entry->motionStorageView;
            views.confidence = entry->confidenceStorageView;

            asteria_camera_motion::Parameters params{};
            params.inverseCurrentViewProjection = committed.inverseWorldToClip;
            params.previousViewProjection = committed.previousWorldToClip;
            params.extentFlags[0] = entry->initializedWidth;
            params.extentFlags[1] = entry->initializedHeight;
            const bool validHistory = committed.hasPreviousCamera && entry->previousDepthInitialized && !resetHistory;
            params.extentFlags[2] = (validHistory ? 1u : 0u) |
                                    (!validHistory ? 2u : 0u) |
                                    (validHistory ? 4u : 0u);
            params.depthLimits[0] = 0.000001f;
            params.depthLimits[1] = 0.999999f;
            params.depthLimits[2] = 1.0f;
            params.previousDepthControls[0] = 0.025f;
            params.previousDepthControls[1] = 0.0f;
            const asteria_camera_motion::NativeViewport curVp{
                {committed.viewport.x, committed.viewport.y, committed.viewport.width, committed.viewport.height},
                {committed.viewport.minDepth, committed.viewport.maxDepth},
                true};
            const asteria_camera_motion::NativeViewport prevVp{
                {committed.previousViewport.x, committed.previousViewport.y, committed.previousViewport.width, committed.previousViewport.height},
                {committed.previousViewport.minDepth, committed.previousViewport.maxDepth},
                true};
            asteria_camera_motion::SetNativeViewports(params, curVp, prevVp);
            params = asteria_camera_motion::ValidatedParameters(params);

            if (fn.cmdBindPipeline && fn.cmdBindDescriptorSets && fn.cmdDispatch &&
                entry->motionComputePipeline != VK_NULL_HANDLE)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto &slot = entry->motionSlots[entry->lifetimeCursor % entry->motionSlots.size()];
                ++entry->lifetimeCursor;
                if (slot.mappedUniform && slot.descriptorSet != VK_NULL_HANDLE) {
                    std::memcpy(slot.mappedUniform, &params, sizeof(params));
                    fn.cmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, entry->motionComputePipeline);
                    fn.cmdBindDescriptorSets(
                        command, VK_PIPELINE_BIND_POINT_COMPUTE, entry->motionPipelineLayout,
                        0, 1, &slot.descriptorSet, 0, nullptr);
                    fn.cmdDispatch(
                        command,
                        (entry->initializedWidth + 7u) / 8u,
                        (entry->initializedHeight + 7u) / 8u,
                        1u);
                    ++diagnostics_.motionReconstructionsDispatched;
                }
            }
        } else {
            TransitionImage(fn, command, reconstructedMotionImage,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);
            if (fn.cmdClearColorImage) {
                VkClearColorValue zero{};
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                fn.cmdClearColorImage(command, reconstructedMotionImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            }
        }

        TransitionImage(fn, command, internalDepthImage,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        TransitionImage(fn, command, entry->previousDepthImage,
                        (entry->motionReady && motionReconstructionEnabled) ? VK_IMAGE_LAYOUT_GENERAL :
                        (entry->previousDepthInitialized ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        (entry->motionReady && motionReconstructionEnabled) ? VK_ACCESS_SHADER_READ_BIT : 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        (entry->motionReady && motionReconstructionEnabled) ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);

        fn.cmdCopyImage(command, internalDepthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        entry->previousDepthImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopy);
        entry->previousDepthInitialized = true;

        TransitionImage(fn, command, internalDepthImage,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        TransitionImage(fn, command, reconstructedMotionImage,
                        (entry->motionReady && motionReconstructionEnabled) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        (entry->motionReady && motionReconstructionEnabled) ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT,
                        (entry->motionReady && motionReconstructionEnabled) ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
        return true;
    }

    void ReleaseSwapchainResources(VkDevice device) {
        if (!device)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(Key(device));
        if (it == devices_.end())
            return;
        DestroyDeviceGpuResourcesLocked(it->second);
    }

    void OnDestroyDevice(VkDevice device) {
        if (!device)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(Key(device));
        if (it != devices_.end()) {
            DestroyDeviceGpuResourcesLocked(it->second);
            devices_.erase(it);
        }
        for (auto cmdIt = commands_.begin(); cmdIt != commands_.end();) {
            if (cmdIt->second.device == device)
                cmdIt = commands_.erase(cmdIt);
            else
                ++cmdIt;
        }
    }

    Diagnostics QueryDiagnostics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return diagnostics_;
    }

private:
    struct DepthImageMeta {
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        uint32_t width = 0;
        uint32_t height = 0;
        VkImageUsageFlags usage = 0;
    };

    struct CommandState {
        VkDevice device = VK_NULL_HANDLE;
        PFN_vkGetDeviceProcAddr gdpa = nullptr;
        bool inTargetDepthPass = false;
        bool isKhr = false;
        VkImage depthImage = VK_NULL_HANDLE;
        DepthImageMeta depthMeta{};
        VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkRenderingFlags renderingFlags = 0;
        VkRect2D renderArea{};
        uint32_t layerCount = 1;
        uint32_t viewMask = 0;
        std::vector<VkRenderingAttachmentInfo> colorAttachments;
        bool hasDepthAttachment = false;
        VkRenderingAttachmentInfo depthAttachment{};
        bool hasStencilAttachment = false;
        VkRenderingAttachmentInfo stencilAttachment{};
    };

    struct SnapshotAction {
        bool shouldSnapshot = false;
        asteria_depth_capture::Source source{};
    };

    struct DeviceEntry {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        PFN_vkGetDeviceProcAddr gdpa = nullptr;
        uint32_t targetWidth = 0;
        uint32_t targetHeight = 0;
        uint32_t initializedWidth = 0;
        uint32_t initializedHeight = 0;
        VkImage boundInternalDepth = VK_NULL_HANDLE;
        VkImage boundReconstructedMotion = VK_NULL_HANDLE;

        std::unordered_map<uint64_t, DepthImageMeta> depthImages;
        std::unordered_map<uint64_t, VkImage> depthViews;

        VkImage activeDepthImage = VK_NULL_HANDLE;
        uint32_t mainWorldDrawsSinceClear = 0;
        uint32_t bestMainWorldDrawsThisFrame = 0;
        bool hasCandidateCamera = false;
        asteria_frame_association::Camera candidateCamera{};
        VkViewport candidateViewport{};
        bool hasCommittedCameraThisFrame = false;
        asteria_frame_association::Camera committedCameraThisFrame{};
        VkViewport committedViewportThisFrame{};
        bool snapshotRecordedThisFrame = false;

        uint64_t lastCheckedBuffer = 0;
        VkDeviceSize lastCheckedOffset = 0;
        bool lastCheckedCameraValid = false;
        asteria_frame_association::Camera lastCheckedCamera{};

        bool hasPreviousFrameCamera = false;
        std::array<float, 16> previousFrameWorldToClip{};
        VkViewport previousFrameViewport{};
        CommittedSceneFrame latestCommittedFrame{};

        bool depthReady = false;
        bool motionReady = false;
        asteria_depth_capture::Capture depthCapture;
        VkDescriptorSetLayout motionDescriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout motionPipelineLayout = VK_NULL_HANDLE;
        VkPipeline motionComputePipeline = VK_NULL_HANDLE;
        VkDescriptorPool motionDescriptorPool = VK_NULL_HANDLE;
        struct MotionSlot {
            VkBuffer uniformBuffer = VK_NULL_HANDLE;
            VkDeviceMemory uniformMemory = VK_NULL_HANDLE;
            void *mappedUniform = nullptr;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        };
        std::array<MotionSlot, 8> motionSlots{};
        size_t lifetimeCursor = 0;

        VkImage previousDepthImage = VK_NULL_HANDLE;
        VkDeviceMemory previousDepthMemory = VK_NULL_HANDLE;
        VkImageView previousDepthStorageView = VK_NULL_HANDLE;
        bool previousDepthInitialized = false;

        VkImage confidenceImage = VK_NULL_HANDLE;
        VkDeviceMemory confidenceMemory = VK_NULL_HANDLE;
        VkImageView confidenceStorageView = VK_NULL_HANDLE;

        VkImageView currentDepthStorageView = VK_NULL_HANDLE;
        VkImageView motionStorageView = VK_NULL_HANDLE;
    };

    struct DeviceDispatch {
        PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
        PFN_vkCmdCopyImage cmdCopyImage = nullptr;
        PFN_vkCmdClearColorImage cmdClearColorImage = nullptr;
        PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
        PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
        PFN_vkCmdDispatch cmdDispatch = nullptr;
    };

    static uintptr_t Key(const void *handle) { return reinterpret_cast<uintptr_t>(handle); }
    template <typename T>
    static uint64_t Bits(T handle) { return asteria_vk::Bits(handle); }

    static asteria_depth_capture::Source MakeDepthSource(const DepthImageMeta &meta, VkImageLayout layout) {
        asteria_depth_capture::Source src{};
        src.image = meta.image;
        src.format = meta.format;
        src.samples = meta.samples;
        src.width = meta.width;
        src.height = meta.height;
        src.layout = layout;
        src.insideRenderPass = false;
        src.reversedZ = false;
        return src;
    }

    static void SaveRenderingInfo(CommandState &cmd, const VkRenderingInfo &info) {
        cmd.renderingFlags = info.flags;
        cmd.renderArea = info.renderArea;
        cmd.layerCount = info.layerCount;
        cmd.viewMask = info.viewMask;
        cmd.colorAttachments.assign(info.pColorAttachments, info.pColorAttachments + info.colorAttachmentCount);
        for (auto &att : cmd.colorAttachments) {
            att.pNext = nullptr;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        if (info.pDepthAttachment) {
            cmd.hasDepthAttachment = true;
            cmd.depthAttachment = *info.pDepthAttachment;
            cmd.depthAttachment.pNext = nullptr;
            cmd.depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            cmd.depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        if (info.pStencilAttachment) {
            cmd.hasStencilAttachment = true;
            cmd.stencilAttachment = *info.pStencilAttachment;
            cmd.stencilAttachment.pNext = nullptr;
            cmd.stencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            cmd.stencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
    }

    static void EndDynamicRendering(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, VkCommandBuffer command, bool isKhr) {
        if (!gdpa)
            return;
        auto fn = reinterpret_cast<PFN_vkCmdEndRendering>(
            gdpa(device, isKhr ? "vkCmdEndRenderingKHR" : "vkCmdEndRendering"));
        if (!fn)
            fn = reinterpret_cast<PFN_vkCmdEndRendering>(gdpa(device, "vkCmdEndRendering"));
        if (fn)
            fn(command);
    }

    static void ResumeDynamicRendering(
        VkDevice device,
        PFN_vkGetDeviceProcAddr gdpa,
        VkCommandBuffer command,
        const CommandState &cmd)
    {
        if (!gdpa)
            return;
        auto fn = reinterpret_cast<PFN_vkCmdBeginRendering>(
            gdpa(device, cmd.isKhr ? "vkCmdBeginRenderingKHR" : "vkCmdBeginRendering"));
        if (!fn)
            fn = reinterpret_cast<PFN_vkCmdBeginRendering>(gdpa(device, "vkCmdBeginRendering"));
        if (!fn)
            return;
        VkRenderingInfo resume{VK_STRUCTURE_TYPE_RENDERING_INFO};
        resume.flags = cmd.renderingFlags;
        resume.renderArea = cmd.renderArea;
        resume.layerCount = cmd.layerCount;
        resume.viewMask = cmd.viewMask;
        resume.colorAttachmentCount = static_cast<uint32_t>(cmd.colorAttachments.size());
        resume.pColorAttachments = cmd.colorAttachments.empty() ? nullptr : cmd.colorAttachments.data();
        resume.pDepthAttachment = cmd.hasDepthAttachment ? &cmd.depthAttachment : nullptr;
        resume.pStencilAttachment = cmd.hasStencilAttachment ? &cmd.stencilAttachment : nullptr;
        fn(command, &resume);
    }

    void RecordSnapshotNow(VkDevice device, VkCommandBuffer command, const asteria_depth_capture::Source &source) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(Key(device));
        if (it == devices_.end() || !it->second.depthReady)
            return;
        if (it->second.depthCapture.RecordSnapshotBeforeClear(command, source))
            ++diagnostics_.depthSnapshotsRecorded;
    }

    static DeviceDispatch ResolveDeviceDispatch(PFN_vkGetDeviceProcAddr gdpa, VkDevice device) {
        DeviceDispatch fn{};
        if (!gdpa || !device)
            return fn;
        fn.cmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(gdpa(device, "vkCmdPipelineBarrier"));
        fn.cmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(gdpa(device, "vkCmdCopyImage"));
        fn.cmdClearColorImage = reinterpret_cast<PFN_vkCmdClearColorImage>(gdpa(device, "vkCmdClearColorImage"));
        fn.cmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(gdpa(device, "vkCmdBindPipeline"));
        fn.cmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(gdpa(device, "vkCmdBindDescriptorSets"));
        fn.cmdDispatch = reinterpret_cast<PFN_vkCmdDispatch>(gdpa(device, "vkCmdDispatch"));
        return fn;
    }

    static void TransitionImage(
        const DeviceDispatch &fn,
        VkCommandBuffer command,
        VkImage image,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkAccessFlags srcAccess,
        VkAccessFlags dstAccess,
        VkPipelineStageFlags srcStage,
        VkPipelineStageFlags dstStage,
        VkImageAspectFlags aspectMask)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspectMask, 0, 1, 0, 1};
        fn.cmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    static bool CreateImageViewOnly(
        VkDevice device,
        PFN_vkGetDeviceProcAddr gdpa,
        VkImage image,
        VkFormat format,
        VkImageView &outView)
    {
        auto createView = reinterpret_cast<PFN_vkCreateImageView>(gdpa(device, "vkCreateImageView"));
        if (!createView)
            return false;
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return createView(device, &viewInfo, nullptr, &outView) == VK_SUCCESS;
    }

    static bool CreateStorageImageAndView(
        VkDevice device,
        PFN_vkGetDeviceProcAddr gdpa,
        const VkPhysicalDeviceMemoryProperties &memProps,
        uint32_t width,
        uint32_t height,
        VkFormat format,
        VkImage &outImage,
        VkDeviceMemory &outMemory,
        VkImageView &outView)
    {
        auto createImage = reinterpret_cast<PFN_vkCreateImage>(gdpa(device, "vkCreateImage"));
        auto destroyImage = reinterpret_cast<PFN_vkDestroyImage>(gdpa(device, "vkDestroyImage"));
        auto getMemReq = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(gdpa(device, "vkGetImageMemoryRequirements"));
        auto allocMem = reinterpret_cast<PFN_vkAllocateMemory>(gdpa(device, "vkAllocateMemory"));
        auto freeMem = reinterpret_cast<PFN_vkFreeMemory>(gdpa(device, "vkFreeMemory"));
        auto bindMem = reinterpret_cast<PFN_vkBindImageMemory>(gdpa(device, "vkBindImageMemory"));
        if (!createImage || !destroyImage || !getMemReq || !allocMem || !freeMem || !bindMem)
            return false;

        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (createImage(device, &info, nullptr, &outImage) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req{};
        getMemReq(device, outImage, &req);
        uint32_t typeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            {
                typeIndex = i;
                break;
            }
        }
        if (typeIndex == UINT32_MAX) {
            destroyImage(device, outImage, nullptr);
            outImage = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = req.size;
        allocInfo.memoryTypeIndex = typeIndex;
        if (allocMem(device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS) {
            destroyImage(device, outImage, nullptr);
            outImage = VK_NULL_HANDLE;
            return false;
        }
        if (bindMem(device, outImage, outMemory, 0) != VK_SUCCESS ||
            !CreateImageViewOnly(device, gdpa, outImage, format, outView))
        {
            freeMem(device, outMemory, nullptr);
            destroyImage(device, outImage, nullptr);
            outMemory = VK_NULL_HANDLE;
            outImage = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    static bool InitializeMotionRingLocked(
        DeviceEntry &dev,
        const VkPhysicalDeviceMemoryProperties &memProps,
        const std::vector<uint32_t> &spirv)
    {
        if (!dev.device || !dev.gdpa || spirv.size() < 5 || spirv[0] != 0x07230203u)
            return false;

        auto createSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(dev.gdpa(dev.device, "vkCreateDescriptorSetLayout"));
        auto createPipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(dev.gdpa(dev.device, "vkCreatePipelineLayout"));
        auto createShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(dev.gdpa(dev.device, "vkCreateShaderModule"));
        auto destroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(dev.gdpa(dev.device, "vkDestroyShaderModule"));
        auto createComputePipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(dev.gdpa(dev.device, "vkCreateComputePipelines"));
        auto createDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(dev.gdpa(dev.device, "vkCreateDescriptorPool"));
        auto allocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(dev.gdpa(dev.device, "vkAllocateDescriptorSets"));
        auto updateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(dev.gdpa(dev.device, "vkUpdateDescriptorSets"));
        auto createBuffer = reinterpret_cast<PFN_vkCreateBuffer>(dev.gdpa(dev.device, "vkCreateBuffer"));
        auto getBufferMemReq = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(dev.gdpa(dev.device, "vkGetBufferMemoryRequirements"));
        auto allocMem = reinterpret_cast<PFN_vkAllocateMemory>(dev.gdpa(dev.device, "vkAllocateMemory"));
        auto bindBufferMem = reinterpret_cast<PFN_vkBindBufferMemory>(dev.gdpa(dev.device, "vkBindBufferMemory"));
        auto mapMem = reinterpret_cast<PFN_vkMapMemory>(dev.gdpa(dev.device, "vkMapMemory"));
        if (!createSetLayout || !createPipelineLayout || !createShaderModule || !destroyShaderModule ||
            !createComputePipelines || !createDescriptorPool || !allocateDescriptorSets || !updateDescriptorSets ||
            !createBuffer || !getBufferMemReq || !allocMem || !bindBufferMem || !mapMem)
            return false;

        VkDescriptorSetLayoutBinding bindings[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = (i == 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setLayoutInfo.bindingCount = 5;
        setLayoutInfo.pBindings = bindings;
        if (createSetLayout(dev.device, &setLayoutInfo, nullptr, &dev.motionDescriptorSetLayout) != VK_SUCCESS)
            return false;

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &dev.motionDescriptorSetLayout;
        if (createPipelineLayout(dev.device, &layoutInfo, nullptr, &dev.motionPipelineLayout) != VK_SUCCESS)
            return false;

        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = spirv.size() * sizeof(uint32_t);
        moduleInfo.pCode = spirv.data();
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (createShaderModule(dev.device, &moduleInfo, nullptr, &shaderModule) != VK_SUCCESS)
            return false;

        VkComputePipelineCreateInfo computeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        computeInfo.layout = dev.motionPipelineLayout;
        computeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeInfo.stage.module = shaderModule;
        computeInfo.stage.pName = "main";
        const VkResult pipeRes = createComputePipelines(dev.device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &dev.motionComputePipeline);
        destroyShaderModule(dev.device, shaderModule, nullptr);
        if (pipeRes != VK_SUCCESS)
            return false;

        const uint32_t slotCount = static_cast<uint32_t>(dev.motionSlots.size());
        const VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4u * slotCount},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, slotCount}
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = slotCount;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        if (createDescriptorPool(dev.device, &poolInfo, nullptr, &dev.motionDescriptorPool) != VK_SUCCESS)
            return false;

        const VkImageView storageViews[4] = {
            dev.currentDepthStorageView,
            dev.previousDepthStorageView,
            dev.motionStorageView,
            dev.confidenceStorageView
        };

        for (auto &slot : dev.motionSlots) {
            VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufInfo.size = sizeof(asteria_camera_motion::Parameters);
            bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            if (createBuffer(dev.device, &bufInfo, nullptr, &slot.uniformBuffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements req{};
            getBufferMemReq(dev.device, slot.uniformBuffer, &req);
            uint32_t memType = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if ((req.memoryTypeBits & (1u << i)) &&
                    (memProps.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                {
                    memType = i;
                    break;
                }
            }
            if (memType == UINT32_MAX)
                return false;

            VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocInfo.allocationSize = req.size;
            allocInfo.memoryTypeIndex = memType;
            if (allocMem(dev.device, &allocInfo, nullptr, &slot.uniformMemory) != VK_SUCCESS ||
                bindBufferMem(dev.device, slot.uniformBuffer, slot.uniformMemory, 0) != VK_SUCCESS ||
                mapMem(dev.device, slot.uniformMemory, 0, sizeof(asteria_camera_motion::Parameters), 0, &slot.mappedUniform) != VK_SUCCESS)
                return false;

            VkDescriptorSetAllocateInfo allocSetInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocSetInfo.descriptorPool = dev.motionDescriptorPool;
            allocSetInfo.descriptorSetCount = 1;
            allocSetInfo.pSetLayouts = &dev.motionDescriptorSetLayout;
            if (allocateDescriptorSets(dev.device, &allocSetInfo, &slot.descriptorSet) != VK_SUCCESS)
                return false;

            VkDescriptorImageInfo imageInfos[4]{};
            VkWriteDescriptorSet writes[5]{};
            for (uint32_t i = 0; i < 4; ++i) {
                imageInfos[i].imageView = storageViews[i];
                imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = slot.descriptorSet;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i].pImageInfo = &imageInfos[i];
            }
            VkDescriptorBufferInfo bufDesc{slot.uniformBuffer, 0, sizeof(asteria_camera_motion::Parameters)};
            writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[4].dstSet = slot.descriptorSet;
            writes[4].dstBinding = 4;
            writes[4].descriptorCount = 1;
            writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[4].pBufferInfo = &bufDesc;
            updateDescriptorSets(dev.device, 5, writes, 0, nullptr);
        }
        return true;
    }

    void DestroyDeviceGpuResourcesLocked(DeviceEntry &dev) {
        if (dev.device && dev.gdpa) {
            auto destroyView = reinterpret_cast<PFN_vkDestroyImageView>(dev.gdpa(dev.device, "vkDestroyImageView"));
            auto destroyImage = reinterpret_cast<PFN_vkDestroyImage>(dev.gdpa(dev.device, "vkDestroyImage"));
            auto freeMem = reinterpret_cast<PFN_vkFreeMemory>(dev.gdpa(dev.device, "vkFreeMemory"));
            auto unmapMem = reinterpret_cast<PFN_vkUnmapMemory>(dev.gdpa(dev.device, "vkUnmapMemory"));
            auto destroyBuf = reinterpret_cast<PFN_vkDestroyBuffer>(dev.gdpa(dev.device, "vkDestroyBuffer"));
            auto destroyPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(dev.gdpa(dev.device, "vkDestroyDescriptorPool"));
            auto destroyPipe = reinterpret_cast<PFN_vkDestroyPipeline>(dev.gdpa(dev.device, "vkDestroyPipeline"));
            auto destroyPipeLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(dev.gdpa(dev.device, "vkDestroyPipelineLayout"));
            auto destroySetLayout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(dev.gdpa(dev.device, "vkDestroyDescriptorSetLayout"));

            for (auto &slot : dev.motionSlots) {
                if (slot.mappedUniform && slot.uniformMemory && unmapMem)
                    unmapMem(dev.device, slot.uniformMemory);
                if (slot.uniformBuffer && destroyBuf)
                    destroyBuf(dev.device, slot.uniformBuffer, nullptr);
                if (slot.uniformMemory && freeMem)
                    freeMem(dev.device, slot.uniformMemory, nullptr);
                slot = {};
            }
            if (dev.motionDescriptorPool && destroyPool)
                destroyPool(dev.device, dev.motionDescriptorPool, nullptr);
            if (dev.motionComputePipeline && destroyPipe)
                destroyPipe(dev.device, dev.motionComputePipeline, nullptr);
            if (dev.motionPipelineLayout && destroyPipeLayout)
                destroyPipeLayout(dev.device, dev.motionPipelineLayout, nullptr);
            if (dev.motionDescriptorSetLayout && destroySetLayout)
                destroySetLayout(dev.device, dev.motionDescriptorSetLayout, nullptr);

            if (destroyView) {
                if (dev.currentDepthStorageView) destroyView(dev.device, dev.currentDepthStorageView, nullptr);
                if (dev.motionStorageView) destroyView(dev.device, dev.motionStorageView, nullptr);
                if (dev.previousDepthStorageView) destroyView(dev.device, dev.previousDepthStorageView, nullptr);
                if (dev.confidenceStorageView) destroyView(dev.device, dev.confidenceStorageView, nullptr);
            }
            if (destroyImage) {
                if (dev.previousDepthImage) destroyImage(dev.device, dev.previousDepthImage, nullptr);
                if (dev.confidenceImage) destroyImage(dev.device, dev.confidenceImage, nullptr);
            }
            if (freeMem) {
                if (dev.previousDepthMemory) freeMem(dev.device, dev.previousDepthMemory, nullptr);
                if (dev.confidenceMemory) freeMem(dev.device, dev.confidenceMemory, nullptr);
            }
        }
        dev.motionDescriptorPool = VK_NULL_HANDLE;
        dev.motionComputePipeline = VK_NULL_HANDLE;
        dev.motionPipelineLayout = VK_NULL_HANDLE;
        dev.motionDescriptorSetLayout = VK_NULL_HANDLE;
        dev.currentDepthStorageView = VK_NULL_HANDLE;
        dev.motionStorageView = VK_NULL_HANDLE;
        dev.previousDepthStorageView = VK_NULL_HANDLE;
        dev.previousDepthImage = VK_NULL_HANDLE;
        dev.previousDepthMemory = VK_NULL_HANDLE;
        dev.confidenceStorageView = VK_NULL_HANDLE;
        dev.confidenceImage = VK_NULL_HANDLE;
        dev.confidenceMemory = VK_NULL_HANDLE;
        dev.previousDepthInitialized = false;
        dev.motionReady = false;
        if (dev.depthReady) {
            dev.depthCapture.ReleaseAfterGpuIdle();
            dev.depthReady = false;
        }
        dev.initializedWidth = 0;
        dev.initializedHeight = 0;
        dev.boundInternalDepth = VK_NULL_HANDLE;
        dev.boundReconstructedMotion = VK_NULL_HANDLE;
    }

    mutable std::mutex mutex_;
    std::unordered_map<uintptr_t, DeviceEntry> devices_;
    std::unordered_map<uintptr_t, CommandState> commands_;
    std::vector<uint32_t> resolveSpirv_;
    std::vector<uint32_t> reconstructSpirv_;
    Diagnostics diagnostics_{};
};

inline Runtime &GlobalRuntime() {
    static Runtime instance;
    return instance;
}

} // namespace asteria_live_scene
