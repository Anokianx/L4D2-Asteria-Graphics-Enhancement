#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace asteria_camera_motion
{
inline constexpr const char *NativeViewportAuthor = "Anokianx";
inline constexpr const char *NativeViewportConvention = "explicit_Vulkan_pixel_center_to_NDC_zero_to_one_no_profile_inference";

enum class ViewportMode : uint32_t
{
    DefaultTopLeft = 0,
    NativeVulkanZeroToOne = 1
};

struct NativeViewport
{
    std::array<float, 4> rectangle{};
    std::array<float, 2> depthRange{0, 1};
    bool known = false;
};

inline bool ValidNativeViewport(const NativeViewport &viewport)
{
    if (!viewport.known) return false;
    for (float value : viewport.rectangle) if (!std::isfinite(value)) return false;
    for (float value : viewport.depthRange) if (!std::isfinite(value) || value < 0 || value > 1) return false;
    return viewport.rectangle[2] > 0 && viewport.rectangle[3] != 0 &&
           std::isfinite(viewport.rectangle[0] + viewport.rectangle[2]) &&
           std::isfinite(viewport.rectangle[1] + viewport.rectangle[3]) &&
           viewport.depthRange[0] != viewport.depthRange[1] &&
           std::isfinite(2.0f / viewport.rectangle[2]) && std::isfinite(2.0f / viewport.rectangle[3]) &&
           std::isfinite(1.0f / (viewport.depthRange[1] - viewport.depthRange[0]));
}

inline bool NativePixelToNdc(const NativeViewport &viewport, const std::array<double, 3> &pixel, std::array<double, 3> &ndc)
{
    if (!ValidNativeViewport(viewport)) return false;
    for (double value : pixel) if (!std::isfinite(value)) return false;
    ndc = {2.0 * (pixel[0] - viewport.rectangle[0]) / viewport.rectangle[2] - 1,
           2.0 * (pixel[1] - viewport.rectangle[1]) / viewport.rectangle[3] - 1,
           (pixel[2] - viewport.depthRange[0]) / (viewport.depthRange[1] - viewport.depthRange[0])};
    return std::isfinite(ndc[0]) && std::isfinite(ndc[1]) && std::isfinite(ndc[2]);
}

inline bool NativeNdcToPixel(const NativeViewport &viewport, const std::array<double, 3> &ndc, std::array<double, 3> &pixel)
{
    if (!ValidNativeViewport(viewport)) return false;
    for (double value : ndc) if (!std::isfinite(value)) return false;
    pixel = {viewport.rectangle[0] + viewport.rectangle[2] * (ndc[0] + 1) * 0.5,
             viewport.rectangle[1] + viewport.rectangle[3] * (ndc[1] + 1) * 0.5,
             viewport.depthRange[0] + (viewport.depthRange[1] - viewport.depthRange[0]) * ndc[2]};
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]) && std::isfinite(pixel[2]);
}
}
