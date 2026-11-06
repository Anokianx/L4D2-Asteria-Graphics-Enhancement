#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <reshade.hpp>
#include <json.hpp>
#include "../DepthCapture/ReShadeDepthSemanticBridge.h"
#include "VulkanTextureInterop.h"
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace api = reshade::api;
namespace fs = std::filesystem;
using Json = nlohmann::json;
extern "C" __declspec(dllexport) const char *NAME = "Asteria GPU Input Snapshot";
extern "C" __declspec(dllexport) const char *AUTHOR = "Anokianx";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Owned GPU copies of ReShade color and depth; game input qualification remains separate.";

namespace
{
constexpr const char *Effect = "AsteriaInputSnapshot.fx";
constexpr const char *Technique = "AsteriaInputSnapshot";
struct Snapshot
{
    api::device *device = nullptr;
    api::command_queue *queue = nullptr;
    api::resource color{}, depth{}, readback{};
    api::resource_view sourceColorTarget{}, sourceDepthTarget{};
    uint32_t width = 0, height = 0;
    uint64_t generation = 0, frame = 0, present = 0, command = 0;
    bool recorded = false;
    bool readbackHostVisible = false;
    asteria_live_inputs::VulkanTextureIdentity nativeColor{}, nativeDepth{};
    api::resource_usage state = api::resource_usage::copy_dest;
    ~Snapshot()
    {
        if (queue) { queue->flush_immediate_command_list(); queue->wait_idle(); }
        if (readback.handle) device->destroy_resource(readback);
        if (sourceColorTarget.handle) device->destroy_resource_view(sourceColorTarget);
        if (sourceDepthTarget.handle) device->destroy_resource_view(sourceDepthTarget);
        if (depth.handle) device->destroy_resource(depth);
        if (color.handle) device->destroy_resource(color);
    }
};
struct Runtime
{
    asteria_depth_capture::ReShadeDepthSemanticBridge bridge;
    std::unique_ptr<Snapshot> snapshot;
    uint64_t begins = 0, presents = 0, techniqueRenders = 0, completed = 0, reloads = 0;
    uint64_t semanticResource = 0, semanticView = 0;
    bool depthReady = false, borrowRetired = true;
    std::string issue;
    Json captures = Json::array();
};
std::map<api::effect_runtime *, std::unique_ptr<Runtime>> Runtimes;
std::recursive_mutex RuntimeMutex;
std::atomic<uint64_t> Generation{0};
fs::path Destination;
bool Active = false;
bool InvalidateSourceFixture = false;

void Require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

const char *HeapName(api::memory_heap heap)
{
    switch (heap)
    {
    case api::memory_heap::gpu_only:return "gpu_only";
    case api::memory_heap::cpu_to_gpu:return "cpu_to_gpu";
    case api::memory_heap::gpu_to_cpu:return "gpu_to_cpu";
    case api::memory_heap::cpu_only:return "cpu_only";
    case api::memory_heap::custom:return "custom";
    default:return "unknown";
    }
}

void Save(api::effect_runtime *runtime, Runtime &state)
{
    Json report = {{"author", "Anokianx"}, {"schema_version", 1}, {"process_bits", sizeof(void *) * 8},
        {"backend", uint32_t(runtime->get_device()->get_api())}, {"begin_effects", state.begins},
        {"technique_renders", state.techniqueRenders}, {"completed_readbacks", state.completed}, {"effects_reloads", state.reloads},
        {"present_callbacks", state.presents},
        {"last_depth_ready", state.depthReady}, {"borrow_retired_after_callback", state.borrowRetired}, {"last_issue", state.issue},
        {"private_gpu_color_and_depth", bool(state.snapshot)}, {"depth_format", "R32_FLOAT"}, {"color_format", "RGBA8_UNORM"},
        {"observer_sequences_only", true}, {"source_semantic_identity_observed_during_begin_only", true},
        {"controlled_single_graphics_queue_host_only", true},
        {"readback_heap", "cpu_to_gpu_host_coherent_in_pinned_ReShade_Vulkan"},
        {"source_outputs_overwritten_after_private_copy_fixture", InvalidateSourceFixture},
        {"source_main_view_verified", false}, {"camera_association_verified", false}, {"hud_absence_verified", false},
        {"motion_producer_connected", false}, {"frame_input_provider_registered", false}, {"game_frame_generation", false},
        {"captures", state.captures}};
    const fs::path temporary = Destination.wstring() + L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << report.dump(2); output.close();
    if (output) MoveFileExW(temporary.c_str(), Destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

api::resource Texture(api::effect_runtime *runtime, const char *name, api::format format, uint32_t width, uint32_t height)
{
    const auto variable = runtime->find_texture_variable(Effect, name);
    api::resource_view view{}, srgb{};
    Require(variable.handle != 0, "Capture effect texture is missing");
    runtime->get_texture_binding(variable, &view, &srgb);
    Require(view.handle != 0, "Capture effect texture binding is missing");
    auto *device = runtime->get_device();
    const auto resource = device->get_resource_from_view(view);
    const auto desc = device->get_resource_desc(resource);
    const auto viewDesc = device->get_resource_view_desc(view);
    Require(resource.handle && desc.type == api::resource_type::texture_2d && desc.texture.width == width &&
        desc.texture.height == height && desc.texture.samples == 1 && desc.texture.levels == 1 &&
        desc.texture.depth_or_layers == 1 && api::format_to_typeless(desc.texture.format) == api::format_to_typeless(format) &&
        viewDesc.format == format, "Capture texture type, format or extent mismatch");
    return resource;
}

std::unique_ptr<Snapshot> Allocate(api::effect_runtime *runtime, uint32_t width, uint32_t height)
{
    Require(width && height && width <= 8192 && height <= 8192, "Input snapshot extent rejected");
    auto result = std::make_unique<Snapshot>();
    result->device = runtime->get_device(); result->queue = runtime->get_command_queue();
    result->width = width; result->height = height; result->generation = ++Generation;
    const auto usage = api::resource_usage::copy_source | api::resource_usage::copy_dest | api::resource_usage::shader_resource;
    const api::resource_desc color(width, height, 1, 1, api::format::r8g8b8a8_unorm, 1, api::memory_heap::gpu_only, usage);
    const api::resource_desc depth(width, height, 1, 1, api::format::r32_float, 1, api::memory_heap::gpu_only, usage);
    Require(result->device->create_resource(color, nullptr, api::resource_usage::copy_dest, &result->color), "Owned GPU color allocation failed");
    Require(result->device->create_resource(depth, nullptr, api::resource_usage::copy_dest, &result->depth), "Owned GPU depth allocation failed");
    Require(asteria_live_inputs::DescribeVulkanTexture(result->device,result->queue,result->color,color,api::format::r8g8b8a8_unorm,width,height,result->nativeColor) &&
        asteria_live_inputs::DescribeVulkanTexture(result->device,result->queue,result->depth,depth,api::format::r32_float,width,height,result->nativeDepth),
        "Private Vulkan native image mapping contract failed");
    const api::resource_desc readback(uint64_t(width) * height * (InvalidateSourceFixture ? 16 : 8), api::memory_heap::cpu_to_gpu, api::resource_usage::copy_dest);
    Require(result->device->create_resource(readback, nullptr, api::resource_usage::copy_dest, &result->readback), "Owned readback allocation failed");
    return result;
}

void OnInit(api::effect_runtime *runtime)
{
    std::lock_guard<std::recursive_mutex> lock(RuntimeMutex);
    Runtimes.emplace(runtime, std::make_unique<Runtime>());
}
void OnReload(api::effect_runtime *runtime)
{
    std::lock_guard<std::recursive_mutex> lock(RuntimeMutex);
    const auto found = Runtimes.find(runtime); if (found == Runtimes.end()) return;
    auto &state = *found->second;
    state.snapshot.reset(); state.bridge.OnEffectsReloaded(); ++state.reloads;
    const auto technique = runtime->find_technique(Effect, Technique);
    if (technique.handle) runtime->set_technique_state(technique, true);
}
void OnBegin(api::effect_runtime *runtime, api::command_list *, api::resource_view, api::resource_view)
{
    std::lock_guard<std::recursive_mutex> lock(RuntimeMutex);
    const auto found = Runtimes.find(runtime); if (found == Runtimes.end()) return;
    auto &state = *found->second; ++state.begins;
    asteria_depth_capture::SemanticRequest request{};
    request.effectName = Effect; request.textureName = "AsteriaInputDepth"; request.readyName = "AsteriaInputDepthReady";
    runtime->get_screenshot_width_and_height(&request.width, &request.height);
    state.bridge.BeginEffectsAfterGenericDepth(runtime, state.begins);
    const auto binding = state.bridge.Observe(runtime, request);
    state.depthReady = binding.ValidDuringBorrow();
    state.semanticResource = binding.resource.handle; state.semanticView = binding.view.handle;
    state.bridge.EndBorrow(); state.borrowRetired = !binding.ValidDuringBorrow();
}
void OnTechnique(api::effect_runtime *runtime, api::effect_technique technique, api::command_list *list, api::resource_view, api::resource_view)
{
    std::lock_guard<std::recursive_mutex> lock(RuntimeMutex);
    const auto found = Runtimes.find(runtime); if (found == Runtimes.end()) return;
    const auto expected = runtime->find_technique(Effect, Technique);
    if (!expected.handle || technique.handle != expected.handle) return;
    auto &state = *found->second; ++state.techniqueRenders;
    try
    {
        Require(runtime->get_device()->get_api() == api::device_api::vulkan, "Owned input snapshots currently require Vulkan");
        Require(list && list == runtime->get_command_queue()->get_immediate_command_list(), "Capture command list is not the runtime queue's immediate list");
        uint32_t width = 0, height = 0; runtime->get_screenshot_width_and_height(&width, &height);
        const auto color = Texture(runtime, "AsteriaCapturedColor", api::format::r8g8b8a8_unorm, width, height);
        const auto depth = Texture(runtime, "AsteriaCapturedDepth", api::format::r32_float, width, height);
        if (!state.snapshot || state.snapshot->width != width || state.snapshot->height != height) state.snapshot = Allocate(runtime, width, height);
        auto &snapshot = *state.snapshot;
        Require(!snapshot.recorded, "Owned snapshot was reused before completion");
        if (InvalidateSourceFixture)
        {
            if (!snapshot.sourceColorTarget.handle)
            {
                const api::resource_view_desc colorTarget(api::resource_view_type::texture_2d, api::format::r8g8b8a8_unorm, 0, 1, 0, 1);
                const api::resource_view_desc depthTarget(api::resource_view_type::texture_2d, api::format::r32_float, 0, 1, 0, 1);
                Require(snapshot.device->create_resource_view(color, api::resource_usage::render_target, colorTarget, &snapshot.sourceColorTarget) &&
                    snapshot.device->create_resource_view(depth, api::resource_usage::render_target, depthTarget, &snapshot.sourceDepthTarget),
                    "Source-overwrite fixture target view creation failed");
            }
        }
        const api::resource source[] = {color, depth}, destination[] = {snapshot.color, snapshot.depth};
        const api::resource_usage sampled[] = {api::resource_usage::shader_resource, api::resource_usage::shader_resource};
        const api::resource_usage copySource[] = {api::resource_usage::copy_source, api::resource_usage::copy_source};
        const api::resource_usage previous[] = {snapshot.state, snapshot.state};
        const api::resource_usage copyDestination[] = {api::resource_usage::copy_dest, api::resource_usage::copy_dest};
        if (snapshot.readbackHostVisible) list->barrier(snapshot.readback, api::resource_usage::cpu_access, api::resource_usage::copy_dest);
        list->barrier(2, source, sampled, copySource);
        if (snapshot.state != api::resource_usage::copy_dest) list->barrier(2, destination, previous, copyDestination);
        for (uint32_t index = 0; index < 2; ++index) list->copy_texture_region(source[index], 0, nullptr, destination[index], 0, nullptr);
        list->barrier(2, source, copySource, sampled);
        list->barrier(2, destination, copyDestination, copySource);
        if (InvalidateSourceFixture)
        {
            const api::resource_usage targets[] = {api::resource_usage::render_target, api::resource_usage::render_target};
            list->barrier(2, source, sampled, targets);
            const float sentinel[4] = {.9375f, .0625f, .5f, 1};
            list->clear_render_target_view(snapshot.sourceColorTarget, sentinel, 0, nullptr);
            list->clear_render_target_view(snapshot.sourceDepthTarget, sentinel, 0, nullptr);
            list->barrier(2, source, targets, copySource);
            list->copy_texture_to_buffer(color, 0, nullptr, snapshot.readback, uint64_t(width) * height * 8, width, height);
            list->copy_texture_to_buffer(depth, 0, nullptr, snapshot.readback, uint64_t(width) * height * 12, width, height);
            list->barrier(2, source, copySource, sampled);
        }
        list->copy_texture_to_buffer(snapshot.color, 0, nullptr, snapshot.readback, 0, width, height);
        list->copy_texture_to_buffer(snapshot.depth, 0, nullptr, snapshot.readback, uint64_t(width) * height * 4, width, height);
        list->barrier(snapshot.readback, api::resource_usage::copy_dest, api::resource_usage::cpu_access);
        list->barrier(2, destination, copySource, sampled);
        snapshot.readbackHostVisible = true;
        snapshot.state = api::resource_usage::shader_resource;
        snapshot.frame = state.begins; snapshot.present = state.presents + 1;
        snapshot.command = list->get_native(); snapshot.recorded = true;
        state.issue.clear();
    }
    catch (const std::exception &error) { state.issue = error.what(); }
}
void OnPresent(api::effect_runtime *runtime)
{
    std::lock_guard<std::recursive_mutex> lock(RuntimeMutex);
    const auto found = Runtimes.find(runtime); if (found == Runtimes.end()) return;
    auto &state = *found->second;
    ++state.presents;
    auto *snapshot = state.snapshot.get();
    if (snapshot && snapshot->recorded)
    {
        try
        {
            snapshot->queue->flush_immediate_command_list(); snapshot->queue->wait_idle();
            void *mapped = nullptr;
            const size_t bytes = size_t(snapshot->width) * snapshot->height * 4;
            if (snapshot->sourceColorTarget.handle) snapshot->device->destroy_resource_view(snapshot->sourceColorTarget);
            if (snapshot->sourceDepthTarget.handle) snapshot->device->destroy_resource_view(snapshot->sourceDepthTarget);
            snapshot->sourceColorTarget = {}; snapshot->sourceDepthTarget = {};
            std::vector<uint8_t> color(bytes), depth(bytes), sourceColor(InvalidateSourceFixture ? bytes : 0), sourceDepth(InvalidateSourceFixture ? bytes : 0);
            Require(snapshot->device->map_buffer_region(snapshot->readback, 0, bytes * (InvalidateSourceFixture ? 4 : 2), api::map_access::read_only, &mapped) && mapped,
                "Completed input snapshot mapping failed");
            std::memcpy(color.data(), mapped, bytes); std::memcpy(depth.data(), static_cast<uint8_t *>(mapped) + bytes, bytes);
            if (InvalidateSourceFixture)
            {
                std::memcpy(sourceColor.data(), static_cast<uint8_t *>(mapped) + bytes * 2, bytes);
                std::memcpy(sourceDepth.data(), static_cast<uint8_t *>(mapped) + bytes * 3, bytes);
            }
            snapshot->device->unmap_buffer_region(snapshot->readback);
            ++state.completed;
            if (state.captures.size() < 6 && snapshot->frame >= 40 && snapshot->frame % 10 == 0)
            {
                const auto name = "snapshot_" + std::to_string(snapshot->generation) + "_" + std::to_string(snapshot->frame);
                std::vector<uint8_t> display(bytes);
                Require(runtime->capture_screenshot(display.data()), "Display color control capture failed");
                std::ofstream colorFile(Destination.parent_path() / (name + ".rgba8"), std::ios::binary);
                std::ofstream depthFile(Destination.parent_path() / (name + ".r32f"), std::ios::binary);
                std::ofstream displayFile(Destination.parent_path() / (name + ".display.rgba8"), std::ios::binary);
                colorFile.write(reinterpret_cast<const char *>(color.data()), color.size());
                depthFile.write(reinterpret_cast<const char *>(depth.data()), depth.size());
                displayFile.write(reinterpret_cast<const char *>(display.data()), display.size());
                Require(bool(colorFile) && bool(depthFile) && bool(displayFile), "Input snapshot evidence write failed");
                if (InvalidateSourceFixture)
                {
                    std::ofstream sourceColorFile(Destination.parent_path() / (name + ".source.rgba8"), std::ios::binary);
                    std::ofstream sourceDepthFile(Destination.parent_path() / (name + ".source.r32f"), std::ios::binary);
                    sourceColorFile.write(reinterpret_cast<const char *>(sourceColor.data()), sourceColor.size());
                    sourceDepthFile.write(reinterpret_cast<const char *>(sourceDepth.data()), sourceDepth.size());
                    Require(bool(sourceColorFile) && bool(sourceDepthFile), "Source-overwrite evidence write failed");
                }
                state.captures.push_back({{"begin_effects_observer_sequence", snapshot->frame}, {"present_observer_id", snapshot->present},
                    {"generation", snapshot->generation}, {"width", snapshot->width}, {"height", snapshot->height},
                    {"color_file", name + ".rgba8"}, {"depth_file", name + ".r32f"},
                    {"display_file", name + ".display.rgba8"}, {"resource_handle_domain", "ReShadeResource"},
                    {"vulkan_native_mapping_contract_checked",true},{"native_vk_device",snapshot->nativeColor.device},
                    {"native_vk_graphics_queue",snapshot->nativeColor.queue},{"native_vk_color_image",snapshot->nativeColor.image},
                    {"native_vk_depth_image",snapshot->nativeDepth.image},
                    {"native_vk_color_heap",{{"requested",HeapName(snapshot->nativeColor.requested_heap)},
                        {"queried",HeapName(snapshot->nativeColor.queried_heap)},{"queried_known",snapshot->nativeColor.queried_heap_known}}},
                    {"native_vk_depth_heap",{{"requested",HeapName(snapshot->nativeDepth.requested_heap)},
                        {"queried",HeapName(snapshot->nativeDepth.queried_heap)},{"queried_known",snapshot->nativeDepth.queried_heap_known}}},
                    {"source_color_file", InvalidateSourceFixture ? Json(name + ".source.rgba8") : Json(nullptr)},
                    {"source_depth_file", InvalidateSourceFixture ? Json(name + ".source.r32f") : Json(nullptr)}, {"generic_depth_ready", state.depthReady},
                    {"source_semantic_resource", state.semanticResource}, {"source_semantic_view", state.semanticView},
                    {"owned_color_resource", snapshot->color.handle}, {"owned_depth_resource", snapshot->depth.handle},
                    {"command_native", snapshot->command}, {"same_callback_and_command", true},
                    {"runtime_immediate_command_list_verified", true}, {"transfer_to_host_barrier_recorded", true},
                    {"readback_host_coherent_by_pinned_backend", true}, {"observer_sequences_only", true},
                    {"completed_by_flush_wait_idle_and_map", true}, {"hud_absence_verified", false}, {"main_view_verified", false}});
            }
            snapshot->recorded = false;
        }
        catch (const std::exception &error) { state.issue = error.what(); }
    }
    Save(runtime, state);
}
void OnDestroy(api::effect_runtime *runtime)
{
    std::lock_guard<std::recursive_mutex> lock(RuntimeMutex);
    const auto found = Runtimes.find(runtime); if (found == Runtimes.end()) return;
    found->second->bridge.OnRuntimeDestroyed(); Runtimes.erase(found);
}
void Events(bool install)
{
#define ASTERIA_EVENT(name, callback) if (install) reshade::register_event<reshade::addon_event::name>(callback); else reshade::unregister_event<reshade::addon_event::name>(callback)
    ASTERIA_EVENT(init_effect_runtime, OnInit);
    ASTERIA_EVENT(destroy_effect_runtime, OnDestroy);
    ASTERIA_EVENT(reshade_reloaded_effects, OnReload);
    ASTERIA_EVENT(reshade_begin_effects, OnBegin);
    ASTERIA_EVENT(reshade_render_technique, OnTechnique);
    ASTERIA_EVENT(reshade_present, OnPresent);
#undef ASTERIA_EVENT
}
}
extern "C" __declspec(dllexport) bool AddonInit(HMODULE module, HMODULE host)
{
    wchar_t enabled[8]{}, path[32768]{};
    if (GetEnvironmentVariableW(L"ASTERIA_INPUT_SNAPSHOT", enabled, 8) != 1 || enabled[0] != L'1') return true;
    if (GetEnvironmentVariableW(L"ASTERIA_INPUT_SNAPSHOT_CONTROLLED_HOST",enabled,8)!=1 || enabled[0]!=L'1') return false;
    const auto size = GetEnvironmentVariableW(L"ASTERIA_INPUT_SNAPSHOT_OUTPUT", path, 32768);
    if (!size || size >= 32768 || !fs::path(path).is_absolute() || !reshade::register_addon(module, host)) return false;
#if defined(ASTERIA_SNAPSHOT_SOURCE_OVERWRITE_FIXTURE)
    InvalidateSourceFixture = GetEnvironmentVariableW(L"ASTERIA_INPUT_SNAPSHOT_TEST_INVALIDATE_SOURCE", enabled, 8) == 1 && enabled[0] == L'1';
#endif
    Destination = path; Active = true; Events(true); return true;
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE module, HMODULE host)
{
    if (!Active) return;
    Events(false);
    { std::lock_guard<std::recursive_mutex> lock(RuntimeMutex); Runtimes.clear(); Active = false; }
    reshade::unregister_addon(module, host);
}
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
