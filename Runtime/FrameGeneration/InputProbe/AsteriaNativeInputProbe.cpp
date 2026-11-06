#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#include <reshade.hpp>
#include <json.hpp>
#include "EngineCameraProbe.h"
#include "AsteriaApiFormatName.h"
#include "PipelineProvenance.h"
#include "NativeUniformSampler.h"
#include "../Layer/AsteriaFrameInputApi.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace api = reshade::api;
using Json = nlohmann::json;

extern "C" __declspec(dllexport) const char *NAME = "Asteria Native Input Probe";
extern "C" __declspec(dllexport) const char *AUTHOR = "Anokianx";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Read-only engine input discovery; no frame generation or motion synthesis.";

namespace
{
constexpr size_t MaxDevices = 8, MaxResources = 4096, MaxViews = 8192;
constexpr size_t MaxCommands = 256, MaxTables = 4096, MaxDescriptors = 256;
constexpr size_t MaxBindings = 128;
constexpr size_t MaxLayouts = 1024, MaxLayoutParams = 64;
constexpr uint64_t TableRetentionFrames = 2;

enum class LimitReason : size_t
{
    Resources, Views, CommandLists, DescriptorTables, DescriptorSlots, CommandTableBindings,
    UpdateBatch, CopyBatch, DescriptorRange, RenderTargetRange, BindTableRange, PipelineLayouts, LayoutParameters, LayoutRanges, ViewFormats, Count
};

constexpr const char *LimitNames[] = {
    "resources", "views", "command_lists", "descriptor_tables", "descriptor_slots", "command_table_bindings",
    "update_batch_range", "copy_batch_range", "descriptor_range", "render_target_range", "bind_table_range",
    "pipeline_layouts", "layout_parameters", "layout_ranges", "formats_per_resource"
};

struct Counts
{
    uint64_t reads = 0, writes = 0, draws = 0, dispatches = 0;
    Counts &operator+=(const Counts &other)
    {
        reads += other.reads; writes += other.writes;
        draws += other.draws; dispatches += other.dispatches;
        return *this;
    }
};

struct Resource
{
    api::resource_desc desc{};
    Counts pending{}, last{}, total{};
    uint64_t first_frame = 0, last_frame = 0;
    bool observed_render_target = false, observed_depth = false;
    bool observed_sampled = false, observed_uav = false;
    std::unordered_set<uint32_t> observed_view_formats;
};

struct View
{
    uint64_t resource = 0;
    api::resource_view_desc desc{};
    api::resource_usage usage = api::resource_usage::undefined;
    api::resource_usage observed_usage = api::resource_usage::undefined;
    bool creation_usage_known = false;
};

struct Descriptor
{
    uint64_t view = 0;
    bool writable = false;
};

using DescriptorSlots = std::map<std::array<uint32_t, 4>, Descriptor>;

struct Table
{
    DescriptorSlots descriptors;
    uint64_t last_touched_frame = 0;
};

struct LayoutParameter
{
    bool known = false, may_contain_textures = false;
};

struct Bindings
{
    std::vector<uint64_t> targets;
    uint64_t depth = 0;
    DescriptorSlots pushed;
    std::map<std::array<uint32_t, 2>, uint64_t> tables;
    bool viewport_known = false;
    api::viewport viewport{};
};

struct Command
{
    Bindings bindings, saved;
    uint32_t effect_depth = 0;
    uint64_t draw_ordinal = 0;
};

struct Device
{
    api::device_api api = api::device_api::d3d9;
    uint64_t native = 0, owner = 0, frame = 0, pending_draws = 0, last_draws = 0;
    uint64_t total_draws = 0, pending_dispatches = 0, last_dispatches = 0;
    uint64_t total_dispatches = 0, excluded_draws = 0, excluded_bindings = 0;
    uint64_t effect_scopes = 0, limit_hits = 0;
    std::array<uint64_t, static_cast<size_t>(LimitReason::Count)> limit_reasons{};
    uint64_t descriptor_tables_pruned = 0, missing_table_bindings = 0;
    uint64_t descriptor_updates = 0, descriptor_table_binds = 0;
    uint64_t ignored_nontexture_updates = 0, nontexture_table_binds = 0, unknown_layout_table_binds = 0;
    uint64_t unclassified_copy_sources = 0;
    uint64_t peak_tables = 0, peak_commands = 0, last_table_gc_frame = UINT64_MAX;
    uint32_t effect_depth = 0;
    std::map<DWORD, uint32_t> effect_threads;
    std::unordered_map<uint64_t, Resource> resources;
    std::unordered_map<uint64_t, View> views;
    std::unordered_map<api::command_list *, Command> commands;
    std::unordered_map<uint64_t, Table> tables;
    std::unordered_map<uint64_t, std::vector<LayoutParameter>> layouts;
    Json engine_camera = {{"status", "unavailable"}, {"reason", "No presentation callback sampled yet."}};
    uint64_t last_present_tick_ms = 0, camera_samples_started = 0, camera_samples_completed = 0;
    uint64_t camera_sample_started_tick_ms = 0, camera_sample_completed_tick_ms = 0;
    DWORD camera_sample_thread = 0;
    uint32_t render_width = 0, render_height = 0;
    uint64_t draw_recording_sequence = 0;
};

std::mutex mutex;
std::condition_variable wake;
std::unordered_map<api::device *, Device> devices;
std::filesystem::path output_path;
HANDLE writer_thread = nullptr;
Json latest = Json::object();
bool active = false, stopping = false;
uint64_t snapshot_count = 0;
uint64_t device_limit_hits = 0;
std::unique_ptr<asteria_pipeline_provenance::Collector> pipeline_provenance;
std::shared_ptr<asteria_native_uniform::Sampler> native_uniform;
std::unordered_map<uint64_t, uint64_t> frame_input_registrations;
uint64_t early_frame_input_registration = 0;

using RegisterFrameInputProviderFn = int32_t (ASTERIA_FG_CALL *)(const AsteriaFrameInputRegistrationV1 *, uint64_t *);
using UnregisterFrameInputProviderFn = int32_t (ASTERIA_FG_CALL *)(uint64_t);

HMODULE FrameInputLayerModule()
{
    return GetModuleHandleW(L"AsteriaFrameGenerationLayer.dll");
}

void RegisterFrameInputProviderForDevice(api::device *device)
{
    if (!device || device->get_api() != api::device_api::vulkan) return;
    wchar_t enabled[8]{};
    if (GetEnvironmentVariableW(L"ASTERIA_FG_PROVIDER_REGISTRATION", enabled, 8) != 1 || enabled[0] != L'1') return;
    if (early_frame_input_registration)
    {
        frame_input_registrations[reinterpret_cast<uintptr_t>(device)] = early_frame_input_registration;
        early_frame_input_registration = 0;
        return;
    }
    const auto module = FrameInputLayerModule();
    if (!module) return;
    const auto registerProvider = reinterpret_cast<RegisterFrameInputProviderFn>(GetProcAddress(module, "AsteriaRegisterFrameInputProviderV1"));
    if (!registerProvider) return;
    AsteriaFrameInputRegistrationV1 info{sizeof(info), ASTERIA_FG_INPUT_API_V1, 3};
    uint64_t registration = 0;
    if (registerProvider(&info, &registration) == ASTERIA_FG_INPUT_OK && registration)
        frame_input_registrations[reinterpret_cast<uintptr_t>(device)] = registration;
}

void RegisterFrameInputProviderEarly()
{
    wchar_t early[8]{};
    if (GetEnvironmentVariableW(L"ASTERIA_FG_PROVIDER_REGISTRATION_EARLY", early, 8) != 1 || early[0] != L'1') return;
    wchar_t enabled[8]{};
    if (GetEnvironmentVariableW(L"ASTERIA_FG_PROVIDER_REGISTRATION", enabled, 8) != 1 || enabled[0] != L'1') return;
    const auto module = FrameInputLayerModule();
    if (!module) return;
    const auto registerProvider = reinterpret_cast<RegisterFrameInputProviderFn>(GetProcAddress(module, "AsteriaRegisterFrameInputProviderV1"));
    if (!registerProvider) return;
    AsteriaFrameInputRegistrationV1 info{sizeof(info), ASTERIA_FG_INPUT_API_V1, 3};
    registerProvider(&info, &early_frame_input_registration);
}

void UnregisterFrameInputProviderForDevice(api::device *device)
{
    if (!device) return;
    const auto entry = frame_input_registrations.find(reinterpret_cast<uintptr_t>(device));
    if (entry == frame_input_registrations.end()) return;
    if (const auto module = FrameInputLayerModule())
        if (const auto unregisterProvider = reinterpret_cast<UnregisterFrameInputProviderFn>(GetProcAddress(module, "AsteriaUnregisterFrameInputProviderV1")))
            unregisterProvider(entry->second);
    frame_input_registrations.erase(entry);
}

std::shared_ptr<asteria_native_uniform::Sampler> NativeUniformSampler()
{
    return std::atomic_load_explicit(&native_uniform, std::memory_order_acquire);
}

void Limit(Device &device, LimitReason reason, uint64_t amount = 1)
{
    device.limit_hits += amount;
    device.limit_reasons[static_cast<size_t>(reason)] += amount;
    if (auto sampler = NativeUniformSampler()) sampler->MarkAssociationCoverageLoss(device.owner, "input_resource_observer_limit");
}

bool TextureDescriptor(api::descriptor_type type)
{
    return type == api::descriptor_type::sampler_with_resource_view || type == api::descriptor_type::shader_resource_view ||
           type == api::descriptor_type::unordered_access_view;
}

bool FloatRg(api::format format)
{
    return format == api::format::r16g16_float || format == api::format::r32g32_float;
}

std::string Hex(uint64_t value)
{
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << value;
    return text.str();
}

const char *ApiName(api::device_api value)
{
    switch (value)
    {
    case api::device_api::d3d9: return "D3D9";
    case api::device_api::d3d10: return "D3D10";
    case api::device_api::d3d11: return "D3D11";
    case api::device_api::d3d12: return "D3D12";
    case api::device_api::vulkan: return "Vulkan";
    case api::device_api::opengl: return "OpenGL";
    default: return "Unknown";
    }
}

const char *FormatName(api::format value)
{
    return asteria_native_input::ApiFormatName(value);
}

bool Texture(const api::resource_desc &desc)
{
    return desc.type != api::resource_type::unknown && desc.type != api::resource_type::buffer;
}

Device *FindDevice(api::device *device)
{
    auto found = devices.find(device);
    if (found != devices.end()) return &found->second;
    if (devices.size() >= MaxDevices) { ++device_limit_hits; return nullptr; }
    Device &state = devices[device];
    state.api = device->get_api();
    state.owner = reinterpret_cast<uintptr_t>(device);
    state.native = device->get_native();
    return &state;
}

Command *FindCommand(Device &state, api::command_list *list)
{
    auto found = state.commands.find(list);
    if (found != state.commands.end()) return &found->second;
    if (state.commands.size() >= MaxCommands) { Limit(state, LimitReason::CommandLists); return nullptr; }
    auto &command = state.commands[list];
    state.peak_commands = std::max<uint64_t>(state.peak_commands, state.commands.size());
    return &command;
}

void PruneTables(Device &state, bool pressure)
{
    if (!pressure && state.last_table_gc_frame == state.frame) return;
    state.last_table_gc_frame = state.frame;
    std::unordered_set<uint64_t> referenced;
    for (const auto &entry : state.commands)
    {
        for (const auto &binding : entry.second.bindings.tables) referenced.insert(binding.second);
        for (const auto &binding : entry.second.saved.tables) referenced.insert(binding.second);
    }
    for (auto entry = state.tables.begin(); entry != state.tables.end();)
    {
        const bool old = state.frame > entry->second.last_touched_frame &&
                         state.frame - entry->second.last_touched_frame > TableRetentionFrames;
        if (old && !referenced.count(entry->first)) { entry = state.tables.erase(entry); ++state.descriptor_tables_pruned; }
        else ++entry;
    }
}

Table *FindTable(Device &state, uint64_t handle)
{
    auto found = state.tables.find(handle);
    if (found != state.tables.end()) { found->second.last_touched_frame = state.frame; return &found->second; }
    if (state.tables.size() >= MaxTables) PruneTables(state, true);
    if (state.tables.size() >= MaxTables) { Limit(state, LimitReason::DescriptorTables); return nullptr; }
    Table &table = state.tables[handle]; table.last_touched_frame = state.frame;
    state.peak_tables = std::max<uint64_t>(state.peak_tables, state.tables.size());
    return &table;
}

Resource *TrackResource(Device &state, api::device *device, api::resource handle, const api::resource_desc *known = nullptr)
{
    if (!handle.handle) return nullptr;
    auto found = state.resources.find(handle.handle);
    if (found != state.resources.end()) return &found->second;
    if (state.resources.size() >= MaxResources) { Limit(state, LimitReason::Resources); return nullptr; }
    const auto desc = known ? *known : device->get_resource_desc(handle);
    if (!Texture(desc)) return nullptr;
    Resource &value = state.resources[handle.handle];
    value.desc = desc;
    value.first_frame = state.frame;
    return &value;
}

View *TrackView(Device &state, api::device *device, api::resource_view handle)
{
    if (!handle.handle) return nullptr;
    auto found = state.views.find(handle.handle);
    if (found != state.views.end()) return &found->second;
    if (state.views.size() >= MaxViews) { Limit(state, LimitReason::Views); return nullptr; }
    const api::resource resource = device->get_resource_from_view(handle);
    if (!TrackResource(state, device, resource)) return nullptr;
    View &view = state.views[handle.handle];
    view.resource = resource.handle;
    view.desc = device->get_resource_view_desc(handle);
    return &view;
}

void RemoveViewReferences(Device &state, uint64_t handle)
{
    auto erase = [handle](DescriptorSlots &slots) {
        for (auto entry = slots.begin(); entry != slots.end();)
            if (entry->second.view == handle) entry = slots.erase(entry); else ++entry;
    };
    auto remove = [&](Bindings &bindings) {
        bindings.targets.erase(std::remove(bindings.targets.begin(), bindings.targets.end(), handle), bindings.targets.end());
        if (bindings.depth == handle) bindings.depth = 0;
        erase(bindings.pushed);
    };
    for (auto &entry : state.commands) { remove(entry.second.bindings); remove(entry.second.saved); }
    for (auto &entry : state.tables) erase(entry.second.descriptors);
    state.views.erase(handle);
}

bool Excluded(Device &state, const Command *command = nullptr)
{
    const auto thread = state.effect_threads.find(GetCurrentThreadId());
    return (thread != state.effect_threads.end() && thread->second) || (command && command->effect_depth);
}

void ObserveBindings(Device &state, api::device *device, const Bindings &bindings, bool dispatch)
{
    std::unordered_set<uint64_t> reads, writes, seen;
    auto observe = [&](uint64_t handle, bool write, int role) {
        View *view = TrackView(state, device, api::resource_view{handle});
        if (!view) return;
        auto resource = state.resources.find(view->resource);
        if (resource == state.resources.end()) return;
        Resource &value = resource->second;
        const uint32_t view_format = static_cast<uint32_t>(view->desc.format);
        if (value.observed_view_formats.size() < 32 || value.observed_view_formats.count(view_format)) value.observed_view_formats.insert(view_format);
        else Limit(state, LimitReason::ViewFormats);
        if (role == 0) view->observed_usage |= api::resource_usage::render_target;
        if (role == 1) view->observed_usage |= api::resource_usage::depth_stencil;
        if (role == 2) view->observed_usage |= api::resource_usage::shader_resource;
        if (role == 3) view->observed_usage |= api::resource_usage::unordered_access;
        value.last_frame = state.frame;
        if (role == 0) value.observed_render_target = true;
        if (role == 1) value.observed_depth = true;
        if (role == 2) value.observed_sampled = true;
        if (role == 3) value.observed_uav = true;
        (write ? writes : reads).insert(view->resource);
        seen.insert(view->resource);
    };
    if (!dispatch)
    {
        for (uint64_t view : bindings.targets) observe(view, true, 0);
        if (bindings.depth) observe(bindings.depth, true, 1);
    }
    for (const auto &entry : bindings.pushed)
        observe(entry.second.view, entry.second.writable, entry.second.writable ? 3 : 2);
    for (const auto &binding : bindings.tables)
    {
        auto table = state.tables.find(binding.second);
        if (table == state.tables.end()) continue;
        table->second.last_touched_frame = state.frame;
        for (const auto &entry : table->second.descriptors)
            observe(entry.second.view, entry.second.writable, entry.second.writable ? 3 : 2);
    }
    for (uint64_t handle : reads) ++state.resources[handle].pending.reads;
    for (uint64_t handle : writes) ++state.resources[handle].pending.writes;
    for (uint64_t handle : seen)
        if (dispatch) ++state.resources[handle].pending.dispatches;
        else ++state.resources[handle].pending.draws;
}

Json CountJson(const Counts &counts)
{
    return {{"read_binding_uses", counts.reads}, {"write_binding_uses", counts.writes},
            {"draw_binding_uses", counts.draws}, {"dispatch_binding_uses", counts.dispatches}};
}

Json Snapshot()
{
    Json report = {{"schema_version", 1}, {"author", "Anokianx"}, {"pid", GetCurrentProcessId()},
                   {"architecture_bits", sizeof(void *) * 8}, {"mode", "native-input-discovery"},
                   {"enabled", active}, {"session_active", !stopping}, {"resources_are_live", !stopping}, {"read_only", true},
                   {"native_fg_ready", false}, {"motion_vectors_verified", false},
                   {"unbound_native_inputs", {"motion_vectors", "camera", "jitter", "depth_convention", "frame_id"}},
                   {"snapshot_count", ++snapshot_count}, {"snapshot_interval_ms", 1000},
                   {"device_limit_hits", device_limit_hits},
                   {"pipeline_provenance_enabled", bool(pipeline_provenance)},
                   {"native_uniform_sampler_enabled", bool(NativeUniformSampler())},
                   {"count_semantics", "Observed command recording and bound-resource use per present; no pixel-write or motion semantics inference."},
                   {"limits", {{"devices", MaxDevices}, {"resources_per_device", MaxResources}, {"views_per_device", MaxViews},
                               {"command_lists_per_device", MaxCommands}, {"descriptor_tables_per_device", MaxTables},
                               {"descriptors_per_table", MaxDescriptors}, {"unreferenced_table_retention_frames", TableRetentionFrames},
                               {"pipeline_layouts_per_device", MaxLayouts}, {"parameters_per_layout", MaxLayoutParams}}},
                   {"devices", Json::array()}};
    report["heartbeat"] = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto &device : devices)
    {
        const Device &state = device.second;
        const uint64_t now_tick = GetTickCount64();
        const uint64_t frame_age = state.last_present_tick_ms && now_tick >= state.last_present_tick_ms ? now_tick - state.last_present_tick_ms : UINT64_MAX;
        Json data = {{"native_handle_hex", Hex(state.native)}, {"api", ApiName(state.api)},
                     {"frame_id_observed", state.frame}, {"last_frame_draws", state.last_draws},
                     {"total_draws", state.total_draws}, {"last_frame_dispatches", state.last_dispatches},
                     {"total_dispatches", state.total_dispatches}, {"excluded_effect_draws", state.excluded_draws},
                     {"excluded_effect_bindings", state.excluded_bindings}, {"effect_scopes", state.effect_scopes},
                     {"limit_hits", state.limit_hits}, {"resources", Json::array()}, {"views", Json::array()}};
        data["engine_camera"] = state.engine_camera;
        data["last_present_tick_ms"] = state.last_present_tick_ms;
        data["frame_observation_age_ms"] = frame_age == UINT64_MAX ? Json(nullptr) : Json(frame_age);
        data["frame_progress_fresh"] = frame_age != UINT64_MAX && frame_age <= 5000;
        data["camera_callback"] = {{"samples_started", state.camera_samples_started}, {"samples_completed", state.camera_samples_completed},
                                   {"in_flight", state.camera_samples_started != state.camera_samples_completed},
                                   {"started_tick_ms", state.camera_sample_started_tick_ms}, {"completed_tick_ms", state.camera_sample_completed_tick_ms},
                                   {"thread_id", state.camera_sample_thread}};
        data["limit_reasons"] = Json::object();
        for (size_t index = 0; index < static_cast<size_t>(LimitReason::Count); ++index)
            data["limit_reasons"][LimitNames[index]] = state.limit_reasons[index];
        data["tracking"] = {{"resources_cached", state.resources.size()}, {"views_cached", state.views.size()},
                            {"command_lists_cached", state.commands.size()}, {"descriptor_tables_cached", state.tables.size()},
                            {"descriptor_tables_peak", state.peak_tables}, {"command_lists_peak", state.peak_commands},
                            {"descriptor_tables_pruned", state.descriptor_tables_pruned},
                            {"descriptor_updates", state.descriptor_updates}, {"descriptor_table_binds", state.descriptor_table_binds},
                            {"missing_table_bindings", state.missing_table_bindings},
                            {"pipeline_layouts_cached", state.layouts.size()}, {"ignored_nontexture_descriptor_updates", state.ignored_nontexture_updates},
                            {"known_nontexture_table_binds", state.nontexture_table_binds}, {"unknown_layout_table_binds", state.unknown_layout_table_binds},
                            {"unclassified_copy_sources", state.unclassified_copy_sources},
                            {"binding_observation_without_tracking_losses", !state.limit_hits && !state.missing_table_bindings &&
                                                                            !state.unknown_layout_table_binds && !state.unclassified_copy_sources && !device_limit_hits}};
        uint64_t live_references = 0, saved_references = 0, old_references = 0;
        std::unordered_set<uint64_t> live_tables, saved_tables, referenced_tables;
        for (const auto &command : state.commands)
        {
            live_references += command.second.bindings.tables.size();
            saved_references += command.second.saved.tables.size();
            for (const auto &binding : command.second.bindings.tables) live_tables.insert(binding.second);
            for (const auto &binding : command.second.saved.tables) saved_tables.insert(binding.second);
        }
        referenced_tables = live_tables;
        referenced_tables.insert(saved_tables.begin(), saved_tables.end());
        for (uint64_t table : referenced_tables)
        {
            auto value = state.tables.find(table);
            if (value != state.tables.end() && state.frame > value->second.last_touched_frame &&
                state.frame - value->second.last_touched_frame > TableRetentionFrames) ++old_references;
        }
        data["tracking"]["references"] = {{"live_binding_entries", live_references}, {"saved_effect_binding_entries", saved_references},
                                            {"distinct_live_tables", live_tables.size()}, {"distinct_saved_tables", saved_tables.size()},
                                            {"distinct_referenced_tables", referenced_tables.size()}, {"old_referenced_cached_tables", old_references}};
        for (const auto &entry : state.resources)
        {
            const Resource &value = entry.second;
            if (!value.observed_render_target && !value.observed_depth && !value.observed_sampled && !value.observed_uav) continue;
            bool view_rg = false;
            Json view_formats = Json::array();
            for (uint32_t format : value.observed_view_formats)
            {
                view_rg |= FloatRg(static_cast<api::format>(format));
                view_formats.push_back({{"format_id", format}, {"format_name", FormatName(static_cast<api::format>(format))}});
            }
            const bool resource_rg = FloatRg(value.desc.texture.format), rg = resource_rg || view_rg;
            Json resource = {{"native_handle_hex", Hex(entry.first)}, {"native_handle_u64", entry.first},
                             {"type_id", static_cast<uint32_t>(value.desc.type)},
                             {"width", value.desc.texture.width}, {"height", value.desc.texture.height},
                             {"depth_or_layers", value.desc.texture.depth_or_layers}, {"mip_levels", value.desc.texture.levels},
                             {"format_id", static_cast<uint32_t>(value.desc.texture.format)}, {"format_name", FormatName(value.desc.texture.format)},
                             {"format_enum", "ReShade api::format"}, {"observed_view_formats", view_formats},
                             {"samples", value.desc.texture.samples}, {"usage_bits", static_cast<uint32_t>(value.desc.usage)},
                             {"memory_heap_id", static_cast<uint32_t>(value.desc.heap)},
                             {"flags_bits", static_cast<uint32_t>(value.desc.flags)}, {"first_seen_frame", value.first_frame},
                             {"last_application_use_frame", value.last_frame},
                             {"provenance", "application_event_outside_reshade_effects"},
                             {"roles", {{"render_target", value.observed_render_target}, {"depth_view", value.observed_depth},
                                        {"sampled_texture", value.observed_sampled}, {"unordered_access", value.observed_uav}}},
                             {"last_frame", CountJson(value.last)}, {"total", CountJson(value.total)},
                             {"motion_candidate", {{"format_only", rg}, {"verified", false},
                                                   {"resource_format_float_rg", resource_rg}, {"observed_view_format_float_rg", view_rg},
                                                   {"reason", rg ? "An RG float resource or observed view is present; motion semantics are unverified." : "No motion interpretation."}}}};
            data["resources"].push_back(std::move(resource));
        }
        for (const auto &entry : state.views)
        {
            auto resource = state.resources.find(entry.second.resource);
            if (resource == state.resources.end()) continue;
            const Resource &value = resource->second;
            if (!value.observed_render_target && !value.observed_depth && !value.observed_sampled && !value.observed_uav) continue;
            data["views"].push_back({{"native_handle_hex", Hex(entry.first)}, {"resource_handle_hex", Hex(entry.second.resource)},
                                     {"format_id", static_cast<uint32_t>(entry.second.desc.format)},
                                     {"format_name", FormatName(entry.second.desc.format)}, {"format_enum", "ReShade api::format"},
                                     {"motion_candidate", {{"format_only", FloatRg(entry.second.desc.format)}, {"verified", false}}},
                                     {"type_id", static_cast<uint32_t>(entry.second.desc.type)},
                                     {"first_mip_level", entry.second.desc.texture.first_level},
                                     {"mip_level_count", entry.second.desc.texture.level_count},
                                     {"first_array_layer", entry.second.desc.texture.first_layer},
                                     {"array_layer_count", entry.second.desc.texture.layer_count},
                                     {"usage_bits", static_cast<uint32_t>(entry.second.creation_usage_known ? entry.second.usage : entry.second.observed_usage)},
                                     {"usage_source", entry.second.creation_usage_known ? "creation_callback" : "observed_binding"},
                                     {"creation_usage_bits_known", entry.second.creation_usage_known}});
        }
        report["devices"].push_back(std::move(data));
    }
    return report;
}

void WriteReport(const Json &report, const std::filesystem::path &destination = output_path)
{
    try
    {
        std::filesystem::create_directories(destination.parent_path());
        auto temporary = destination; temporary += L".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << report.dump(2);
        output.close();
        if (output && MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        DeleteFileW(temporary.c_str());
    }
    catch (...) { auto temporary = destination; temporary += L".tmp"; DeleteFileW(temporary.c_str()); }
}

void Writer()
{
    std::unique_ptr<asteria_pipeline_provenance::Writer> pipeline_writer;
    if (pipeline_provenance) pipeline_writer = std::make_unique<asteria_pipeline_provenance::Writer>();
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopping)
    {
        latest = Snapshot();
        Json report = latest;
        asteria_pipeline_provenance::Batch pipeline_batch;
        if (pipeline_provenance) pipeline_batch = pipeline_provenance->Snapshot(true);
        auto uniform_sampler = NativeUniformSampler();
        lock.unlock();
        WriteReport(report);
        if (pipeline_writer) pipeline_writer->Write(output_path.parent_path() / L"Provenance", std::move(pipeline_batch));
        if (uniform_sampler) WriteReport(uniform_sampler->Snapshot(true), output_path.parent_path() / L"NativeUniform" / L"status.json");
        lock.lock();
        wake.wait_for(lock, std::chrono::seconds(1), [] { return stopping; });
    }
    if (!devices.empty()) latest = Snapshot();
    latest["session_active"] = false;
    latest["resources_are_live"] = false;
    latest["native_fg_ready"] = false;
    Json report = latest;
    asteria_pipeline_provenance::Batch pipeline_batch;
    if (pipeline_provenance) pipeline_batch = pipeline_provenance->Snapshot(false);
    auto uniform_sampler = NativeUniformSampler();
    lock.unlock();
    WriteReport(report);
    if (pipeline_writer) pipeline_writer->Write(output_path.parent_path() / L"Provenance", std::move(pipeline_batch));
    if (uniform_sampler) WriteReport(uniform_sampler->Snapshot(false), output_path.parent_path() / L"NativeUniform" / L"status.json");
}

unsigned int __stdcall WriterEntry(void *)
{
    Writer();
    return 0;
}

void OnInitDevice(api::device *device)
{
    std::lock_guard<std::mutex> lock(mutex); FindDevice(device); RegisterFrameInputProviderForDevice(device);
}
void OnDestroyDevice(api::device *device)
{
    std::lock_guard<std::mutex> lock(mutex);
    UnregisterFrameInputProviderForDevice(device);
    if (pipeline_provenance) pipeline_provenance->DestroyDevice(device);
    latest = Snapshot(); devices.erase(device);
}
void OnDestroyCommand(api::command_list *list)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (pipeline_provenance) pipeline_provenance->ResetCommand(list);
    auto device = devices.find(list->get_device());
    if (device != devices.end()) device->second.commands.erase(list);
}
void OnResetCommand(api::command_list *list)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (pipeline_provenance) pipeline_provenance->ResetCommand(list);
    Device *device = FindDevice(list->get_device());
    if (device) if (Command *command = FindCommand(*device, list)) *command = {};
}
void OnInitResource(api::device *device, const api::resource_desc &desc, const api::subresource_data *, api::resource_usage, api::resource handle)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (pipeline_provenance) pipeline_provenance->DestroyResource(device, handle.handle);
    Device *state = FindDevice(device);
    if (!state) return;
    auto resource = state->resources.find(handle.handle);
    if (resource != state->resources.end()) resource->second.desc = desc;
}
void OnDestroyResource(api::device *device, api::resource resource)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (pipeline_provenance) pipeline_provenance->DestroyResource(device, resource.handle);
    auto state = devices.find(device);
    if (state == devices.end()) return;
    std::vector<uint64_t> views;
    for (const auto &view : state->second.views)
        if (view.second.resource == resource.handle) views.push_back(view.first);
    for (uint64_t view : views) RemoveViewReferences(state->second, view);
    state->second.resources.erase(resource.handle);
}
void OnInitView(api::device *device, api::resource resource, api::resource_usage usage, const api::resource_view_desc &desc, api::resource_view handle)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(device);
    if (!state) return;
    auto view = state->views.find(handle.handle);
    if (view == state->views.end()) return;
    view->second.resource = resource.handle; view->second.desc = desc;
    view->second.usage = usage; view->second.creation_usage_known = true;
}
void OnDestroyView(api::device *device, api::resource_view view)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto state = devices.find(device);
    if (state != devices.end()) RemoveViewReferences(state->second, view.handle);
}

void OnInitLayout(api::device *device, uint32_t count, const api::pipeline_layout_param *params, api::pipeline_layout layout)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(device);
    if (!state) return;
    if (state->layouts.size() >= MaxLayouts && !state->layouts.count(layout.handle)) { Limit(*state, LimitReason::PipelineLayouts); return; }
    if (count > MaxLayoutParams) Limit(*state, LimitReason::LayoutParameters, count - MaxLayoutParams);
    std::vector<LayoutParameter> parameters(std::min<uint32_t>(count, MaxLayoutParams));
    for (uint32_t index = 0; params && index < parameters.size(); ++index)
    {
        auto &parameter = parameters[index];
        const auto &input = params[index];
        if (input.type == api::pipeline_layout_param_type::descriptor_table || input.type == api::pipeline_layout_param_type::push_descriptors_with_ranges)
        {
            const auto &table = input.descriptor_table;
            parameter.known = !table.count || table.ranges;
            if (table.count > MaxDescriptors) { Limit(*state, LimitReason::LayoutRanges, table.count - MaxDescriptors); parameter.known = false; }
            for (uint32_t range = 0; table.ranges && range < std::min<uint32_t>(table.count, MaxDescriptors); ++range)
                parameter.may_contain_textures |= TextureDescriptor(table.ranges[range].type);
        }
        else if (input.type == api::pipeline_layout_param_type::descriptor_table_with_static_samplers ||
                 input.type == api::pipeline_layout_param_type::push_descriptors_with_static_samplers)
        {
            const auto &table = input.descriptor_table_with_static_samplers;
            parameter.known = !table.count || table.ranges;
            if (table.count > MaxDescriptors) { Limit(*state, LimitReason::LayoutRanges, table.count - MaxDescriptors); parameter.known = false; }
            for (uint32_t range = 0; table.ranges && range < std::min<uint32_t>(table.count, MaxDescriptors); ++range)
                parameter.may_contain_textures |= TextureDescriptor(table.ranges[range].type);
        }
        else if (input.type == api::pipeline_layout_param_type::push_descriptors)
        {
            parameter.known = true; parameter.may_contain_textures = TextureDescriptor(input.push_descriptors.type);
        }
        else if (input.type == api::pipeline_layout_param_type::push_constants) parameter.known = true;
    }
    state->layouts[layout.handle] = std::move(parameters);
}

void OnDestroyLayout(api::device *device, api::pipeline_layout layout)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto state = devices.find(device);
    if (state != devices.end()) state->second.layouts.erase(layout.handle);
}

void BindTargets(api::command_list *list, uint32_t count, const api::resource_view *targets, api::resource_view depth)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(list->get_device());
    if (!state) return;
    Command *command = FindCommand(*state, list);
    if (!command) return;
    if (Excluded(*state, command)) { ++state->excluded_bindings; return; }
    if (count > 16) Limit(*state, LimitReason::RenderTargetRange, count - 16);
    command->bindings.targets.clear();
    for (uint32_t i = 0; targets && i < std::min<uint32_t>(count, 16); ++i)
        command->bindings.targets.push_back(targets[i].handle);
    command->bindings.depth = depth.handle;
}
void OnBeginRenderPass(api::command_list *list, uint32_t count, const api::render_pass_render_target_desc *targets, const api::render_pass_depth_stencil_desc *depth)
{
    std::array<api::resource_view, 16> views{};
    for (uint32_t i = 0; targets && i < std::min<uint32_t>(count, 16); ++i) views[i] = targets[i].view;
    BindTargets(list, count, views.data(), depth ? depth->view : api::resource_view{});
}
void OnEndRenderPass(api::command_list *list) { BindTargets(list, 0, nullptr, {}); }
void OnBindViewports(api::command_list *list, uint32_t first, uint32_t count, const api::viewport *viewports)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(list->get_device()); if (!state) return;
    Command *command = FindCommand(*state, list); if (!command || Excluded(*state, command)) return;
    command->bindings.viewport_known = first == 0 && count == 1 && viewports;
    if (command->bindings.viewport_known) command->bindings.viewport = viewports[0];
}

void UpdateDescriptors(Device &state, DescriptorSlots &slots, uint32_t stage, uint32_t parameter, const api::descriptor_table_update &update)
{
    const bool combined = update.type == api::descriptor_type::sampler_with_resource_view;
    const bool read = update.type == api::descriptor_type::shader_resource_view;
    const bool write = update.type == api::descriptor_type::unordered_access_view;
    if (!update.descriptors) return;
    if (update.count > MaxDescriptors) Limit(state, LimitReason::DescriptorRange, update.count - MaxDescriptors);
    for (uint32_t i = 0; i < std::min<uint32_t>(update.count, MaxDescriptors); ++i)
    {
        const bool array = state.api == api::device_api::vulkan;
        const std::array<uint32_t, 4> key{stage, parameter, update.binding + (array ? 0 : i), array ? update.array_offset + i : 0};
        if (!combined && !read && !write) { slots.erase(key); continue; }
        const auto view = combined ? static_cast<const api::sampler_with_resource_view *>(update.descriptors)[i].view
                                   : static_cast<const api::resource_view *>(update.descriptors)[i];
        if (!view.handle) { slots.erase(key); continue; }
        if (slots.size() >= MaxDescriptors && !slots.count(key)) { Limit(state, LimitReason::DescriptorSlots); continue; }
        slots[key] = {view.handle, write};
    }
}
bool OnUpdateDescriptors(api::device *device, uint32_t count, const api::descriptor_table_update *updates)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(device);
    if (!state || Excluded(*state)) return false;
    if (count > MaxDescriptors) Limit(*state, LimitReason::UpdateBatch, count - MaxDescriptors);
    for (uint32_t i = 0; updates && i < std::min<uint32_t>(count, MaxDescriptors); ++i)
    {
        const auto &update = updates[i];
        ++state->descriptor_updates;
        if (!TextureDescriptor(update.type) && !state->tables.count(update.table.handle)) { ++state->ignored_nontexture_updates; continue; }
        if (Table *table = FindTable(*state, update.table.handle))
            UpdateDescriptors(*state, table->descriptors, 0, 0, update);
    }
    return false;
}
bool OnCopyDescriptors(api::device *device, uint32_t count, const api::descriptor_table_copy *copies)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(device);
    if (!state || Excluded(*state)) return false;
    if (count > MaxDescriptors) Limit(*state, LimitReason::CopyBatch, count - MaxDescriptors);
    for (uint32_t index = 0; copies && index < std::min<uint32_t>(count, MaxDescriptors); ++index)
    {
        const auto &copy = copies[index];
        if (copy.count > MaxDescriptors) Limit(*state, LimitReason::DescriptorRange, copy.count - MaxDescriptors);
        auto source = state->tables.find(copy.source_table.handle);
        if (source == state->tables.end()) { ++state->unclassified_copy_sources; continue; }
        source->second.last_touched_frame = state->frame;
        std::vector<std::pair<std::array<uint32_t, 4>, Descriptor>> copied;
        const bool array = state->api == api::device_api::vulkan;
        for (uint32_t i = 0; i < std::min<uint32_t>(copy.count, MaxDescriptors); ++i)
        {
            const std::array<uint32_t, 4> source_key{0, 0, copy.source_binding + (array ? 0 : i), array ? copy.source_array_offset + i : 0};
            const std::array<uint32_t, 4> dest_key{0, 0, copy.dest_binding + (array ? 0 : i), array ? copy.dest_array_offset + i : 0};
            auto descriptor = source->second.descriptors.find(source_key);
            if (descriptor != source->second.descriptors.end()) copied.push_back({dest_key, descriptor->second});
        }
        Table *table = FindTable(*state, copy.dest_table.handle);
        if (!table) continue;
        auto &destination = table->descriptors;
        for (const auto &entry : copied)
        {
            if (destination.size() >= MaxDescriptors && !destination.count(entry.first)) { Limit(*state, LimitReason::DescriptorSlots); continue; }
            destination[entry.first] = entry.second;
        }
    }
    return false;
}
void OnPushDescriptors(api::command_list *list, api::shader_stage stages, api::pipeline_layout, uint32_t parameter, const api::descriptor_table_update &update)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(list->get_device());
    if (!state) return;
    Command *command = FindCommand(*state, list);
    if (!command) return;
    if (Excluded(*state, command)) { ++state->excluded_bindings; return; }
    UpdateDescriptors(*state, command->bindings.pushed, static_cast<uint32_t>(stages), parameter, update);
}
void OnBindTables(api::command_list *list, api::shader_stage stages, api::pipeline_layout layout, uint32_t first, uint32_t count, const api::descriptor_table *tables)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(list->get_device());
    if (!state) return;
    Command *command = FindCommand(*state, list);
    if (!command) return;
    if (Excluded(*state, command)) { ++state->excluded_bindings; return; }
    if (count > 32) Limit(*state, LimitReason::BindTableRange, count - 32);
    for (uint32_t i = 0; tables && i < std::min<uint32_t>(count, 32); ++i)
    {
        const auto definition = state->layouts.find(layout.handle);
        const bool known = definition != state->layouts.end() && first + i < definition->second.size() && definition->second[first + i].known;
        if (known && !definition->second[first + i].may_contain_textures)
        {
            ++state->nontexture_table_binds;
            for (uint32_t bit = 0; bit < 32; ++bit)
                if (static_cast<uint32_t>(stages) & (uint32_t(1) << bit)) command->bindings.tables.erase({uint32_t(1) << bit, first + i});
            continue;
        }
        if (!known) ++state->unknown_layout_table_binds;
        if (tables[i].handle)
        {
            ++state->descriptor_table_binds;
            auto table = state->tables.find(tables[i].handle);
            if (table != state->tables.end()) table->second.last_touched_frame = state->frame;
            else ++state->missing_table_bindings;
            for (uint32_t bit = 0; bit < 32; ++bit)
            {
                if (!(static_cast<uint32_t>(stages) & (uint32_t(1) << bit))) continue;
                const std::array<uint32_t, 2> key{uint32_t(1) << bit, first + i};
                if (command->bindings.tables.size() >= MaxBindings && !command->bindings.tables.count(key)) { Limit(*state, LimitReason::CommandTableBindings); continue; }
                command->bindings.tables[key] = tables[i].handle;
            }
        }
        else
            for (uint32_t bit = 0; bit < 32; ++bit)
                if (static_cast<uint32_t>(stages) & (uint32_t(1) << bit)) command->bindings.tables.erase({uint32_t(1) << bit, first + i});
    }
}

bool Draw(api::command_list *list, bool dispatch)
{
    std::unique_lock<std::mutex> lock(mutex);
    Device *state = FindDevice(list->get_device());
    if (!state) return false;
    Command *command = FindCommand(*state, list);
    if (!command) return false;
    if (Excluded(*state, command)) { ++state->excluded_draws; return false; }
    const uint64_t draw_ordinal = dispatch ? 0 : ++command->draw_ordinal;
    const uint64_t recording_sequence = dispatch ? 0 : ++state->draw_recording_sequence;
    const uint64_t recording_tick = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (dispatch) ++state->pending_dispatches; else ++state->pending_draws;
    ObserveBindings(*state, list->get_device(), command->bindings, dispatch);
    if (pipeline_provenance)
    {
        auto resource_id = [&](uint64_t view) {
            auto found = state->views.find(view);
            if (found == state->views.end()) return pipeline_provenance->ResourceId(list->get_device(), 0, nullptr, state->frame);
            auto resource = state->resources.find(found->second.resource);
            return pipeline_provenance->ResourceId(list->get_device(), found->second.resource,
                                                   resource == state->resources.end() ? nullptr : &resource->second.desc, state->frame);
        };
        std::vector<uint64_t> targets;
        uint64_t depth = 0;
        if (!dispatch)
        {
            for (uint64_t target : command->bindings.targets) targets.push_back(target ? resource_id(target) : 0);
            if (command->bindings.depth) depth = resource_id(command->bindings.depth);
        }
        pipeline_provenance->Draw(list, dispatch, targets, depth, state->frame);
    }
    auto uniform_sampler = NativeUniformSampler();
    std::unique_ptr<asteria_native_uniform::DrawContext> uniform_context;
    bool uniform_context_missing = false;
    if (uniform_sampler && !dispatch && state->api == api::device_api::vulkan && command->bindings.depth)
    {
        auto depth_view = state->views.find(command->bindings.depth);
        if (depth_view != state->views.end())
        {
            auto depth_resource = state->resources.find(depth_view->second.resource);
            if (depth_resource == state->resources.end() || !state->render_width || !state->render_height) uniform_context_missing = true;
            if (depth_resource != state->resources.end() && state->render_width && state->render_height &&
                depth_resource->second.desc.type != api::resource_type::buffer &&
                depth_resource->second.desc.texture.width == state->render_width && depth_resource->second.desc.texture.height == state->render_height)
            {
                uniform_context = std::make_unique<asteria_native_uniform::DrawContext>();
                uniform_context->owner = reinterpret_cast<uintptr_t>(list->get_device()); uniform_context->native_device = state->native;
                uniform_context->frame = state->frame; uniform_context->device_api = state->api;
                uniform_context->native_command = list->get_native(); uniform_context->draw_ordinal = draw_ordinal;
                uniform_context->recording_sequence = recording_sequence; uniform_context->recording_tick_nanoseconds = recording_tick;
                uniform_context->viewport_known = command->bindings.viewport_known;
                const auto &viewport = command->bindings.viewport;
                uniform_context->viewport = {viewport.x, viewport.y, viewport.width, viewport.height, viewport.min_depth, viewport.max_depth};
                uniform_context->render_width = state->render_width; uniform_context->render_height = state->render_height;
                uniform_context->depth = {depth_view->second.resource, depth_resource->second.desc};
                uniform_context->pipeline_provenance_enabled = bool(pipeline_provenance);
                if (pipeline_provenance) uniform_context->pipeline_record_ids = pipeline_provenance->CurrentPipelineIds(list);
                for (uint64_t target : command->bindings.targets)
                {
                    auto view = state->views.find(target); if (view == state->views.end()) { uniform_context->attachments_complete = false; continue; }
                    auto resource = state->resources.find(view->second.resource); if (resource == state->resources.end()) { uniform_context->attachments_complete = false; continue; }
                    uniform_context->targets.push_back({view->second.resource, resource->second.desc});
                }
            }
        }
        else uniform_context_missing = true;
    }
    lock.unlock();
    if (uniform_context) uniform_sampler->ObserveDraw(list, *uniform_context);
    else if (uniform_sampler && uniform_context_missing) uniform_sampler->MarkAssociationCoverageLoss(list->get_device(), "candidate_draw_context_unavailable");
    return false;
}
bool OnDraw(api::command_list *list, uint32_t, uint32_t, uint32_t, uint32_t) { return Draw(list, false); }
bool OnDrawIndexed(api::command_list *list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return Draw(list, false); }
bool OnDispatch(api::command_list *list, uint32_t, uint32_t, uint32_t) { return Draw(list, true); }

void OnInitPipeline(api::device *device, api::pipeline_layout layout, uint32_t count, const api::pipeline_subobject *subobjects, api::pipeline handle)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(device);
    if (!state || !pipeline_provenance || Excluded(*state)) return;
    pipeline_provenance->InitPipeline(device, layout, count, subobjects, handle, state->frame);
}

void OnDestroyPipeline(api::device *device, api::pipeline handle)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (pipeline_provenance) pipeline_provenance->DestroyPipeline(device, handle);
}

void OnBindPipeline(api::command_list *list, api::pipeline_stage stages, api::pipeline handle)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(list->get_device());
    if (!state || !pipeline_provenance) return;
    Command *command = FindCommand(*state, list);
    if (!command || Excluded(*state, command)) return;
    pipeline_provenance->BindPipeline(list, stages, handle);
}

void OnBeginEffects(api::effect_runtime *, api::command_list *list, api::resource_view, api::resource_view)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(list->get_device());
    if (!state) return;
    ++state->effect_depth; ++state->effect_scopes;
    ++state->effect_threads[GetCurrentThreadId()];
    if (Command *command = FindCommand(*state, list))
    {
        if (!command->effect_depth)
        {
            command->saved = command->bindings;
            if (pipeline_provenance) pipeline_provenance->BeginEffects(list);
        }
        ++command->effect_depth;
    }
}
void OnFinishEffects(api::effect_runtime *, api::command_list *list, api::resource_view, api::resource_view)
{
    std::lock_guard<std::mutex> lock(mutex);
    Device *state = FindDevice(list->get_device());
    if (!state) return;
    if (state->effect_depth) --state->effect_depth;
    auto thread = state->effect_threads.find(GetCurrentThreadId());
    if (thread != state->effect_threads.end() && thread->second && !--thread->second) state->effect_threads.erase(thread);
    if (Command *command = FindCommand(*state, list))
        if (command->effect_depth && !--command->effect_depth)
        {
            command->bindings = std::move(command->saved); command->saved = {};
            if (pipeline_provenance) pipeline_provenance->FinishEffects(list);
        }
}
void OnPresent(api::effect_runtime *runtime)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        Device *state = FindDevice(runtime->get_device());
        if (state)
        {
            ++state->camera_samples_started; state->camera_sample_started_tick_ms = GetTickCount64();
            state->camera_sample_thread = GetCurrentThreadId();
        }
    }
    uint32_t camera_width = 0, camera_height = 0;
    runtime->get_screenshot_width_and_height(&camera_width, &camera_height);
    Json camera = asteria_native_input::SampleEngineCamera(camera_width, camera_height);
    std::unique_lock<std::mutex> lock(mutex);
    Device *state = FindDevice(runtime->get_device());
    if (!state) return;
    ++state->camera_samples_completed; state->camera_sample_completed_tick_ms = GetTickCount64();
    state->last_present_tick_ms = state->camera_sample_completed_tick_ms;
    state->render_width = camera_width; state->render_height = camera_height;
    ++state->frame;
    camera["observer_frame_id"] = state->frame;
    camera["observer_callback"] = "reshade_present";
    state->engine_camera = std::move(camera);
    PruneTables(*state, false);
    state->last_draws = state->pending_draws; state->total_draws += state->pending_draws; state->pending_draws = 0;
    state->last_dispatches = state->pending_dispatches; state->total_dispatches += state->pending_dispatches; state->pending_dispatches = 0;
    for (auto &entry : state->resources)
    {
        entry.second.last = entry.second.pending;
        entry.second.total += entry.second.pending;
        entry.second.pending = {};
    }
    const uint64_t frame = state->frame;
    const uint64_t owner = reinterpret_cast<uintptr_t>(runtime->get_device());
    auto uniform_sampler = NativeUniformSampler();
    Json camera_observation = uniform_sampler ? state->engine_camera : Json(nullptr);
    lock.unlock();
    if (uniform_sampler) uniform_sampler->AdvanceFrame(owner, frame, camera_observation, camera_width, camera_height);
}

void UniformInitDevice(api::device *device) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->InitDevice(device); }
void UniformDestroyDevice(api::device *device) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->DestroyDevice(device); }
void UniformResetCommand(api::command_list *list) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->ResetCommand(list); }
void UniformInitCommand(api::command_list *list) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->InitCommand(list); }
void UniformDestroyCommand(api::command_list *list) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->DestroyCommand(list); }
void UniformExecuteCommand(api::command_queue *, api::command_list *list) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->SubmitCommand(list); }
void UniformExecuteSecondary(api::command_list *list, api::command_list *secondary)
{
    if (auto uniform_sampler = NativeUniformSampler())
    {
        uniform_sampler->UnsupportedCommand(list, "secondary_command_executed");
        uniform_sampler->UnsupportedCommand(secondary, "secondary_command_recording");
    }
}
bool UniformIndirect(api::command_list *list, api::indirect_command, api::resource, uint64_t, uint32_t, uint32_t)
{
    if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->UnsupportedCommand(list, "indirect_draw_or_dispatch");
    return false;
}
void UniformInitResource(api::device *device, const api::resource_desc &desc, const api::subresource_data *, api::resource_usage, api::resource resource)
{ if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->InitResource(device, desc, resource); }
void UniformDestroyResource(api::device *device, api::resource resource) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->DestroyResource(device, resource); }
void UniformInitLayout(api::device *device, uint32_t count, const api::pipeline_layout_param *params, api::pipeline_layout layout)
{ if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->InitLayout(device, count, params, layout); }
void UniformDestroyLayout(api::device *device, api::pipeline_layout layout) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->DestroyLayout(device, layout); }
bool UniformUpdateDescriptors(api::device *device, uint32_t count, const api::descriptor_table_update *updates)
{ if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->UpdateDescriptors(device, count, updates); return false; }
bool UniformCopyDescriptors(api::device *device, uint32_t count, const api::descriptor_table_copy *copies)
{ if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->CopyDescriptors(device, count, copies); return false; }
void UniformBindTables(api::command_list *list, api::shader_stage stages, api::pipeline_layout layout, uint32_t first, uint32_t count, const api::descriptor_table *tables)
{ if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->BindTables(list, stages, layout, first, count, tables); }
void UniformPushDescriptors(api::command_list *list, api::shader_stage stages, api::pipeline_layout layout, uint32_t parameter, const api::descriptor_table_update &update)
{ if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->PushDescriptors(list, stages, layout, parameter, update); }
void UniformInitPipeline(api::device *device, api::pipeline_layout, uint32_t count, const api::pipeline_subobject *subobjects, api::pipeline pipeline)
{ if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->InitPipeline(device, count, subobjects, pipeline); }
void UniformDestroyPipeline(api::device *device, api::pipeline pipeline) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->DestroyPipeline(device, pipeline); }
void UniformBindPipeline(api::command_list *list, api::pipeline_stage stages, api::pipeline pipeline) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->BindPipeline(list, stages, pipeline); }
void UniformBeginEffects(api::effect_runtime *, api::command_list *list, api::resource_view, api::resource_view) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->BeginEffects(list); }
void UniformFinishEffects(api::effect_runtime *, api::command_list *list, api::resource_view, api::resource_view) { if (auto uniform_sampler = NativeUniformSampler()) uniform_sampler->FinishEffects(list); }

void RegisterEvents(bool install)
{
#define ASTERIA_EVENT(name, callback) if (install) reshade::register_event<reshade::addon_event::name>(&callback); else reshade::unregister_event<reshade::addon_event::name>(&callback)
    ASTERIA_EVENT(init_device, OnInitDevice);
    ASTERIA_EVENT(destroy_device, OnDestroyDevice);
    ASTERIA_EVENT(destroy_command_list, OnDestroyCommand);
    ASTERIA_EVENT(reset_command_list, OnResetCommand);
    ASTERIA_EVENT(init_resource, OnInitResource);
    ASTERIA_EVENT(destroy_resource, OnDestroyResource);
    ASTERIA_EVENT(init_resource_view, OnInitView);
    ASTERIA_EVENT(destroy_resource_view, OnDestroyView);
    ASTERIA_EVENT(init_pipeline_layout, OnInitLayout);
    ASTERIA_EVENT(destroy_pipeline_layout, OnDestroyLayout);
    ASTERIA_EVENT(bind_render_targets_and_depth_stencil, BindTargets);
    ASTERIA_EVENT(begin_render_pass, OnBeginRenderPass);
    ASTERIA_EVENT(end_render_pass, OnEndRenderPass);
    ASTERIA_EVENT(bind_viewports, OnBindViewports);
    ASTERIA_EVENT(update_descriptor_tables, OnUpdateDescriptors);
    ASTERIA_EVENT(copy_descriptor_tables, OnCopyDescriptors);
    ASTERIA_EVENT(push_descriptors, OnPushDescriptors);
    ASTERIA_EVENT(bind_descriptor_tables, OnBindTables);
    ASTERIA_EVENT(draw, OnDraw);
    ASTERIA_EVENT(draw_indexed, OnDrawIndexed);
    ASTERIA_EVENT(dispatch, OnDispatch);
    ASTERIA_EVENT(reshade_begin_effects, OnBeginEffects);
    ASTERIA_EVENT(reshade_finish_effects, OnFinishEffects);
    ASTERIA_EVENT(reshade_present, OnPresent);
    if (pipeline_provenance)
    {
        ASTERIA_EVENT(init_pipeline, OnInitPipeline);
        ASTERIA_EVENT(destroy_pipeline, OnDestroyPipeline);
        ASTERIA_EVENT(bind_pipeline, OnBindPipeline);
    }
    if (NativeUniformSampler())
    {
        ASTERIA_EVENT(init_device, UniformInitDevice);
        ASTERIA_EVENT(destroy_device, UniformDestroyDevice);
        ASTERIA_EVENT(init_command_list, UniformInitCommand);
        ASTERIA_EVENT(destroy_command_list, UniformDestroyCommand);
        ASTERIA_EVENT(reset_command_list, UniformResetCommand);
        ASTERIA_EVENT(execute_command_list, UniformExecuteCommand);
        ASTERIA_EVENT(execute_secondary_command_list, UniformExecuteSecondary);
        ASTERIA_EVENT(draw_or_dispatch_indirect, UniformIndirect);
        ASTERIA_EVENT(init_resource, UniformInitResource);
        ASTERIA_EVENT(destroy_resource, UniformDestroyResource);
        ASTERIA_EVENT(init_pipeline_layout, UniformInitLayout);
        ASTERIA_EVENT(destroy_pipeline_layout, UniformDestroyLayout);
        ASTERIA_EVENT(update_descriptor_tables, UniformUpdateDescriptors);
        ASTERIA_EVENT(copy_descriptor_tables, UniformCopyDescriptors);
        ASTERIA_EVENT(bind_descriptor_tables, UniformBindTables);
        ASTERIA_EVENT(push_descriptors, UniformPushDescriptors);
        ASTERIA_EVENT(init_pipeline, UniformInitPipeline);
        ASTERIA_EVENT(destroy_pipeline, UniformDestroyPipeline);
        ASTERIA_EVENT(bind_pipeline, UniformBindPipeline);
        ASTERIA_EVENT(reshade_begin_effects, UniformBeginEffects);
        ASTERIA_EVENT(reshade_finish_effects, UniformFinishEffects);
    }
#undef ASTERIA_EVENT
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE module, HMODULE host)
{
    if (active) return true;
    wchar_t enabled[8]{};
    if (GetEnvironmentVariableW(L"ASTERIA_NATIVE_INPUT_PROBE", enabled, 8) != 1 || enabled[0] != L'1') return true;
    if (!reshade::register_addon(module, host)) return false;
    RegisterFrameInputProviderEarly();
    wchar_t destination[32768]{};
    DWORD size = GetEnvironmentVariableW(L"ASTERIA_NATIVE_INPUT_PROBE_OUTPUT", destination, 32768);
    if (size && size < 32768) output_path = destination;
    else
    {
        GetEnvironmentVariableW(L"LOCALAPPDATA", destination, 32768);
        output_path = std::filesystem::path(destination) / L"Danyria" / L"AsteriaRuntime" / L"NativeInputProbe" / L"status.json";
    }
    if (!output_path.is_absolute()) { reshade::unregister_addon(module, host); return false; }
    wchar_t pipeline_enabled[8]{};
    if (GetEnvironmentVariableW(L"ASTERIA_NATIVE_PIPELINE_PROBE", pipeline_enabled, 8) == 1 && pipeline_enabled[0] == L'1')
        pipeline_provenance = std::make_unique<asteria_pipeline_provenance::Collector>();
    wchar_t uniform_enabled[8]{};
    if (GetEnvironmentVariableW(L"ASTERIA_NATIVE_UNIFORM_PROBE", uniform_enabled, 8) == 1 && uniform_enabled[0] == L'1')
        std::atomic_store_explicit(&native_uniform, std::make_shared<asteria_native_uniform::Sampler>(), std::memory_order_release);
    active = true; stopping = false;
    RegisterEvents(true);
    writer_thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &WriterEntry, nullptr, 0, nullptr));
    if (!writer_thread) { RegisterEvents(false); pipeline_provenance.reset(); std::atomic_store_explicit(&native_uniform, std::shared_ptr<asteria_native_uniform::Sampler>{}, std::memory_order_release); active = false; reshade::unregister_addon(module, host); return false; }
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE module, HMODULE host)
{
    if (!active) return;
    RegisterEvents(false);
    { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
    wake.notify_all();
    if (writer_thread)
    {
        WaitForSingleObject(writer_thread, INFINITE);
        CloseHandle(writer_thread); writer_thread = nullptr;
    }
    { std::lock_guard<std::mutex> lock(mutex); devices.clear(); pipeline_provenance.reset(); std::atomic_store_explicit(&native_uniform, std::shared_ptr<asteria_native_uniform::Sampler>{}, std::memory_order_release); active = false; }
    reshade::unregister_addon(module, host);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
