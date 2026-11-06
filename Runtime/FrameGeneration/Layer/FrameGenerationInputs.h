#pragma once
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>

namespace asteria_fg_runtime
{
enum class MotionInputSource : uint32_t
{
    Unavailable = 0,
    Engine = 1,
    Estimated = 2
};

struct FrameInputs
{
    FfxResource depth{};
    FfxResource motion{};
    uint64_t device = 0;
    uint64_t frameID = 0;
    uint32_t width = 0, height = 0;
    float deltaMilliseconds = 0;
    float nearPlane = 0, farPlane = 0, verticalFovRadians = 0;
    float metersPerUnit = 0;
    float position[3]{}, up[3]{}, right[3]{}, forward[3]{};
    FfxFloatCoords2D motionScale{};
    MotionInputSource motionSource = MotionInputSource::Unavailable;
    bool sceneDepth = false;
    bool synchronized = false;
    bool historyReset = false;
    bool hudCompositionVerified = false;
    std::shared_ptr<void> lifetime;
};

inline bool ValidFrameInputs(const FrameInputs &input, uint64_t device, uint64_t frameID,
                             uint32_t width, uint32_t height)
{
    if (!device || input.device != device || input.frameID != frameID || !width || !height ||
        input.width != width || input.height != height || !input.sceneDepth || !input.synchronized ||
        !input.hudCompositionVerified || !input.lifetime || !input.depth.resource || !input.motion.resource ||
        (input.motionSource != MotionInputSource::Engine && input.motionSource != MotionInputSource::Estimated)) return false;
    for (const FfxResource *resource : {&input.depth, &input.motion})
        if (resource->description.type != FFX_RESOURCE_TYPE_TEXTURE2D || resource->description.width != width ||
            resource->description.height != height || resource->description.depth != 1 || resource->description.mipCount != 1 ||
            (resource->state != FFX_RESOURCE_STATE_COMPUTE_READ && resource->state != FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ)) return false;
    if (input.depth.description.format != FFX_SURFACE_FORMAT_R32_FLOAT ||
        (input.motion.description.format != FFX_SURFACE_FORMAT_R16G16_FLOAT &&
         input.motion.description.format != FFX_SURFACE_FORMAT_R32G32_FLOAT)) return false;
    if (!std::isfinite(input.deltaMilliseconds) || input.deltaMilliseconds <= 0 || input.deltaMilliseconds > 1000 ||
        !std::isfinite(input.nearPlane) || !std::isfinite(input.farPlane) || input.nearPlane <= 0 || input.farPlane <= input.nearPlane ||
        !std::isfinite(input.verticalFovRadians) || input.verticalFovRadians <= 0 || input.verticalFovRadians >= 3.14159265f ||
        !std::isfinite(input.metersPerUnit) || input.metersPerUnit <= 0 ||
        !std::isfinite(input.motionScale.x) || !std::isfinite(input.motionScale.y) || input.motionScale.x <= 0 || input.motionScale.y <= 0) return false;
    for (uint32_t index = 0; index < 3; ++index)
        if (!std::isfinite(input.position[index]) || !std::isfinite(input.up[index]) ||
            !std::isfinite(input.right[index]) || !std::isfinite(input.forward[index])) return false;
    auto dot = [](const float *a, const float *b) { return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]; };
    return std::abs(dot(input.up, input.up) - 1) < 0.001 && std::abs(dot(input.right, input.right) - 1) < 0.001 &&
           std::abs(dot(input.forward, input.forward) - 1) < 0.001 && std::abs(dot(input.up, input.right)) < 0.001 &&
           std::abs(dot(input.up, input.forward)) < 0.001 && std::abs(dot(input.right, input.forward)) < 0.001;
}

class FrameInputSession
{
public:
    virtual ~FrameInputSession() = default;
    virtual std::shared_ptr<const FrameInputs> Acquire(uint64_t frameID) = 0;
    virtual void RetireAfterGpuIdle() = 0;
    virtual void BindSwapchain(uint64_t) {}
    virtual void BindNativeSwapchain(uint64_t) {}
    virtual void RecordAcquire(uint32_t) {}
    virtual void BeginPresent(uint32_t) {}
};

class FrameInputProvider
{
public:
    virtual ~FrameInputProvider() = default;
    virtual std::shared_ptr<FrameInputSession> Open(uint64_t device, uint32_t width, uint32_t height) = 0;
};

class EstimatedFrameInputSession final : public FrameInputSession
{
public:
    std::shared_ptr<const FrameInputs> Acquire(uint64_t) override { return nullptr; }
    void RetireAfterGpuIdle() override {}
};

class EstimatedFrameInputProvider final : public FrameInputProvider
{
public:
    std::shared_ptr<FrameInputSession> Open(uint64_t device, uint32_t width, uint32_t height) override
    {
        if (!device || !width || !height) return nullptr;
        return std::make_shared<EstimatedFrameInputSession>();
    }
};

inline std::shared_ptr<FrameInputProvider> EstimatedProvider()
{
    static const auto provider = std::make_shared<EstimatedFrameInputProvider>();
    return provider;
}

inline std::shared_ptr<FrameInputProvider> InputProvider;

inline std::shared_ptr<FrameInputProvider> CurrentFrameInputProvider()
{
    return std::atomic_load_explicit(&InputProvider, std::memory_order_acquire);
}

template<typename WaitForCompletion>
inline bool RetireCurrentFrameInputs(std::shared_ptr<const FrameInputs> &current, WaitForCompletion &&wait)
{
    const auto retained = std::atomic_load_explicit(&current, std::memory_order_acquire);
    if (!retained) return true;
    if (!wait()) return false;
    std::atomic_store_explicit(&current, std::shared_ptr<const FrameInputs>{}, std::memory_order_release);
    return true;
}
}
