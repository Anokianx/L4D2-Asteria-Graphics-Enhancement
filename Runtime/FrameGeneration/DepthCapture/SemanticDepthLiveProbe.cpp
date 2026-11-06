#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <reshade.hpp>
#include <json.hpp>
#include "ReShadeDepthSemanticBridge.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

namespace api = reshade::api;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace asteria_depth_capture;
extern "C" __declspec(dllexport) const char *NAME = "Asteria Depth Semantic Live Probe";
extern "C" __declspec(dllexport) const char *AUTHOR = "Anokianx";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Isolated Generic Depth binding and frame-timing observation; frame generation remains disabled.";

namespace
{
struct Observation
{
    ReShadeDepthSemanticBridge bridge;
    SemanticDepthBinding last;
    uint64_t frame = 0, begins = 0, finishes = 0, presents = 0, ready = 0, captures = 0;
    bool validInBegin = false, retiredAfterCallback = false, sequenceValid = true;
    unsigned phase = 0;
    Json samples = Json::array(), images = Json::array();
};
std::map<api::effect_runtime *, std::unique_ptr<Observation>> runtimes;
fs::path destination;
bool active = false;

std::string Hex(uint64_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

void Save(api::effect_runtime *runtime, Observation &state)
{
    Json output = {{"author", "Anokianx"}, {"mode", "isolated-live-reshade-depth-semantic"},
        {"process_bits", sizeof(void *) * 8}, {"backend", static_cast<uint32_t>(runtime->get_device()->get_api())},
        {"begin_effects", state.begins}, {"finish_effects", state.finishes}, {"present_callbacks", state.presents},
        {"ready_observations", state.ready}, {"sequence_valid", state.sequenceValid},
        {"last_borrow_valid_in_begin", state.validInBegin}, {"last_borrow_retired_after_callback", state.retiredAfterCallback},
        {"main_view_confirmed", false}, {"gpu_completion_confirmed_for_depth", false},
        {"depth_resource_owned", false}, {"native_fg_ready", false},
        {"observations", state.samples}, {"screenshots", state.images}};
    const fs::path temporary = destination.wstring() + L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << output.dump(2); file.close();
    if (file) MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void OnInit(api::effect_runtime *runtime) { runtimes.emplace(runtime, std::make_unique<Observation>()); }
void OnReload(api::effect_runtime *runtime)
{
    const auto found = runtimes.find(runtime);
    if (found == runtimes.end()) return;
    found->second->bridge.OnEffectsReloaded();
    const auto technique = runtime->find_technique("AsteriaDepthSemanticBridge.fx", "AsteriaDepthSemanticProbe");
    if (technique.handle) runtime->set_technique_state(technique, true);
}
void OnBegin(api::effect_runtime *runtime, api::command_list *, api::resource_view, api::resource_view)
{
    const auto found = runtimes.find(runtime);
    if (found == runtimes.end()) return;
    auto &state = *found->second;
    state.sequenceValid = state.sequenceValid && state.phase == 0;
    state.phase = 1;
    ++state.frame; ++state.begins;
    SemanticRequest request{};
    runtime->get_screenshot_width_and_height(&request.width, &request.height);
    state.bridge.BeginEffectsAfterGenericDepth(runtime, state.frame);
    state.last = state.bridge.Observe(runtime, request);
    state.validInBegin = state.last.ValidDuringBorrow();
    if (state.validInBegin) ++state.ready;
    if (state.samples.size() < 96)
    {
        Json definitions = Json::object();
        for (const auto &definition : state.last.definitions)
            definitions[definition.name] = definition.explicitValueAvailable ? Json(definition.value) : Json(nullptr);
        state.samples.push_back({{"frame_id", state.frame}, {"present_id", state.presents + 1}, {"status", state.last.status},
            {"resource", Hex(state.last.resource.handle)}, {"view", Hex(state.last.view.handle)},
            {"native_device", Hex(state.last.nativeDevice)}, {"width", state.last.width}, {"height", state.last.height},
            {"samples", state.last.samples}, {"format", static_cast<uint32_t>(state.last.format)},
            {"generic_depth_ready", state.last.genericDepthReady}, {"valid_during_callback", state.validInBegin},
            {"conventions_explicitly_known", state.last.ConventionsExplicitlyKnown()}, {"definitions", definitions}});
    }
    state.bridge.EndBorrow();
    state.retiredAfterCallback = !state.last.ValidDuringBorrow();
}
void OnFinish(api::effect_runtime *runtime, api::command_list *, api::resource_view, api::resource_view)
{
    const auto found = runtimes.find(runtime);
    if (found == runtimes.end()) return;
    auto &state = *found->second;
    state.sequenceValid = state.sequenceValid && state.phase == 1 && !state.last.ValidDuringBorrow();
    state.phase = 2; ++state.finishes;
}
void OnPresent(api::effect_runtime *runtime)
{
    const auto found = runtimes.find(runtime);
    if (found == runtimes.end()) return;
    auto &state = *found->second;
    state.sequenceValid = state.sequenceValid && (state.phase == 2 || state.begins == 0);
    state.phase = 0; ++state.presents;
    if (state.validInBegin && state.frame >= 20 && state.frame % 10 == 0 && state.captures < 4)
    {
        uint32_t width = 0, height = 0;
        runtime->get_screenshot_width_and_height(&width, &height);
        if (width && height && width <= 4096 && height <= 4096)
        {
            std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
            if (runtime->capture_screenshot(pixels.data()))
            {
                const auto name = "depth-semantic-" + std::to_string(state.frame) + ".rgba";
                const auto image = destination.parent_path() / name;
                std::ofstream file(image, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char *>(pixels.data()), pixels.size());
                if (file)
                {
                    state.images.push_back({{"frame_id", state.frame}, {"present_id", state.presents}, {"file", name}, {"width", width}, {"height", height},
                        {"channel_order", "RGBA8"}, {"capture_screenshot_completed", true}});
                    ++state.captures;
                }
            }
        }
    }
    Save(runtime, state);
}
void OnDestroyResource(api::device *, api::resource resource)
{
    for (auto &entry : runtimes) entry.second->bridge.OnResourceDestroyed(resource);
}
void OnDestroyView(api::device *, api::resource_view view)
{
    for (auto &entry : runtimes) entry.second->bridge.OnResourceViewDestroyed(view);
}
void OnDestroy(api::effect_runtime *runtime)
{
    const auto found = runtimes.find(runtime);
    if (found == runtimes.end()) return;
    found->second->bridge.OnRuntimeDestroyed();
    runtimes.erase(found);
}
void Events(bool install)
{
#define ASTERIA_EVENT(name, callback) if (install) reshade::register_event<reshade::addon_event::name>(callback); else reshade::unregister_event<reshade::addon_event::name>(callback)
    ASTERIA_EVENT(init_effect_runtime, OnInit);
    ASTERIA_EVENT(destroy_effect_runtime, OnDestroy);
    ASTERIA_EVENT(reshade_reloaded_effects, OnReload);
    ASTERIA_EVENT(reshade_begin_effects, OnBegin);
    ASTERIA_EVENT(reshade_finish_effects, OnFinish);
    ASTERIA_EVENT(reshade_present, OnPresent);
    ASTERIA_EVENT(destroy_resource, OnDestroyResource);
    ASTERIA_EVENT(destroy_resource_view, OnDestroyView);
#undef ASTERIA_EVENT
}
}
extern "C" __declspec(dllexport) bool AddonInit(HMODULE module, HMODULE host)
{
    wchar_t enabled[8]{}, path[32768]{};
    if (GetEnvironmentVariableW(L"ASTERIA_DEPTH_SEMANTIC_LIVE_PROBE", enabled, 8) != 1 || enabled[0] != L'1') return true;
    const auto length = GetEnvironmentVariableW(L"ASTERIA_DEPTH_SEMANTIC_LIVE_OUTPUT", path, 32768);
    if (!length || length >= 32768 || !fs::path(path).is_absolute() || !reshade::register_addon(module, host)) return false;
    destination = path;
    active = true; Events(true);
    return true;
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE module, HMODULE host)
{
    if (!active) return;
    Events(false); runtimes.clear(); active = false;
    reshade::unregister_addon(module, host);
}
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
