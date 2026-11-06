#pragma once
#include <vulkan/vulkan.h>
#include <json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace asteria_graphics_pipeline_diagnostic
{
using Json = nlohmann::json;
inline constexpr const char *Author = "Anokianx";
inline constexpr uint32_t MaximumStages = 8;
inline constexpr uint32_t MaximumChainNodes = 8;
inline constexpr uint32_t MaximumLibraries = 4;
inline constexpr uint32_t MaximumDynamicStates = 128;
inline constexpr uint32_t MaximumColorFormats = 8;

template <typename T> inline uint64_t Handle(T value)
{
    if constexpr (std::is_pointer_v<T>) return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
    else return static_cast<uint64_t>(value);
}

inline Json FiniteFloat(float value)
{
    return std::isfinite(value) ? Json(value) : Json(nullptr);
}

inline Json DescribeChain(const void *head)
{
    Json result = {{"types", Json::array()}, {"nodes", Json::array()}, {"truncated", false}};
    auto entry = static_cast<const VkBaseInStructure *>(head);
    for (uint32_t index = 0; entry && index < MaximumChainNodes; ++index)
    {
        Json node = {{"sType", static_cast<uint32_t>(entry->sType)}};
        switch (entry->sType)
        {
        case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO:
        {
            const auto &value = *reinterpret_cast<const VkShaderModuleCreateInfo *>(entry);
            const bool aligned = value.pCode && reinterpret_cast<uintptr_t>(value.pCode) % alignof(uint32_t) == 0;
            const bool readableWord = aligned && value.codeSize >= sizeof(uint32_t) && value.codeSize % sizeof(uint32_t) == 0;
            node["flags"] = value.flags;
            node["code_size"] = static_cast<uint64_t>(value.codeSize);
            node["code_present"] = value.pCode != nullptr;
            node["code_aligned"] = aligned;
            node["first_word_read"] = readableWord;
            node["first_word"] = nullptr;
            if (readableWord)
            {
                uint32_t first = 0;
                std::memcpy(&first, value.pCode, sizeof(first));
                node["first_word"] = first;
            }
            break;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT:
        {
            const auto &value = *reinterpret_cast<const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *>(entry);
            node["identifier_size"] = value.identifierSize;
            node["identifier_present"] = value.pIdentifier != nullptr;
            break;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
        {
            const auto &value = *reinterpret_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT *>(entry);
            node["flags"] = value.flags;
            node["depth_clip_enable"] = value.depthClipEnable;
            break;
        }
        case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
        {
            const auto &value = *reinterpret_cast<const VkGraphicsPipelineLibraryCreateInfoEXT *>(entry);
            node["subset_flags"] = value.flags;
            break;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
        {
            const auto &value = *reinterpret_cast<const VkPipelineLibraryCreateInfoKHR *>(entry);
            node["library_count"] = value.libraryCount;
            node["libraries_present"] = value.pLibraries != nullptr;
            node["libraries"] = Json::array();
            if (value.pLibraries)
                for (uint32_t item = 0; item < (std::min)(value.libraryCount, MaximumLibraries); ++item)
                    node["libraries"].push_back(Handle(value.pLibraries[item]));
            node["libraries_truncated"] = value.libraryCount > MaximumLibraries || (value.libraryCount && !value.pLibraries);
            break;
        }
        case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO:
        {
            const auto &value = *reinterpret_cast<const VkPipelineRenderingCreateInfo *>(entry);
            node["view_mask"] = value.viewMask;
            node["color_attachment_count"] = value.colorAttachmentCount;
            node["color_formats_present"] = value.pColorAttachmentFormats != nullptr;
            node["color_formats"] = Json::array();
            if (value.pColorAttachmentFormats)
                for (uint32_t item = 0; item < (std::min)(value.colorAttachmentCount, MaximumColorFormats); ++item)
                    node["color_formats"].push_back(static_cast<uint32_t>(value.pColorAttachmentFormats[item]));
            node["color_formats_truncated"] = value.colorAttachmentCount > MaximumColorFormats || (value.colorAttachmentCount && !value.pColorAttachmentFormats);
            node["depth_format"] = static_cast<uint32_t>(value.depthAttachmentFormat);
            node["stencil_format"] = static_cast<uint32_t>(value.stencilAttachmentFormat);
            break;
        }
        default:
            break;
        }
        result["types"].push_back(static_cast<uint32_t>(entry->sType));
        result["nodes"].push_back(std::move(node));
        entry = entry->pNext;
    }
    result["truncated"] = entry != nullptr;
    return result;
}

template <typename T> inline Json DescribeState(const T *value)
{
    if (!value) return {{"present", false}};
    return {{"present", true}, {"sType", static_cast<uint32_t>(value->sType)}, {"flags", value->flags},
            {"pnext", DescribeChain(value->pNext)}};
}

inline Json DescribeGraphicsPipelineCreateInfo(const VkGraphicsPipelineCreateInfo &value)
{
    Json result = {{"author", Author}, {"scope", "raw_native_create_info_only"}, {"changes_qualification", false},
        {"sType", static_cast<uint32_t>(value.sType)}, {"flags", value.flags}, {"layout", Handle(value.layout)},
        {"render_pass", Handle(value.renderPass)}, {"subpass", value.subpass},
        {"base_pipeline_handle", Handle(value.basePipelineHandle)}, {"base_pipeline_index", value.basePipelineIndex},
        {"pnext", DescribeChain(value.pNext)}, {"stage_count", value.stageCount}, {"stages_present", value.pStages != nullptr},
        {"stages", Json::array()}, {"stages_truncated", value.stageCount > MaximumStages || (value.stageCount && !value.pStages)}};
    if (value.pStages)
        for (uint32_t index = 0; index < (std::min)(value.stageCount, MaximumStages); ++index)
        {
            const auto &stage = value.pStages[index];
            result["stages"].push_back({{"sType", static_cast<uint32_t>(stage.sType)}, {"flags", stage.flags},
                {"stage", static_cast<uint32_t>(stage.stage)}, {"module", Handle(stage.module)},
                {"entry_point_present", stage.pName != nullptr}, {"pnext", DescribeChain(stage.pNext)}});
        }
    result["vertex_input_state"] = DescribeState(value.pVertexInputState);
    result["input_assembly_state"] = DescribeState(value.pInputAssemblyState);
    result["tessellation_state"] = DescribeState(value.pTessellationState);
    result["viewport_state"] = DescribeState(value.pViewportState);
    result["rasterization_state"] = DescribeState(value.pRasterizationState);
    result["multisample_state"] = DescribeState(value.pMultisampleState);
    result["depth_stencil_state"] = DescribeState(value.pDepthStencilState);
    result["color_blend_state"] = DescribeState(value.pColorBlendState);
    result["dynamic_state"] = DescribeState(value.pDynamicState);
    if (value.pViewportState)
    {
        const auto &state = *value.pViewportState;
        auto &shape = result["viewport_state"];
        shape["viewport_count"] = state.viewportCount;
        shape["viewports_present"] = state.pViewports != nullptr;
        shape["viewports"] = Json::array();
        shape["viewports_truncated"] = state.viewportCount > 1 || (state.viewportCount && !state.pViewports);
        if (state.viewportCount && state.pViewports)
        {
            const auto &viewport = state.pViewports[0];
            shape["viewports"].push_back({FiniteFloat(viewport.x), FiniteFloat(viewport.y), FiniteFloat(viewport.width),
                FiniteFloat(viewport.height), FiniteFloat(viewport.minDepth), FiniteFloat(viewport.maxDepth)});
            shape["first_viewport_finite"] = std::isfinite(viewport.x) && std::isfinite(viewport.y) && std::isfinite(viewport.width) &&
                std::isfinite(viewport.height) && std::isfinite(viewport.minDepth) && std::isfinite(viewport.maxDepth);
        }
        shape["scissor_count"] = state.scissorCount;
        shape["scissors_present"] = state.pScissors != nullptr;
        shape["scissors"] = Json::array();
        shape["scissors_truncated"] = state.scissorCount > 1 || (state.scissorCount && !state.pScissors);
        if (state.scissorCount && state.pScissors)
        {
            const auto &scissor = state.pScissors[0];
            shape["scissors"].push_back({scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height});
        }
    }
    if (value.pDynamicState)
    {
        const auto &state = *value.pDynamicState;
        auto &shape = result["dynamic_state"];
        shape["dynamic_state_count"] = state.dynamicStateCount;
        shape["states_present"] = state.pDynamicStates != nullptr;
        shape["states"] = Json::array();
        if (state.pDynamicStates)
            for (uint32_t index = 0; index < (std::min)(state.dynamicStateCount, MaximumDynamicStates); ++index)
                shape["states"].push_back(static_cast<uint32_t>(state.pDynamicStates[index]));
        shape["states_truncated"] = state.dynamicStateCount > MaximumDynamicStates || (state.dynamicStateCount && !state.pDynamicStates);
    }
    if (value.pRasterizationState)
    {
        const auto &state = *value.pRasterizationState;
        auto &shape = result["rasterization_state"];
        shape["depth_clamp_enable"] = state.depthClampEnable;
        shape["rasterizer_discard_enable"] = state.rasterizerDiscardEnable;
        shape["polygon_mode"] = static_cast<uint32_t>(state.polygonMode);
        shape["cull_mode"] = state.cullMode;
        shape["front_face"] = static_cast<uint32_t>(state.frontFace);
        shape["depth_bias_enable"] = state.depthBiasEnable;
        shape["line_width"] = FiniteFloat(state.lineWidth);
    }
    return result;
}
}
