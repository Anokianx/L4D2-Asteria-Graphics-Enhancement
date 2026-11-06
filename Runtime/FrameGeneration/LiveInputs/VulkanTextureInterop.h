#pragma once
#include <reshade.hpp>
#include <cstdint>

namespace asteria_live_inputs
{
struct VulkanTextureIdentity
{
    uint64_t device = 0, queue = 0, image = 0;
    uint32_t width = 0, height = 0;
    reshade::api::format format = reshade::api::format::unknown;
    reshade::api::memory_heap requested_heap = reshade::api::memory_heap::unknown;
    reshade::api::memory_heap queried_heap = reshade::api::memory_heap::unknown;
    bool queried_heap_known = false;
};

inline bool DescribeVulkanTexture(reshade::api::device *device, reshade::api::command_queue *queue,
    reshade::api::resource texture, const reshade::api::resource_desc &requested,
    reshade::api::format format, uint32_t width, uint32_t height, VulkanTextureIdentity &output)
{
    namespace api=reshade::api;
    output={};
    if (!device || !queue || !texture.handle || device->get_api()!=api::device_api::vulkan || queue->get_device()!=device) return false;
    const auto queried=device->get_resource_desc(texture);
    const auto matches_shape=[&](const api::resource_desc &description) {
        return description.type==api::resource_type::texture_2d && description.texture.width==width && description.texture.height==height &&
            description.texture.levels==1 && description.texture.depth_or_layers==1 && description.texture.samples==1 &&
            description.texture.format==format && (description.usage&api::resource_usage::shader_resource)==api::resource_usage::shader_resource;
    };
    if (!width || !height || requested.heap!=api::memory_heap::gpu_only || !matches_shape(requested) || !matches_shape(queried) ||
        (queried.heap!=api::memory_heap::unknown && queried.heap!=api::memory_heap::gpu_only)) return false;
    const uint64_t native_device=device->get_native(), native_queue=queue->get_native();
    if (!native_device || !native_queue) return false;
    output.device=native_device;output.queue=native_queue;output.image=texture.handle;output.width=width;output.height=height;output.format=format;
    output.requested_heap=requested.heap;output.queried_heap=queried.heap;output.queried_heap_known=queried.heap!=api::memory_heap::unknown;
    return true;
}
}
