#pragma once
#include <windows.h>
#include <reshade.hpp>
#include <json.hpp>
#include "PipelineProvenance.h"
#include "../Layer/AsteriaDescriptorEvidenceApi.h"
#include "../Layer/AsteriaViewportEvidenceApi.h"
#include "../Layer/AsteriaDrawViewportEvidenceApi.h"
#include "../Layer/AsteriaDrawViewportDiagnosticApi.h"
#include "../FrameAssociation/LiveObservedAssociation.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace asteria_native_uniform
{
namespace api = reshade::api;
using Json = nlohmann::json;
using CopyMappedBytes = BOOL (__stdcall *)(uint64_t, uint64_t, uint64_t, uint32_t, void *);
using QueryBoundDescriptor = AsteriaQueryBoundBufferDescriptorFn;
constexpr size_t MaxDevices = 8, MaxTables = 4096, MaxCommands = 256, MaxSlots = 128, MaxLayouts = 1024;
constexpr size_t MaxParameters = 64, MaxRanges = 64, MaxBuffers = 4096, MaxSamples = 2048, SamplesPerFrame = 64;
constexpr size_t MaxPresentationComparisons = 64;
constexpr uint64_t RetainedFrames = 64, UnreferencedTableFrames = 2, ProfileByteOffset = 384, ProfileBytes = 64;
constexpr uint64_t ModelMatrixByteOffset = 1184, ModelMatrixBytes = 48;
constexpr uint32_t VertexStage = static_cast<uint32_t>(api::shader_stage::vertex);

inline std::string Hex(uint64_t value)
{
    char text[19]{};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "0x%016llX", static_cast<unsigned long long>(value));
    return text;
}

struct Target
{
    uint64_t handle = 0;
    api::resource_desc desc{};
};

struct DrawContext
{
    uint64_t owner = 0, native_device = 0, frame = 0;
    api::device_api device_api = api::device_api::d3d9;
    uint32_t render_width = 0, render_height = 0;
    Target depth;
    std::vector<Target> targets;
    std::array<uint64_t, 9> pipeline_record_ids{};
    bool pipeline_provenance_enabled = false;
    uint64_t native_command = 0, draw_ordinal = 0, recording_sequence = 0, recording_tick_nanoseconds = 0;
    bool viewport_known = false;
    bool attachments_complete = true;
    std::array<float, 6> viewport{};
};

struct Descriptor
{
    api::buffer_range range{};
};
using Slots = std::map<std::array<uint32_t, 2>, Descriptor>;

struct Table
{
    Slots slots;
    uint64_t touched_frame = 0;
};

struct Parameter
{
    bool known = false, constant_buffers = false;
};

struct TableBinding
{
    uint64_t table = 0, layout = 0;
};

struct Bindings
{
    std::map<uint32_t, TableBinding> tables;
    std::map<std::array<uint32_t, 3>, Descriptor> pushed;
    std::map<uint32_t, uint64_t> pushed_layouts;
};

struct Command
{
    Bindings current, saved;
    uint64_t vertex_pipeline = 0, saved_vertex_pipeline = 0;
    int32_t profile = -1, saved_profile = -1;
    uint32_t effects = 0;
};

struct Profile
{
    std::string sha256, entry_point;
    std::vector<uint8_t> bytes;
    uint32_t descriptor_set = 0, binding = 0;
};

struct Device
{
    api::device_api api = api::device_api::d3d9;
    uint64_t frame = 0, samples_this_frame = 0, total_samples = 0, candidate_draws = 0, skipped_frame_budget = 0;
    uint64_t limits = 0, missing_table_binds = 0, unknown_layout_binds = 0, unknown_copy_sources = 0;
    uint64_t pruned_tables = 0, ignored_short_ranges = 0, excluded_updates = 0;
    uint64_t pipeline_init_callbacks = 0, matching_pipeline_inits = 0, unknown_pipeline_binds = 0, unverified_draw_profiles = 0;
    std::map<uint64_t, Table> tables;
    std::map<uint64_t, std::vector<Parameter>> layouts;
    std::map<uint64_t, api::resource_desc> buffers;
    std::map<uint64_t, Command> commands;
    std::map<uint64_t, int32_t> pipelines;
    std::map<DWORD, uint32_t> effect_threads;
    std::set<std::array<uint64_t, 6>> sampled_ranges;
};

struct Request
{
    api::buffer_range range{};
    uint64_t table = 0, layout = 0;
    uint32_t parameter = 0, binding = 0, array_element = 0;
    api::resource_desc desc{};
    bool desc_known = false, pushed = false;
};

inline bool CopyBytesBridge(CopyMappedBytes copy, uint64_t device, uint64_t buffer, uint64_t offset, uint32_t byteCount, void *destination)
{
#if defined(_MSC_VER)
    __try { return copy(device, buffer, offset, byteCount, destination) != FALSE; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    return copy(device, buffer, offset, byteCount, destination) != FALSE;
#endif
}

inline bool CopyBridge(CopyMappedBytes copy, uint64_t device, uint64_t buffer, uint64_t offset, void *destination)
{
    return CopyBytesBridge(copy, device, buffer, offset, static_cast<uint32_t>(ProfileBytes), destination);
}

inline uint32_t QueryDescriptorBridge(
    QueryBoundDescriptor query, uint64_t device, uint64_t command, uint32_t parameter, uint32_t binding, uint32_t arrayElement,
    AsteriaBoundBufferDescriptorV1 *outDesc)
{
    if (!query || !outDesc) return ASTERIA_DESCRIPTOR_BAD_ABI;
    std::memset(outDesc, 0, sizeof(*outDesc));
    outDesc->struct_size = sizeof(*outDesc);
    outDesc->api_version = 1;
    outDesc->status = ASTERIA_DESCRIPTOR_BAD_ABI;
#if defined(_MSC_VER)
    __try {
        const BOOL accepted = query(device, command, parameter, binding, arrayElement, sizeof(*outDesc), outDesc);
        if ((!accepted && outDesc->status == ASTERIA_DESCRIPTOR_OK) || outDesc->struct_size != sizeof(*outDesc) || outDesc->api_version != 1)
            return ASTERIA_DESCRIPTOR_BAD_ABI;
        return outDesc->status;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return ASTERIA_DESCRIPTOR_BAD_ABI; }
#else
    const BOOL accepted = query(device, command, parameter, binding, arrayElement, sizeof(*outDesc), outDesc);
    if ((!accepted && outDesc->status == ASTERIA_DESCRIPTOR_OK) || outDesc->struct_size != sizeof(*outDesc) || outDesc->api_version != 1)
        return ASTERIA_DESCRIPTOR_BAD_ABI;
    return outDesc->status;
#endif
}

template <typename CommandList>
inline uint32_t QueryCommandDescriptorBridge(
    QueryBoundDescriptor query, uint64_t device, CommandList *list, uint32_t parameter, uint32_t binding, uint32_t arrayElement,
    AsteriaBoundBufferDescriptorV1 *outDesc)
{
    if (!list) return ASTERIA_DESCRIPTOR_UNKNOWN_COMMAND;
    return QueryDescriptorBridge(query, device, list->get_native(), parameter, binding, arrayElement, outDesc);
}

inline uint32_t QueryViewportBridge(AsteriaQueryNativeViewportV1Fn query, uint64_t device, uint64_t command,
                                   AsteriaNativeViewportEvidenceV1 *output)
{
    if (!query || !output) return ASTERIA_VIEWPORT_EVIDENCE_BAD_ABI;
    std::memset(output, 0, sizeof(*output)); output->struct_size = sizeof(*output); output->api_version = 1;
    output->status = ASTERIA_VIEWPORT_EVIDENCE_BAD_ABI;
#if defined(_MSC_VER)
    __try {
#endif
        const BOOL accepted = query(device, command, sizeof(*output), output);
        if (output->struct_size != sizeof(*output) || output->api_version != 1 ||
            (!accepted && output->status == ASTERIA_VIEWPORT_EVIDENCE_OK)) return ASTERIA_VIEWPORT_EVIDENCE_BAD_ABI;
        return output->status;
#if defined(_MSC_VER)
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return ASTERIA_VIEWPORT_EVIDENCE_BAD_ABI; }
#endif
}

inline uint32_t QueryDrawViewportBridge(AsteriaQueryDrawViewportV1Fn query, uint64_t device, uint64_t command,
                                       AsteriaDrawViewportEvidenceV1 *output)
{
    if (!query || !output) return UINT32_MAX;
    std::memset(output, 0, sizeof(*output)); output->struct_size=sizeof(*output); output->api_version=1; output->status=UINT32_MAX;
#if defined(_MSC_VER)
    __try {
#endif
        const BOOL accepted=query(device,command,sizeof(*output),output);
        if (output->struct_size!=sizeof(*output) || output->api_version!=1 || (!accepted && output->status==0)) return UINT32_MAX;
        return output->status;
#if defined(_MSC_VER)
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return UINT32_MAX; }
#endif
}
inline uint32_t QueryDrawViewportDiagnosticBridge(AsteriaQueryDrawViewportDiagnosticV1Fn query,uint64_t device,uint64_t command,
                                                AsteriaDrawViewportDiagnosticV1* output)
{
    if(!query||!output)return UINT32_MAX;
    std::memset(output,0,sizeof(*output));output->struct_size=sizeof(*output);output->api_version=1;output->status=UINT32_MAX;
#if defined(_MSC_VER)
    __try {
#endif
        const BOOL observed=query(device,command,sizeof(*output),output);
        if(output->struct_size!=sizeof(*output)||output->api_version!=1||(!observed&&output->status==0))return UINT32_MAX;
        return output->status;
#if defined(_MSC_VER)
    } __except(EXCEPTION_EXECUTE_HANDLER) { return UINT32_MAX; }
#endif
}
inline std::string PipelineDiagnosticRecordKey(uint64_t device,uint64_t deviceGeneration,uint64_t pipeline,uint64_t pipelineGeneration)
{
    return std::to_string(device)+":"+std::to_string(deviceGeneration)+":"+std::to_string(pipeline)+":"+std::to_string(pipelineGeneration);
}

inline const char *ValidateNativeDescriptor(const AsteriaBoundBufferDescriptorV1 &native, const Request &request, uint64_t &effectiveRange)
{
    effectiveRange = 0;
    if (native.struct_size != sizeof(native) || native.api_version != 1 || native.status != ASTERIA_DESCRIPTOR_OK)
        return "native_descriptor_abi_invalid";
    if ((native.stage_flags & 1u) == 0) return "native_descriptor_vertex_stage_missing";
    if (native.descriptor_type != 6 && native.descriptor_type != 8) return "native_descriptor_not_uniform_buffer";
    if (native.buffer != request.range.buffer.handle) return "native_descriptor_buffer_mismatch";
    if (!request.desc_known || request.desc.type != api::resource_type::buffer || native.buffer_size != request.desc.buffer.size)
        return "native_descriptor_buffer_size_mismatch";
    if (native.buffer_offset != request.range.offset) return "native_descriptor_base_offset_mismatch";
    if (native.buffer_offset > native.buffer_size || native.dynamic_offset > native.buffer_size - native.buffer_offset ||
        native.effective_offset != native.buffer_offset + native.dynamic_offset)
        return "native_descriptor_effective_offset_invalid";
    if (native.descriptor_type == 6 && (native.dynamic_offset != 0 || native.dynamic_offset_index != UINT32_MAX))
        return "native_descriptor_ordinary_offset_invalid";
    if (native.descriptor_type == 8 && (native.dynamic_offset_count == 0 || native.dynamic_offset_index >= native.dynamic_offset_count))
        return "native_descriptor_dynamic_offset_invalid";
    const uint64_t native_range = native.range == UINT64_MAX ? native.buffer_size - native.buffer_offset : native.range;
    const uint64_t api_range = request.range.size == UINT64_MAX ? native.buffer_size - native.buffer_offset : request.range.size;
    if (!native_range || native_range != api_range || native_range > native.buffer_size - native.effective_offset)
        return "native_descriptor_range_mismatch";
    if (native_range < ProfileByteOffset + ProfileBytes) return "native_descriptor_profile_out_of_range";
    effectiveRange = native_range;
    return nullptr;
}

class PresentationComparator
{
    static constexpr double AbsoluteTolerance = 1e-4, RelativeTolerance = 1e-5;
    std::deque<Json> observations;
    std::map<std::string, std::array<uint64_t, 3>> profiles;
    uint64_t compared = 0, matched = 0, mismatched = 0, unavailable = 0, evicted = 0;

    static bool Flag(const Json &value, const char *key)
    {
        if (!value.is_object()) return false;
        auto found = value.find(key);
        return found != value.end() && found->is_boolean() && found->get<bool>();
    }

    static bool Matrix(const Json &value, std::array<float, 16> &output)
    {
        if (!value.is_array()) return false;
        if (value.size() == 16)
        {
            for (size_t index = 0; index < 16; ++index)
            {
                if (!value[index].is_number()) return false;
                output[index] = value[index].get<float>();
                if (!std::isfinite(output[index])) return false;
            }
            return true;
        }
        if (value.size() != 4) return false;
        for (size_t row = 0; row < 4; ++row)
        {
            if (!value[row].is_array() || value[row].size() != 4) return false;
            for (size_t column = 0; column < 4; ++column)
            {
                if (!value[row][column].is_number()) return false;
                output[row * 4 + column] = value[row][column].get<float>();
                if (!std::isfinite(output[row * 4 + column])) return false;
            }
        }
        return true;
    }

    static Json Number(const Json &value, const char *key)
    {
        if (!value.is_object()) return nullptr;
        auto found = value.find(key);
        return found != value.end() && found->is_number() && std::isfinite(found->get<double>()) ? *found : Json(nullptr);
    }

    static uint64_t Integer(const Json &value, const char *key)
    {
        if (!value.is_object()) return 0;
        auto found = value.find(key);
        return found != value.end() && found->is_number_unsigned() ? found->get<uint64_t>() : 0;
    }

    void Compare(Json &sample, Json &observation)
    {
        if (sample.contains("native_getter_comparison")) return;
        ++observation["sample_count"].get_ref<uint64_t &>();
        Json comparison = {{"present_observer_frame", observation["present_observer_frame"]}, {"draw_observer_frame", observation["draw_observer_frame"]},
                           {"status", "unavailable"}, {"reason", "sample_or_getter_not_stable_finite"}, {"bitwise_equal", false}, {"within_tolerance", false},
                           {"max_absolute_error", nullptr}, {"max_relative_error", nullptr}, {"components_outside_tolerance", nullptr},
                           {"observer_interval_only", true}, {"camera_semantics_verified", false}, {"frame_timing_verified", false},
                           {"native_camera_ready", false}, {"motion_vectors_verified", false}};
        std::array<float, 16> raw{}, native{};
        const bool sample_valid = Flag(sample, "copy_stable") && Flag(sample, "finite") &&
            sample.contains("raw_register_floats") && Matrix(sample["raw_register_floats"], raw);
        const bool getter_valid = Flag(observation["native_getter"], "available") && Matrix(observation["native_getter"]["raw_world_to_screen"], native);
        if (sample_valid && getter_valid)
        {
            uint64_t failed = 0;
            double maximum_absolute = 0, maximum_relative = 0;
            for (size_t index = 0; index < 16; ++index)
            {
                const double difference = std::abs(double(raw[index]) - double(native[index]));
                const double magnitude = std::max(std::abs(double(raw[index])), std::abs(double(native[index])));
                maximum_absolute = std::max(maximum_absolute, difference);
                maximum_relative = std::max(maximum_relative, difference / std::max(magnitude, 1e-12));
                if (difference > AbsoluteTolerance + RelativeTolerance * magnitude) ++failed;
            }
            const bool equal = std::memcmp(raw.data(), native.data(), sizeof(raw)) == 0;
            const bool close = failed == 0;
            comparison["status"] = close ? "matched" : "mismatched";
            comparison["reason"] = "Direct row-major c8-c11 versus native getter; no transpose, fitting or view selection inferred";
            comparison["bitwise_equal"] = equal; comparison["within_tolerance"] = close;
            comparison["max_absolute_error"] = maximum_absolute; comparison["max_relative_error"] = maximum_relative;
            comparison["components_outside_tolerance"] = failed;
            ++observation["compared_samples"].get_ref<uint64_t &>(); ++compared;
            if (close) { ++observation["matched_samples"].get_ref<uint64_t &>(); ++matched; }
            else { ++observation["mismatched_samples"].get_ref<uint64_t &>(); ++mismatched; }
            if (sample.contains("profile_shader_sha256") && sample["profile_shader_sha256"].is_string())
            {
                const auto hash = sample["profile_shader_sha256"].get<std::string>();
                if (profiles.count(hash) || profiles.size() < 256) ++profiles[hash][close ? 0 : 1];
            }
        }
        else
        {
            ++observation["unavailable_samples"].get_ref<uint64_t &>(); ++unavailable;
            if (sample.contains("profile_shader_sha256") && sample["profile_shader_sha256"].is_string())
            {
                const auto hash = sample["profile_shader_sha256"].get<std::string>();
                if (profiles.count(hash) || profiles.size() < 256) ++profiles[hash][2];
            }
        }
        sample["native_getter_comparison"] = std::move(comparison);
    }

public:
    void ObserveSample(Json &sample)
    {
        const uint64_t owner = Integer(sample, "device_owner_u64"), frame = Integer(sample, "observer_frame");
        for (Json &observation : observations)
            if (Integer(observation, "device_owner_u64") == owner && Integer(observation, "draw_observer_frame") == frame)
            { Compare(sample, observation); return; }
    }

    void ObservePresent(uint64_t owner, uint64_t present_frame, const Json &camera, std::deque<Json> &samples)
    {
        if (!present_frame) return;
        for (const Json &existing : observations)
            if (Integer(existing, "device_owner_u64") == owner && Integer(existing, "present_observer_frame") == present_frame) return;
        std::array<float, 16> native{};
        const bool valid = Flag(camera, "available") && camera.contains("raw_world_to_screen") && Matrix(camera["raw_world_to_screen"], native);
        Json projection = camera.is_object() && camera.contains("native_projection_diagnostic") ? camera["native_projection_diagnostic"] : Json(nullptr);
        Json observation = {{"device_owner_u64", owner}, {"present_observer_frame", present_frame}, {"draw_observer_frame", present_frame - 1},
                            {"observer_interval_only", true}, {"camera_semantics_verified", false}, {"frame_timing_verified", false},
                            {"native_camera_ready", false}, {"motion_vectors_verified", false},
                            {"sample_count", uint64_t(0)}, {"compared_samples", uint64_t(0)}, {"matched_samples", uint64_t(0)},
                            {"mismatched_samples", uint64_t(0)}, {"unavailable_samples", uint64_t(0)},
                            {"native_getter", {{"available", valid}, {"raw_world_to_screen", valid ? Json(native) : Json(nullptr)},
                                               {"sample_tick_ms", Number(camera, "sample_tick_ms")}, {"near_plane", Number(projection, "near_plane")},
                                               {"far_plane", Number(projection, "far_plane")}, {"horizontal_fov_degrees", Number(projection, "horizontal_fov_degrees")},
                                               {"vertical_fov_degrees", Number(projection, "vertical_fov_degrees")}, {"projection_aspect_ratio", Number(projection, "projection_aspect_ratio")}}}};
        if (observations.size() >= MaxPresentationComparisons) { observations.pop_front(); ++evicted; }
        observations.push_back(std::move(observation));
        Json &current = observations.back();
        for (Json &sample : samples)
            if (Integer(sample, "device_owner_u64") == owner && Integer(sample, "observer_frame") == present_frame - 1) Compare(sample, current);
    }

    Json Snapshot() const
    {
        Json result = {{"semantics", "Draw observer interval k correlated only with following ReShade Present observer interval k+1; not GPU frame, main view, or motion proof"},
                       {"matrix_layout", "direct_row_major_no_transpose"}, {"absolute_tolerance", AbsoluteTolerance}, {"relative_tolerance", RelativeTolerance},
                       {"comparison_rule", "abs(a-b) <= absolute_tolerance + relative_tolerance * max(abs(a),abs(b)) for all 16 float components"},
                       {"observer_interval_only", true}, {"native_camera_ready", false}, {"camera_semantics_verified", false}, {"frame_timing_verified", false},
                       {"motion_vectors_verified", false}, {"maximum_retained_presentations", MaxPresentationComparisons}, {"presentations_evicted", evicted},
                       {"compared_samples", compared}, {"matched_samples", matched}, {"mismatched_samples", mismatched}, {"unavailable_samples", unavailable},
                       {"profiles", Json::array()}, {"presentations", observations}};
        for (const auto &profile : profiles)
            result["profiles"].push_back({{"shader_sha256", profile.first}, {"matched_samples", profile.second[0]},
                                          {"mismatched_samples", profile.second[1]}, {"unavailable_samples", profile.second[2]}});
        return result;
    }
};

class Sampler
{
    mutable std::mutex mutex;
    std::map<uint64_t, Device> devices;
    std::deque<Json> samples;
    std::vector<Json> first_successful_samples;
    std::vector<Profile> profiles;
    Json profile_manifest = {{"status", "unavailable"}, {"reason", "ASTERIA_NATIVE_UNIFORM_PROFILES is absent"}, {"verified_profiles", 0}};
    uint64_t device_limit = 0, sequence = 0, samples_evicted = 0;
    std::map<std::string, uint64_t> outcomes;
    PresentationComparator presentation_comparator;
    asteria_live_association::Bridge live_association;

    static uint64_t Owner(api::device *device) { return reinterpret_cast<uintptr_t>(device); }
    static uint64_t CommandHandle(api::command_list *list) { return reinterpret_cast<uintptr_t>(list); }

    Device *FindDevice(api::device *device)
    {
        const uint64_t owner = Owner(device);
        auto found = devices.find(owner);
        if (found != devices.end()) return &found->second;
        ++device_limit;
        return nullptr;
    }

    Command *FindCommand(Device &device, api::command_list *list)
    {
        const uint64_t handle = CommandHandle(list);
        auto found = device.commands.find(handle);
        if (found != device.commands.end()) return &found->second;
        if (device.commands.size() >= MaxCommands) { ++device.limits; return nullptr; }
        return &device.commands[handle];
    }

    static bool Excluded(const Device &device, const Command *command = nullptr)
    {
        auto thread = device.effect_threads.find(GetCurrentThreadId());
        return (thread != device.effect_threads.end() && thread->second) || (command && command->effects);
    }

    static bool MainDepthCandidate(const DrawContext &context)
    {
        if (context.device_api != api::device_api::vulkan || !context.depth.handle || context.depth.desc.type == api::resource_type::buffer ||
            !context.render_width || !context.render_height ||
            context.depth.desc.texture.width != context.render_width || context.depth.desc.texture.height != context.render_height) return false;
        for (const auto &target : context.targets)
            if (target.handle && target.desc.type != api::resource_type::buffer && target.desc.texture.width == context.depth.desc.texture.width &&
                target.desc.texture.height == context.depth.desc.texture.height) return true;
        return false;
    }

    static Json TargetJson(const Target &target)
    {
        return {{"handle_hex", Hex(target.handle)}, {"handle_u64", target.handle}, {"width", target.desc.texture.width},
                {"height", target.desc.texture.height}, {"format_id", static_cast<uint32_t>(target.desc.texture.format)}, {"samples", target.desc.texture.samples}};
    }

    void Prune(Device &device)
    {
        std::set<uint64_t> referenced;
        for (const auto &command : device.commands)
        {
            for (const auto &binding : command.second.current.tables) referenced.insert(binding.second.table);
            for (const auto &binding : command.second.saved.tables) referenced.insert(binding.second.table);
        }
        for (auto entry = device.tables.begin(); entry != device.tables.end();)
            if (device.frame > entry->second.touched_frame && device.frame - entry->second.touched_frame > UnreferencedTableFrames && !referenced.count(entry->first))
            { entry = device.tables.erase(entry); ++device.pruned_tables; }
            else ++entry;
    }

    Table *FindTable(Device &device, uint64_t handle)
    {
        auto found = device.tables.find(handle);
        if (found != device.tables.end()) { found->second.touched_frame = device.frame; return &found->second; }
        if (device.tables.size() >= MaxTables) Prune(device);
        if (device.tables.size() >= MaxTables) { ++device.limits; return nullptr; }
        Table &table = device.tables[handle]; table.touched_frame = device.frame;
        return &table;
    }

    void Update(Device &device, Slots &slots, const api::descriptor_table_update &update)
    {
        if (!update.descriptors) return;
        if (update.count > MaxSlots) ++device.limits;
        const bool array = device.api == api::device_api::vulkan;
        for (uint32_t index = 0; index < std::min<uint32_t>(update.count, MaxSlots); ++index)
        {
            const std::array<uint32_t, 2> key{update.binding + (array ? 0 : index), array ? update.array_offset + index : 0};
            if (update.type != api::descriptor_type::constant_buffer) { slots.erase(key); continue; }
            const auto &range = static_cast<const api::buffer_range *>(update.descriptors)[index];
            if (!range.buffer.handle) { slots.erase(key); continue; }
            if (slots.size() >= MaxSlots && !slots.count(key)) { ++device.limits; continue; }
            slots[key] = {range};
        }
    }

public:
    Sampler()
    {
        try
        {
            wchar_t path[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"ASTERIA_NATIVE_UNIFORM_PROFILES", path, 32768);
            if (!length || length >= 32768) return;
            const std::filesystem::path source(path);
            profile_manifest["source"] = source.u8string();
            if (!source.is_absolute() || std::filesystem::file_size(source) > 2 * 1024 * 1024)
            { profile_manifest["reason"] = "profile_manifest_path_or_size_rejected"; return; }
            std::ifstream input(source, std::ios::binary);
            const Json manifest = Json::parse(input);
            if (manifest.value("schema_version", 0) != 1 || !manifest.contains("profiles") || !manifest["profiles"].is_array() || manifest["profiles"].size() > 256)
            { profile_manifest["reason"] = "profile_manifest_schema_rejected"; return; }
            asteria_pipeline_provenance::Writer hasher;
            uint64_t bytes = 0, rejected = 0;
            for (const auto &item : manifest["profiles"])
            {
                if (!item.contains("shader_sha256") || !item.contains("spirv_file") || !item.contains("descriptor_set") || !item.contains("binding") ||
                    item.value("stage", std::string()) != "vertex" ||
                    item.value("relative_byte_offset", uint64_t(0)) != ProfileByteOffset || item.value("byte_length", uint64_t(0)) != ProfileBytes)
                { ++rejected; continue; }
                const std::string hash = item["shader_sha256"].get<std::string>();
                if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) { ++rejected; continue; }
                std::filesystem::path file = std::filesystem::u8path(item["spirv_file"].get<std::string>());
                if (!file.is_absolute()) file = source.parent_path() / file;
                const uint64_t size = std::filesystem::file_size(file);
                if (size < 20 || size > 4 * 1024 * 1024 || size > 64 * 1024 * 1024 - bytes) { ++rejected; continue; }
                Profile profile; profile.sha256 = hash; profile.descriptor_set = item["descriptor_set"].get<uint32_t>(); profile.binding = item["binding"].get<uint32_t>();
                profile.entry_point = item.value("vertex_entry_point", std::string("main")); profile.bytes.resize(static_cast<size_t>(size));
                std::ifstream shader(file, std::ios::binary); shader.read(reinterpret_cast<char *>(profile.bytes.data()), profile.bytes.size());
                uint32_t magic = 0; std::memcpy(&magic, profile.bytes.data(), sizeof(magic));
                if (!shader || magic != 0x07230203 || hasher.HashBytes(profile.bytes.data(), profile.bytes.size()) != hash) { ++rejected; continue; }
                bytes += size; profiles.push_back(std::move(profile));
            }
            profile_manifest["verified_profiles"] = profiles.size(); profile_manifest["profile_bytes"] = bytes; profile_manifest["rejected_profiles"] = rejected;
            profile_manifest["status"] = profiles.empty() ? "unavailable" : "loaded";
            profile_manifest["reason"] = profiles.empty() ? "no_sha256_verified_spirv_profiles" : "SHA256-verified actual SPIR-V bytecode profiles loaded; effective-camera semantics remain unverified";
            for (const auto &profile : profiles) live_association.RegisterProfile(profile.sha256);
        }
        catch (...) { profiles.clear(); profile_manifest["status"] = "unavailable"; profile_manifest["reason"] = "profile_manifest_read_or_validation_failed"; }
    }

    void InitDevice(api::device *device)
    {
        const auto device_api = device->get_api();
        std::lock_guard<std::mutex> lock(mutex);
        auto value = devices.find(Owner(device));
        if (value == devices.end() && devices.size() < MaxDevices)
        {
            Device state; state.api = device_api; devices.emplace(Owner(device), std::move(state));
            live_association.InitDevice(Owner(device), device->get_native());
        }
        else if (value == devices.end()) ++device_limit;
    }

    void DestroyDevice(api::device *device)
    {
        std::lock_guard<std::mutex> lock(mutex); live_association.DestroyDevice(Owner(device)); devices.erase(Owner(device));
    }
    void InitCommand(api::command_list *list) { live_association.InitCommand(Owner(list->get_device()), list->get_native()); }
    void DestroyCommand(api::command_list *list)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto owner = Owner(list->get_device());
        const auto device = devices.find(owner); if (device != devices.end()) device->second.commands.erase(CommandHandle(list));
        live_association.ResetCommand(owner, list->get_native(), true);
    }
    void MarkAssociationCoverageLoss(api::device *device, const char *reason) { live_association.CoverageLoss(Owner(device), reason); }
    void MarkAssociationCoverageLoss(uint64_t owner, const char *reason) { live_association.CoverageLoss(owner, reason); }
    void UnsupportedCommand(api::command_list *list, const char *reason)
    {
        live_association.UnsupportedCommand(Owner(list->get_device()), list->get_native(), reason);
    }
    void SubmitCommand(api::command_list *list)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto device = devices.find(Owner(list->get_device()));
        if (device != devices.end()) live_association.SubmitCommand(device->first, list->get_native(), device->second.frame);
    }
    void ResetCommand(api::command_list *list)
    {
        const uint64_t owner = Owner(list->get_device());
        std::lock_guard<std::mutex> lock(mutex);
        auto device = devices.find(owner); if (device != devices.end()) device->second.commands.erase(CommandHandle(list));
        live_association.ResetCommand(owner, list->get_native());
    }

    void InitPipeline(api::device *device, uint32_t count, const api::pipeline_subobject *subobjects, api::pipeline handle)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            Device *state = FindDevice(device); if (!state || Excluded(*state)) return;
            if (state->pipeline_init_callbacks >= 4096)
            { ++state->limits; live_association.CoverageLoss(Owner(device), "pipeline_init_capacity"); return; }
            ++state->pipeline_init_callbacks;
        }
        int32_t match = -1;
        for (uint32_t index = 0; subobjects && index < std::min<uint32_t>(count, 256); ++index)
        {
            const auto &subobject = subobjects[index];
            if (subobject.type != api::pipeline_subobject_type::vertex_shader || !subobject.data || subobject.count != 1) continue;
            const auto &shader = *static_cast<const api::shader_desc *>(subobject.data);
            if (!shader.code || shader.code_size > 4 * 1024 * 1024) continue;
            for (size_t candidate = 0; candidate < profiles.size(); ++candidate)
                if ((!shader.entry_point || profiles[candidate].entry_point == shader.entry_point) && shader.code_size == profiles[candidate].bytes.size() &&
                    std::memcmp(shader.code, profiles[candidate].bytes.data(), shader.code_size) == 0)
                { match = static_cast<int32_t>(candidate); break; }
        }
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(device); if (!state || Excluded(*state)) return;
        if (state->pipelines.size() >= 4096 && !state->pipelines.count(handle.handle)) { ++state->limits; return; }
        for (auto &command : state->commands)
        {
            if (command.second.vertex_pipeline == handle.handle) { command.second.vertex_pipeline = 0; command.second.profile = -1; }
            if (command.second.saved_vertex_pipeline == handle.handle) { command.second.saved_vertex_pipeline = 0; command.second.saved_profile = -1; }
        }
        state->pipelines[handle.handle] = match;
        live_association.InitPipeline(Owner(device), handle.handle, match >= 0 ? profiles[match].sha256 : std::string());
        if (match >= 0) ++state->matching_pipeline_inits;
    }

    void DestroyPipeline(api::device *device, api::pipeline handle)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = devices.find(Owner(device)); if (found == devices.end()) return;
        Device &state = found->second; state.pipelines.erase(handle.handle);
        live_association.DestroyPipeline(Owner(device), handle.handle);
        for (auto &command : state.commands)
        {
            if (command.second.vertex_pipeline == handle.handle) { command.second.vertex_pipeline = 0; command.second.profile = -1; }
            if (command.second.saved_vertex_pipeline == handle.handle) { command.second.saved_vertex_pipeline = 0; command.second.saved_profile = -1; }
        }
    }

    void BindPipeline(api::command_list *list, api::pipeline_stage stages, api::pipeline handle)
    {
        if (!(static_cast<uint32_t>(stages) & static_cast<uint32_t>(api::pipeline_stage::vertex_shader))) return;
        api::device *device = list->get_device();
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(device); if (!state) return;
        Command *command = FindCommand(*state, list); if (!command || Excluded(*state, command)) return;
        auto profile = state->pipelines.find(handle.handle);
        command->vertex_pipeline = handle.handle; command->profile = profile == state->pipelines.end() ? -1 : profile->second;
        if (handle.handle && profile == state->pipelines.end()) ++state->unknown_pipeline_binds;
    }

    void InitResource(api::device *device, const api::resource_desc &desc, api::resource handle)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(device); if (!state) return;
        live_association.DestroyResource(Owner(device), handle.handle);
        if (desc.type != api::resource_type::buffer && desc.type != api::resource_type::unknown)
            live_association.InitResource(Owner(device), handle.handle, desc.texture.width, desc.texture.height,
                                          desc.texture.samples, static_cast<uint32_t>(desc.texture.format));
        state->buffers.erase(handle.handle);
        if (desc.type != api::resource_type::buffer || !(static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(api::resource_usage::constant_buffer))) return;
        if (state->buffers.size() >= MaxBuffers) { ++state->limits; return; }
        state->buffers[handle.handle] = desc;
    }

    void DestroyResource(api::device *device, api::resource handle)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = devices.find(Owner(device)); if (found == devices.end()) return;
        Device &state = found->second; state.buffers.erase(handle.handle);
        live_association.DestroyResource(Owner(device), handle.handle);
        for (auto &table : state.tables)
            for (auto entry = table.second.slots.begin(); entry != table.second.slots.end();)
                if (entry->second.range.buffer == handle) entry = table.second.slots.erase(entry); else ++entry;
        for (auto &command : state.commands)
            for (Bindings *binding : {&command.second.current, &command.second.saved})
                for (auto entry = binding->pushed.begin(); entry != binding->pushed.end();)
                    if (entry->second.range.buffer == handle) entry = binding->pushed.erase(entry); else ++entry;
    }

    void InitLayout(api::device *device, uint32_t count, const api::pipeline_layout_param *params, api::pipeline_layout handle)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(device); if (!state) return;
        if (state->layouts.size() >= MaxLayouts && !state->layouts.count(handle.handle)) { ++state->limits; return; }
        if (count > MaxParameters) ++state->limits;
        std::vector<Parameter> parameters(std::min<uint32_t>(count, MaxParameters));
        for (uint32_t index = 0; params && index < parameters.size(); ++index)
        {
            auto &parameter = parameters[index];
            const auto &input = params[index];
            if (input.type == api::pipeline_layout_param_type::push_descriptors)
            { parameter.known = true; parameter.constant_buffers = input.push_descriptors.type == api::descriptor_type::constant_buffer; }
            else if (input.type == api::pipeline_layout_param_type::push_constants) parameter.known = true;
            else if (input.type == api::pipeline_layout_param_type::descriptor_table || input.type == api::pipeline_layout_param_type::push_descriptors_with_ranges)
            {
                parameter.known = !input.descriptor_table.count || input.descriptor_table.ranges;
                if (input.descriptor_table.count > MaxRanges) { ++state->limits; parameter.known = false; }
                for (uint32_t range = 0; input.descriptor_table.ranges && range < std::min<uint32_t>(input.descriptor_table.count, MaxRanges); ++range)
                    parameter.constant_buffers |= input.descriptor_table.ranges[range].type == api::descriptor_type::constant_buffer;
            }
            else if (input.type == api::pipeline_layout_param_type::descriptor_table_with_static_samplers || input.type == api::pipeline_layout_param_type::push_descriptors_with_static_samplers)
            {
                parameter.known = !input.descriptor_table_with_static_samplers.count || input.descriptor_table_with_static_samplers.ranges;
                if (input.descriptor_table_with_static_samplers.count > MaxRanges) { ++state->limits; parameter.known = false; }
                for (uint32_t range = 0; input.descriptor_table_with_static_samplers.ranges && range < std::min<uint32_t>(input.descriptor_table_with_static_samplers.count, MaxRanges); ++range)
                    parameter.constant_buffers |= input.descriptor_table_with_static_samplers.ranges[range].type == api::descriptor_type::constant_buffer;
            }
        }
        state->layouts[handle.handle] = std::move(parameters);
    }

    void DestroyLayout(api::device *device, api::pipeline_layout handle)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = devices.find(Owner(device)); if (found != devices.end()) found->second.layouts.erase(handle.handle);
    }

    void UpdateDescriptors(api::device *device, uint32_t count, const api::descriptor_table_update *updates)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(device); if (!state) return;
        if (Excluded(*state)) { ++state->excluded_updates; return; }
        if (count > MaxSlots) ++state->limits;
        for (uint32_t index = 0; updates && index < std::min<uint32_t>(count, MaxSlots); ++index)
        {
            const auto &update = updates[index];
            if (update.type != api::descriptor_type::constant_buffer && !state->tables.count(update.table.handle)) continue;
            if (Table *table = FindTable(*state, update.table.handle)) Update(*state, table->slots, update);
        }
    }

    void CopyDescriptors(api::device *device, uint32_t count, const api::descriptor_table_copy *copies)
    {
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(device); if (!state || Excluded(*state)) return;
        if (count > MaxSlots) ++state->limits;
        for (uint32_t index = 0; copies && index < std::min<uint32_t>(count, MaxSlots); ++index)
        {
            const auto &copy = copies[index];
            auto source = state->tables.find(copy.source_table.handle);
            if (source == state->tables.end())
            {
                ++state->unknown_copy_sources;
                auto destination = state->tables.find(copy.dest_table.handle);
                if (destination != state->tables.end())
                    for (uint32_t slot = 0; slot < std::min<uint32_t>(copy.count, MaxSlots); ++slot)
                        destination->second.slots.erase({copy.dest_binding + (state->api == api::device_api::vulkan ? 0 : slot),
                                                         state->api == api::device_api::vulkan ? copy.dest_array_offset + slot : 0});
                continue;
            }
            if (copy.count > MaxSlots) ++state->limits;
            Slots temporary;
            const bool array = state->api == api::device_api::vulkan;
            for (uint32_t slot = 0; slot < std::min<uint32_t>(copy.count, MaxSlots); ++slot)
            {
                const std::array<uint32_t, 2> source_key{copy.source_binding + (array ? 0 : slot), array ? copy.source_array_offset + slot : 0};
                const std::array<uint32_t, 2> dest_key{copy.dest_binding + (array ? 0 : slot), array ? copy.dest_array_offset + slot : 0};
                auto descriptor = source->second.slots.find(source_key);
                if (descriptor != source->second.slots.end()) temporary[dest_key] = descriptor->second;
            }
            if (temporary.empty() && !state->tables.count(copy.dest_table.handle)) continue;
            Table *destination = FindTable(*state, copy.dest_table.handle); if (!destination) continue;
            for (uint32_t slot = 0; slot < std::min<uint32_t>(copy.count, MaxSlots); ++slot)
                destination->slots.erase({copy.dest_binding + (array ? 0 : slot), array ? copy.dest_array_offset + slot : 0});
            for (const auto &entry : temporary)
            {
                if (destination->slots.size() >= MaxSlots && !destination->slots.count(entry.first)) { ++state->limits; continue; }
                destination->slots[entry.first] = entry.second;
            }
        }
    }

    void BindTables(api::command_list *list, api::shader_stage stages, api::pipeline_layout layout, uint32_t first, uint32_t count, const api::descriptor_table *tables)
    {
        if (!(static_cast<uint32_t>(stages) & VertexStage)) return;
        api::device *device = list->get_device();
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(device); if (!state) return;
        Command *command = FindCommand(*state, list); if (!command || Excluded(*state, command)) return;
        if (count > MaxParameters) ++state->limits;
        for (uint32_t index = 0; tables && index < std::min<uint32_t>(count, MaxParameters); ++index)
        {
            const uint32_t parameter = first + index;
            auto definition = state->layouts.find(layout.handle);
            const bool known = definition != state->layouts.end() && parameter < definition->second.size() && definition->second[parameter].known;
            if (known && !definition->second[parameter].constant_buffers) { command->current.tables.erase(parameter); continue; }
            if (!known) ++state->unknown_layout_binds;
            if (!tables[index].handle) { command->current.tables.erase(parameter); continue; }
            auto table = state->tables.find(tables[index].handle);
            if (table == state->tables.end()) ++state->missing_table_binds; else table->second.touched_frame = state->frame;
            if (command->current.tables.size() >= MaxParameters && !command->current.tables.count(parameter)) { ++state->limits; continue; }
            command->current.tables[parameter] = {tables[index].handle, layout.handle};
        }
    }

    void PushDescriptors(api::command_list *list, api::shader_stage stages, api::pipeline_layout layout, uint32_t parameter, const api::descriptor_table_update &update)
    {
        if (!(static_cast<uint32_t>(stages) & VertexStage)) return;
        api::device *device = list->get_device();
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(device); if (!state) return;
        Command *command = FindCommand(*state, list); if (!command || Excluded(*state, command) || !update.descriptors) return;
        if (update.count > MaxSlots) ++state->limits;
        const bool array = state->api == api::device_api::vulkan;
        for (uint32_t index = 0; index < std::min<uint32_t>(update.count, MaxSlots); ++index)
        {
            const std::array<uint32_t, 3> key{parameter, update.binding + (array ? 0 : index), array ? update.array_offset + index : 0};
            if (update.type != api::descriptor_type::constant_buffer) { command->current.pushed.erase(key); continue; }
            const auto &range = static_cast<const api::buffer_range *>(update.descriptors)[index];
            if (!range.buffer.handle) { command->current.pushed.erase(key); continue; }
            if (command->current.pushed.size() >= MaxSlots && !command->current.pushed.count(key)) { ++state->limits; continue; }
            command->current.pushed[key] = {range}; command->current.pushed_layouts[parameter] = layout.handle;
        }
    }

    void BeginEffects(api::command_list *list)
    {
        api::device *device = list->get_device();
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(device); if (!state) return;
        ++state->effect_threads[GetCurrentThreadId()];
        if (Command *command = FindCommand(*state, list))
        {
            if (!command->effects) { command->saved = command->current; command->saved_profile = command->profile; command->saved_vertex_pipeline = command->vertex_pipeline; }
            ++command->effects;
        }
    }

    void FinishEffects(api::command_list *list)
    {
        api::device *device = list->get_device();
        std::lock_guard<std::mutex> lock(mutex);
        auto found = devices.find(Owner(device)); if (found == devices.end()) return;
        Device &state = found->second;
        auto thread = state.effect_threads.find(GetCurrentThreadId());
        if (thread != state.effect_threads.end() && thread->second && !--thread->second) state.effect_threads.erase(thread);
        if (Command *command = FindCommand(state, list))
            if (command->effects && !--command->effects)
            {
                command->current = std::move(command->saved); command->saved = {}; command->profile = command->saved_profile;
                command->vertex_pipeline = command->saved_vertex_pipeline; command->saved_profile = -1; command->saved_vertex_pipeline = 0;
            }
    }

    void AdvanceFrame(uint64_t owner, uint64_t frame, const Json &camera, uint32_t width = 0, uint32_t height = 0)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = devices.find(owner); if (found == devices.end()) return;
        Device &state = found->second;
        if (state.limits) live_association.CoverageLoss(owner, "sampler_tracking_limit");
        live_association.Present(owner, frame, width, height,
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        presentation_comparator.ObservePresent(owner, frame, camera, samples);
        if (state.frame != frame) { state.frame = frame; state.samples_this_frame = 0; state.sampled_ranges.clear(); Prune(state); }
        for (auto sample = samples.begin(); sample != samples.end();)
            if (sample->value("device_owner_u64", uint64_t(0)) == owner && frame > sample->value("observer_frame", uint64_t(0)) &&
                frame - sample->value("observer_frame", uint64_t(0)) > RetainedFrames)
            { sample = samples.erase(sample); ++samples_evicted; } else ++sample;
    }

    void ObserveDraw(api::command_list *list, const DrawContext &context)
    {
        if (!MainDepthCandidate(context) || !context.attachments_complete)
        { live_association.CoverageLoss(context.owner, "candidate_attachments_incomplete"); return; }
        std::vector<Request> requests;
        int32_t profile_index = -1;
        uint64_t vertex_pipeline = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = devices.find(context.owner); if (found == devices.end()) return;
            Device &state = found->second;
            auto command = state.commands.find(CommandHandle(list));
            if (command == state.commands.end()) { live_association.CoverageLoss(context.owner, "command_bindings_unavailable"); return; }
            if (Excluded(state, &command->second)) return;
            ++state.candidate_draws;
            if (command->second.profile < 0)
            { ++state.unverified_draw_profiles; live_association.CoverageLoss(context.owner, "unverified_draw_profile"); return; }
            profile_index = command->second.profile; vertex_pipeline = command->second.vertex_pipeline;
            const Profile &profile = profiles[profile_index];
            if (state.frame != context.frame) { state.frame = context.frame; state.samples_this_frame = 0; state.sampled_ranges.clear(); Prune(state); }
            if (state.samples_this_frame >= SamplesPerFrame)
            { ++state.skipped_frame_budget; live_association.CoverageLoss(context.owner, "uniform_sample_budget"); return; }
            auto add = [&](const Descriptor &descriptor, uint64_t table, uint64_t layout, uint32_t parameter, uint32_t binding, uint32_t array_element, bool pushed) {
                if (parameter != profile.descriptor_set || binding != profile.binding || array_element != 0) return;
                const auto &range = descriptor.range;
                if (range.size != UINT64_MAX && range.size < ProfileByteOffset + ProfileBytes) { ++state.ignored_short_ranges; return; }
                const std::array<uint64_t, 6> key{range.buffer.handle, range.offset, range.size, layout, parameter, (uint64_t(binding) << 32) | array_element};
                if (state.sampled_ranges.count(key)) { live_association.CoverageLoss(context.owner, "uniform_range_deduplicated"); return; }
                if (state.samples_this_frame >= SamplesPerFrame)
                { ++state.skipped_frame_budget; live_association.CoverageLoss(context.owner, "uniform_sample_budget"); return; }
                state.sampled_ranges.insert(key); ++state.samples_this_frame; ++state.total_samples;
                Request request; request.range = range; request.table = table; request.layout = layout; request.parameter = parameter;
                request.binding = binding; request.array_element = array_element; request.pushed = pushed;
                auto buffer = state.buffers.find(range.buffer.handle);
                if (buffer != state.buffers.end()) { request.desc = buffer->second; request.desc_known = true; }
                requests.push_back(request);
            };
            for (const auto &binding : command->second.current.tables)
            {
                auto table = state.tables.find(binding.second.table); if (table == state.tables.end()) continue;
                for (const auto &descriptor : table->second.slots)
                    add(descriptor.second, binding.second.table, binding.second.layout, binding.first, descriptor.first[0], descriptor.first[1], false);
            }
            for (const auto &descriptor : command->second.current.pushed)
                add(descriptor.second, 0, command->second.current.pushed_layouts[descriptor.first[0]], descriptor.first[0], descriptor.first[1], descriptor.first[2], true);
        }
        if (requests.empty()) { live_association.CoverageLoss(context.owner, "native_uniform_request_unavailable"); return; }
        const bool unique_request = requests.size() == 1;
        if (!unique_request) live_association.CoverageLoss(context.owner, "multiple_native_uniform_requests");
        asteria_live_association::DrawMetadata metadata{};
        metadata.owner = context.owner; metadata.nativeDevice = context.native_device; metadata.nativeCommand = context.native_command;
        metadata.observerInterval = context.frame; metadata.ordinal = context.draw_ordinal; metadata.sequence = context.recording_sequence;
        metadata.tickNanoseconds = context.recording_tick_nanoseconds; metadata.depth = context.depth.handle; metadata.pipeline = vertex_pipeline;
        metadata.width = context.render_width; metadata.height = context.render_height;
        metadata.viewportKnown = context.viewport_known; metadata.viewport = context.viewport;
        uint32_t matching_colors = 0;
        for (const auto &target : context.targets)
            if (target.desc.type != api::resource_type::buffer && target.desc.texture.width == context.render_width &&
                target.desc.texture.height == context.render_height) { metadata.color = target.handle; ++matching_colors; }
        const bool unique_color = matching_colors == 1;
        if (!unique_color) live_association.CoverageLoss(context.owner, "color_attachment_ambiguous_or_missing");
        HMODULE module = nullptr;
        GetModuleHandleExW(0, L"AsteriaFrameGenerationLayer.dll", &module);
        CopyMappedBytes copy = module ? reinterpret_cast<CopyMappedBytes>(GetProcAddress(module, "AsteriaCopyMappedBufferBytes")) : nullptr;
        QueryBoundDescriptor queryDesc = module ? reinterpret_cast<QueryBoundDescriptor>(GetProcAddress(module, "AsteriaQueryBoundBufferDescriptor")) : nullptr;
        AsteriaQueryNativeViewportV1Fn queryViewport = module ? reinterpret_cast<AsteriaQueryNativeViewportV1Fn>(GetProcAddress(module, "AsteriaQueryNativeViewportV1")) : nullptr;
        AsteriaQueryDrawViewportV1Fn queryDrawViewport = module ? reinterpret_cast<AsteriaQueryDrawViewportV1Fn>(GetProcAddress(module, "AsteriaQueryDrawViewportV1")) : nullptr;
        AsteriaQueryDrawViewportDiagnosticV1Fn queryDrawDiagnostic=module?reinterpret_cast<AsteriaQueryDrawViewportDiagnosticV1Fn>(GetProcAddress(module,"AsteriaQueryDrawViewportDiagnosticV1")):nullptr;
        const uint64_t command_handle = list->get_native();
        api::device *device = list->get_device();
        for (Request &request : requests)
        {
            if (!request.desc_known) { request.desc = device->get_resource_desc(request.range.buffer); request.desc_known = request.desc.type == api::resource_type::buffer; }
            AsteriaBoundBufferDescriptorV1 native_desc{};
            const uint32_t desc_status = QueryCommandDescriptorBridge(
                queryDesc, context.native_device, list, request.parameter, request.binding, request.array_element, &native_desc);
            AsteriaNativeViewportEvidenceV1 native_viewport{};
            const uint32_t viewport_status = QueryViewportBridge(queryViewport, context.native_device, command_handle, &native_viewport);
            metadata.viewportKnown = context.viewport_known; metadata.viewport = context.viewport;
            bool native_viewport_observed = viewport_status == ASTERIA_VIEWPORT_EVIDENCE_OK &&
                native_viewport.flags == ASTERIA_VIEWPORT_EVIDENCE_OBSERVED_ONLY && native_viewport.viewport_count == 1 &&
                native_viewport.device_generation == native_desc.device_generation && native_viewport.command_generation == native_desc.command_generation &&
                native_viewport.recording_generation == native_desc.recording_generation && native_viewport.set_sequence != 0 &&
                native_viewport.source >= ASTERIA_VIEWPORT_SOURCE_LEGACY && native_viewport.source <= ASTERIA_VIEWPORT_SOURCE_EXT_WITH_COUNT;
            for (float value : native_viewport.viewport) native_viewport_observed &= std::isfinite(value);
            native_viewport_observed &= native_viewport.viewport[2] != 0 && native_viewport.viewport[3] != 0;
            metadata.nativeViewportSource = native_viewport_observed ? native_viewport.source : 0;
            metadata.nativeViewportSetSequence = native_viewport_observed ? native_viewport.set_sequence : 0;
            if (native_viewport_observed)
            {
                metadata.viewportKnown = true;
                std::copy_n(native_viewport.viewport, 6, metadata.viewport.begin());
            }
            AsteriaDrawViewportEvidenceV1 draw_viewport{};
            const uint32_t draw_viewport_status=QueryDrawViewportBridge(queryDrawViewport,context.native_device,command_handle,&draw_viewport);
            const uint32_t required_flags=ASTERIA_DRAW_VIEWPORT_RECORDING_CONTRACT_SUPPORTED|ASTERIA_DRAW_VIEWPORT_OBSERVER_ONLY;
            bool draw_viewport_supported=draw_viewport_status==ASTERIA_DRAW_VIEWPORT_OK && draw_viewport.rejection_reason==ASTERIA_DRAW_VIEWPORT_REJECTION_NONE &&
                (draw_viewport.flags&required_flags)==required_flags &&
                (draw_viewport.flags&~uint32_t(7))==0 && draw_viewport.viewport_count==1 &&
                draw_viewport.device_generation==native_desc.device_generation && draw_viewport.command_generation==native_desc.command_generation &&
                draw_viewport.recording_generation==native_desc.recording_generation && draw_viewport.pipeline==vertex_pipeline &&
                draw_viewport.pipeline_generation && draw_viewport.bind_sequence && draw_viewport.mode>=1 && draw_viewport.mode<=3;
            if (draw_viewport.mode==ASTERIA_DRAW_VIEWPORT_STATIC)
                draw_viewport_supported &= draw_viewport.source==ASTERIA_DRAW_VIEWPORT_SOURCE_NONE && draw_viewport.setting_sequence==0 &&
                    (draw_viewport.flags&ASTERIA_DRAW_VIEWPORT_DYNAMIC_SET_AFTER_BOUNDARY)==0;
            else
                draw_viewport_supported &= (draw_viewport.flags&ASTERIA_DRAW_VIEWPORT_DYNAMIC_SET_AFTER_BOUNDARY)!=0 && draw_viewport.setting_sequence!=0 &&
                    ((draw_viewport.mode==ASTERIA_DRAW_VIEWPORT_LEGACY_DYNAMIC && draw_viewport.source==ASTERIA_DRAW_VIEWPORT_SOURCE_LEGACY) ||
                     (draw_viewport.mode==ASTERIA_DRAW_VIEWPORT_DYNAMIC_WITH_COUNT &&
                      (draw_viewport.source==ASTERIA_DRAW_VIEWPORT_SOURCE_CORE_WITH_COUNT || draw_viewport.source==ASTERIA_DRAW_VIEWPORT_SOURCE_EXT_WITH_COUNT)));
            for (float value:draw_viewport.viewport) draw_viewport_supported &= std::isfinite(value);
            draw_viewport_supported &= draw_viewport.viewport[2]>0 && draw_viewport.viewport[3]!=0 &&
                draw_viewport.viewport[4]>=0 && draw_viewport.viewport[4]<=1 && draw_viewport.viewport[5]>=0 && draw_viewport.viewport[5]<=1 &&
                draw_viewport.viewport[4]!=draw_viewport.viewport[5];
            metadata.viewportDrawStateVerified=draw_viewport_supported;
            metadata.pipelineViewport={{"status",draw_viewport_status},{"recording_contract_supported",draw_viewport_supported},
                {"observer_only",true},{"mode",draw_viewport.mode},{"source",draw_viewport.source},{"viewport_count",draw_viewport.viewport_count},
                {"pipeline",draw_viewport.pipeline},{"pipeline_generation",draw_viewport.pipeline_generation},
                {"bind_sequence",draw_viewport.bind_sequence},{"setting_sequence",draw_viewport.setting_sequence},
                {"flags",draw_viewport.flags},{"rejection_reason",draw_viewport.rejection_reason},{"device_generation",draw_viewport.device_generation},
                {"command_generation",draw_viewport.command_generation},{"recording_generation",draw_viewport.recording_generation},
                {"viewport",std::vector<float>(draw_viewport.viewport,draw_viewport.viewport+6)}};
            AsteriaDrawViewportDiagnosticV1 diagnostic{};
            const auto diagnosticStatus=QueryDrawViewportDiagnosticBridge(queryDrawDiagnostic,context.native_device,command_handle,&diagnostic);
            const bool diagnosticMatched=diagnosticStatus==0&&(diagnostic.flags&3)==3&&(diagnostic.flags&~uint32_t(15))==0&&
                diagnostic.device_generation==native_desc.device_generation&&diagnostic.command_generation==native_desc.command_generation&&
                diagnostic.recording_generation==native_desc.recording_generation&&diagnostic.pipeline==vertex_pipeline&&
                diagnostic.pipeline_generation==draw_viewport.pipeline_generation&&diagnostic.pipeline_generation!=0;
            const auto poisonJson=[&](const AsteriaViewportPoisonV1& poison)
            {
                return Json{{"event",poison.event},{"pipeline",poison.pipeline},{"pipeline_generation",poison.pipeline_generation},
                    {"sequence",poison.sequence},{"rejection_reason",poison.rejection_reason},
                    {"payload_hint_index",poison.payload_hint_index==UINT32_MAX?Json(nullptr):Json(poison.payload_hint_index)},
                    {"creation_record_key",poison.pipeline&&poison.pipeline_generation?Json(PipelineDiagnosticRecordKey(context.native_device,diagnostic.device_generation,poison.pipeline,poison.pipeline_generation)):Json(nullptr)}};
            };
            metadata.pipelineViewport["diagnostic"]={{"status",diagnosticStatus},{"observer_only",true},{"matched_descriptor_recording",diagnosticMatched},
                {"flags",diagnostic.flags},{"native_device",context.native_device},{"device_generation",diagnostic.device_generation},
                {"command_generation",diagnostic.command_generation},{"recording_generation",diagnostic.recording_generation},
                {"pipeline",diagnostic.pipeline},{"pipeline_generation",diagnostic.pipeline_generation},
                {"current_creation_observed",bool(diagnostic.flags&ASTERIA_DRAW_DIAGNOSTIC_CURRENT_CREATION_OBSERVED)},
                {"current_creation_rejection",diagnostic.current_creation_rejection},
                {"payload_hint_index",diagnostic.payload_hint_index==UINT32_MAX?Json(nullptr):Json(diagnostic.payload_hint_index)},
                {"creation_record_key",diagnostic.pipeline&&diagnostic.pipeline_generation?Json(PipelineDiagnosticRecordKey(context.native_device,diagnostic.device_generation,diagnostic.pipeline,diagnostic.pipeline_generation)):Json(nullptr)},
                {"first_poison",poisonJson(diagnostic.first_poison)},{"last_poison",poisonJson(diagnostic.last_poison)}};
            if (draw_viewport_supported)
            {
                metadata.viewportKnown=true;
                std::copy_n(draw_viewport.viewport,6,metadata.viewport.begin());
            }
            uint64_t descriptor_range = 0;
            const char *descriptor_issue = desc_status == ASTERIA_DESCRIPTOR_OK ? ValidateNativeDescriptor(native_desc, request, descriptor_range) : "native_descriptor_unavailable";
            const bool desc_verified = descriptor_issue == nullptr;
            const uint64_t base_offset = desc_verified ? native_desc.effective_offset : 0;
            Json record = {{"device_owner_u64", context.owner}, {"device_handle_hex", Hex(context.native_device)}, {"device_handle_u64", context.native_device},
                           {"observer_frame", context.frame}, {"sample_tick_ms", GetTickCount64()}, {"source", "application_draw_bound_vertex_constant_buffer"},
                           {"command_native_handle_hex", Hex(command_handle)}, {"command_native_handle_u64", command_handle},
                           {"status", "unavailable"}, {"reason", "bridge_unavailable"}, {"profile", "DXVK_2.3.1_hardware_VS_float_member_offset_256_c8_to_c11"},
                           {"profile_candidate", true}, {"spirv_binding_profile_verified", true}, {"profile_shader_sha256", profiles[profile_index].sha256},
                           {"vertex_pipeline_handle_hex", Hex(vertex_pipeline)}, {"vertex_pipeline_handle_u64", vertex_pipeline}, {"camera_semantics_verified", false}, {"frame_timing_verified", false},
                           {"native_camera_ready", false}, {"motion_vectors_verified", false}, {"bridge_resolved", copy != nullptr}, {"bridge_copy_first", false},
                           {"bridge_copy_second", false}, {"copy_stable", false}, {"finite", false}, {"raw_register_floats", nullptr},
                           {"pipeline_record_ids", context.pipeline_record_ids}, {"pipeline_provenance_enabled", context.pipeline_provenance_enabled},
                           {"descriptor", {{"buffer_handle_hex", Hex(request.range.buffer.handle)}, {"buffer_handle_u64", request.range.buffer.handle},
                                           {"range_offset_bytes", request.range.offset}, {"range_size_bytes", request.range.size}, {"profile_relative_byte_offset", ProfileByteOffset},
                                           {"table_handle_hex", Hex(request.table)}, {"table_handle_u64", request.table}, {"layout_handle_hex", Hex(request.layout)},
                                           {"layout_handle_u64", request.layout}, {"parameter_index", request.parameter}, {"binding", request.binding}, {"array_element", request.array_element},
                                           {"stage_bits", VertexStage}, {"pushed", request.pushed},
                                           {"dynamic_offsets_observed", desc_verified ? (native_desc.dynamic_offset_count > 0) : false},
                                           {"actual_native_dynamic_offset_count", desc_verified ? Json(native_desc.dynamic_offset_count) : Json(nullptr)},
                                           {"actual_descriptor_type_verified", desc_verified},
                                           {"native_binding_offset_verified", desc_verified},
                                           {"native_descriptor_validation", descriptor_issue ? Json(descriptor_issue) : Json("verified")},
                                           {"native_descriptor_status", desc_status},
                                           {"native_descriptor_type", desc_verified ? Json(native_desc.descriptor_type) : Json(nullptr)},
                                           {"native_dynamic_offset_bytes", desc_verified ? Json(native_desc.dynamic_offset) : Json(nullptr)},
                                           {"native_effective_offset_bytes", desc_verified ? Json(native_desc.effective_offset) : Json(nullptr)},
                                           {"offset_interpretation", desc_verified ?
                                               "Verified native Vulkan bound buffer descriptor with exact effective_offset" :
                                               "Unverified native binding; candidate metadata retained without copying guessed offsets"}}},
                           {"buffer", {{"descriptor_known", request.desc_known}, {"type_id", static_cast<uint32_t>(request.desc.type)},
                                       {"size_bytes", request.desc_known ? Json(request.desc.buffer.size) : Json(nullptr)}, {"usage_bits", static_cast<uint32_t>(request.desc.usage)},
                                       {"heap_id", static_cast<uint32_t>(request.desc.heap)}, {"flags_bits", static_cast<uint32_t>(request.desc.flags)}}},
                           {"candidate_filter", "depth_and_color_target_match_last_present_dimensions"},
                           {"last_present_dimensions", {{"width", context.render_width}, {"height", context.render_height}}},
                           {"bound_depth", TargetJson(context.depth)}, {"bound_targets", Json::array()}};
            record["draw_recording"] = {{"native_command", context.native_command}, {"ordinal", context.draw_ordinal},
                {"sequence", context.recording_sequence}, {"tick_nanoseconds", context.recording_tick_nanoseconds},
                {"viewport_known", metadata.viewportKnown}, {"viewport", metadata.viewport},
                {"viewport_draw_state_verified", metadata.viewportDrawStateVerified}, {"pipeline_viewport",metadata.pipelineViewport},
                {"reshade_viewport_event_observed", context.viewport_known},
                {"native_viewport_observation", {{"status", viewport_status}, {"matched_descriptor_recording_generation", native_viewport_observed},
                    {"observed_only", true}, {"source", native_viewport.source}, {"set_sequence", native_viewport.set_sequence},
                    {"viewport_count", native_viewport.viewport_count}, {"device_generation", native_viewport.device_generation},
                    {"command_generation", native_viewport.command_generation}, {"recording_generation", native_viewport.recording_generation}}},
                {"native_command_allocation_generation", native_desc.command_generation},
                {"native_command_recording_generation", native_desc.recording_generation},
                {"native_device_generation",native_desc.device_generation},
                {"gpu_frame_aligned", false}};
            for (const Target &target : context.targets) record["bound_targets"].push_back(TargetJson(target));
            const bool offset_valid = desc_verified && base_offset <= UINT64_MAX - ProfileByteOffset - ProfileBytes;
            const uint64_t offset = offset_valid ? base_offset + ProfileByteOffset : 0;
            record["descriptor"]["absolute_buffer_byte_offset"] = offset_valid ? Json(offset) : Json(nullptr);
            if (!desc_verified) record["reason"] = descriptor_issue;
            else if (!request.desc_known) record["reason"] = "buffer_descriptor_unavailable";
            else if (!offset_valid || offset > request.desc.buffer.size || ProfileBytes > request.desc.buffer.size - offset) record["reason"] = "buffer_range_out_of_bounds";
            else if (copy)
            {
                std::array<float, 16> first{}, second{};
                const bool first_ok = CopyBridge(copy, context.native_device, request.range.buffer.handle, offset, first.data());
                const bool second_ok = first_ok && CopyBridge(copy, context.native_device, request.range.buffer.handle, offset, second.data());
                const bool stable = first_ok && second_ok && std::memcmp(first.data(), second.data(), ProfileBytes) == 0;
                bool finite = first_ok;
                for (float value : first) finite &= std::isfinite(value);
                record["bridge_copy_first"] = first_ok; record["bridge_copy_second"] = second_ok; record["copy_stable"] = stable; record["finite"] = finite;
                if (first_ok) record["raw_register_floats"] = first;
                if (unique_request && unique_color && stable && finite && context.native_command == command_handle)
                    live_association.Record(metadata, native_desc, profiles[profile_index].sha256, first, true);
                else live_association.CoverageLoss(context.owner, "native_matrix_copy_unqualified");
                if (stable && finite && descriptor_range >= ModelMatrixByteOffset + ModelMatrixBytes && base_offset <= UINT64_MAX - ModelMatrixByteOffset - ModelMatrixBytes)
                {
                    const uint64_t model_offset = base_offset + ModelMatrixByteOffset;
                    if (model_offset <= request.desc.buffer.size && ModelMatrixBytes <= request.desc.buffer.size - model_offset)
                    {
                        std::array<float, 12> model_first{}, model_second{};
                        if (CopyBytesBridge(copy, context.native_device, request.range.buffer.handle, model_offset, static_cast<uint32_t>(ModelMatrixBytes), model_first.data()) &&
                            CopyBytesBridge(copy, context.native_device, request.range.buffer.handle, model_offset, static_cast<uint32_t>(ModelMatrixBytes), model_second.data()) &&
                            std::memcmp(model_first.data(), model_second.data(), ModelMatrixBytes) == 0)
                        {
                            record["raw_model_c58_c60_floats"] = model_first;
                        }
                    }
                }
                if (!first_ok || !second_ok) record["reason"] = "mapped_buffer_copy_failed";
                else if (!stable) record["reason"] = "mapped_buffer_copy_changed_between_reads";
                else if (!finite) record["reason"] = "register_values_not_finite";
                else { record["status"] = "copied_profile_candidate"; record["reason"] = "Stable finite bytes copied from verified native binding; matrix semantics and render-frame alignment remain unverified"; }
            }
            if (record["status"] != "copied_profile_candidate")
                live_association.CoverageLoss(context.owner, record["reason"].get<std::string>());
            {
                std::lock_guard<std::mutex> lock(mutex);
                record["sample_id"] = ++sequence;
                presentation_comparator.ObserveSample(record);
                ++outcomes[record["reason"].get<std::string>()];
                if (first_successful_samples.size() < 16 && record["status"] == "copied_profile_candidate") first_successful_samples.push_back(record);
                if (samples.size() >= MaxSamples) { samples.pop_front(); ++samples_evicted; }
                samples.push_back(std::move(record));
            }
        }
        if (module) FreeLibrary(module);
    }

    Json Snapshot(bool active) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        Json report = {{"schema_version", 1}, {"author", "Anokianx"}, {"pid", GetCurrentProcessId()}, {"enabled", true}, {"read_only", true},
                       {"session_active", active}, {"resources_are_live", false}, {"native_fg_ready", false}, {"native_camera_ready", false},
                       {"camera_semantics_verified", false}, {"motion_vectors_verified", false}, {"profile_candidate", true},
                       {"count_semantics", "Existing CPU-mapped buffer bytes copied during recorded application draw; no GPU mapping, execution, motion or effective-camera inference"},
                       {"candidate_filter", "depth_and_color_target_match_last_present_dimensions"},
                       {"candidate_filter_semantics", "Both bound depth and a color target must match the most recent presentation dimensions; this is not main-view or frame-alignment proof"},
                       {"limits", {{"devices", MaxDevices}, {"tables_per_device", MaxTables}, {"commands_per_device", MaxCommands}, {"slots_per_table", MaxSlots},
                                   {"sample_attempts_per_device_frame", SamplesPerFrame}, {"samples_retained", MaxSamples}, {"observer_frames_retained", RetainedFrames}}},
                       {"sample_attempts", sequence}, {"samples_evicted", samples_evicted}, {"device_limit_hits", device_limit},
                       {"outcomes", outcomes}, {"presentation_comparison", presentation_comparator.Snapshot()}, {"profile_manifest", profile_manifest}, {"first_successful_samples", first_successful_samples},
                       {"devices", Json::array()}, {"samples", Json::array()}};
        report["heartbeat"] = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        report["live_draw_association"] = live_association.Snapshot();
        for (const auto &device : devices)
            report["devices"].push_back({{"device_owner_u64", device.first}, {"observer_frame", device.second.frame}, {"tables_cached", device.second.tables.size()},
                                         {"commands_cached", device.second.commands.size()}, {"buffer_descriptors_cached", device.second.buffers.size()},
                                         {"layouts_cached", device.second.layouts.size()}, {"candidate_draws", device.second.candidate_draws}, {"sample_attempts", device.second.total_samples},
                                         {"frame_budget_skips", device.second.skipped_frame_budget}, {"limit_hits", device.second.limits}, {"missing_table_binds", device.second.missing_table_binds},
                                         {"unknown_layout_binds", device.second.unknown_layout_binds}, {"unknown_copy_sources", device.second.unknown_copy_sources},
                                         {"pipeline_init_callbacks", device.second.pipeline_init_callbacks}, {"matching_pipeline_inits", device.second.matching_pipeline_inits},
                                         {"unknown_pipeline_binds", device.second.unknown_pipeline_binds}, {"unverified_draw_profiles", device.second.unverified_draw_profiles},
                                         {"ignored_short_ranges", device.second.ignored_short_ranges}, {"excluded_descriptor_updates", device.second.excluded_updates}, {"pruned_tables", device.second.pruned_tables}});
        for (const Json &sample : samples) report["samples"].push_back(sample);
        return report;
    }
};
}
