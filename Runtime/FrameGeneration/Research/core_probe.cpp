#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <iostream>
#include <memory>

int main()
{
    static_assert(sizeof(void*) == 4);
    auto frame_context = std::make_unique<FfxFrameInterpolationContext>();
    auto flow_context = std::make_unique<FfxOpticalflowContext>();
    FfxFrameInterpolationContextDescription frame_description{};
    FfxOpticalflowContextDescription flow_description{};
    const auto frame_result = ffxFrameInterpolationContextCreate(frame_context.get(), &frame_description);
    const auto flow_result = ffxOpticalflowContextCreate(flow_context.get(), &flow_description);
    const auto frame_version = ffxFrameInterpolationGetEffectVersion();
    const auto flow_version = ffxOpticalflowGetEffectVersion();
    std::cout << "{\"architecture_bits\":" << sizeof(void*) * 8
              << ",\"frameinterpolation_version\":" << frame_version
              << ",\"opticalflow_version\":" << flow_version
              << ",\"frameinterpolation_without_backend_result\":" << frame_result
              << ",\"opticalflow_without_backend_result\":" << flow_result
              << ",\"gpu_context_created\":false,\"generated_frames\":0}" << std::endl;
    return frame_result == FFX_ERROR_INCOMPLETE_INTERFACE && flow_result == FFX_ERROR_INCOMPLETE_INTERFACE ? 0 : 1;
}
