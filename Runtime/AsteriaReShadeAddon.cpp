#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <reshade.hpp>
#include <json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include "AsteriaRuntimeMetrics.h"

using Clock = std::chrono::steady_clock;

struct EffectBinding
{
    const char *key;
    const char *file;
    const char *technique;
};

static constexpr std::array<EffectBinding, 8> s_effects = {{
    {"smaa", "AsteriaSMAA.fx", "SMAA"},
    {"cas", "AsteriaCAS.fx", "ContrastAdaptiveSharpen"},
    {"bloom", "AsteriaBloom.fx", "AsteriaBloom"},
    {"color", "AsteriaColor.fx", "AsteriaColor"},
    {"tonemap", "AsteriaTonemap.fx", "AsteriaTonemap"},
    {"vignette", "AsteriaVignette.fx", "AsteriaVignette"},
    {"deband", "AsteriaDeband.fx", "AsteriaDeband"},
    {"marker", "Asteria.fx", "AsteriaMarker"},
}};

struct EffectState
{
    reshade::api::effect_technique technique{};
    bool ready = false;
    bool active = false;
    bool rendered = false;
};

struct RuntimeState
{
    Clock::time_point last_frame{};
    Clock::time_point last_status{};
    double frame_total = 0.0;
    unsigned int frame_count = 0;
    bool enabled = false;
    double control_updated_at = 0.0;
    std::array<EffectState, s_effects.size()> effects{};
    AsteriaRuntimeMetrics metrics;
};

static std::unordered_map<reshade::api::effect_runtime *, RuntimeState> s_states;
static std::filesystem::path s_root;
static std::mutex s_mutex;

static bool IsGameProcess()
{
    static const bool game = [] {
        wchar_t executable[32768] = {};
        GetModuleFileNameW(nullptr, executable, 32768);
        return _wcsicmp(std::filesystem::path(executable).filename().c_str(), L"left4dead2.exe") == 0;
    }();
    return game;
}

static const char *ApiName(reshade::api::device_api api)
{
    switch (api)
    {
    case reshade::api::device_api::d3d9: return "D3D9";
    case reshade::api::device_api::vulkan: return "Vulkan";
    case reshade::api::device_api::d3d11: return "D3D11";
    case reshade::api::device_api::d3d12: return "D3D12";
    case reshade::api::device_api::opengl: return "OpenGL";
    default: return "Unknown";
    }
}

static bool PrepareRoot()
{
    if (!s_root.empty()) return true;
    const wchar_t *local_app_data = _wgetenv(L"LOCALAPPDATA");
    if (!local_app_data) return false;
    s_root = std::filesystem::path(local_app_data) / L"Danyria" / L"AsteriaRuntime";
    std::error_code error;
    std::filesystem::create_directories(s_root, error);
    return !error;
}

static bool SetFloat(reshade::api::effect_runtime *runtime, const char *file, const char *name, float value)
{
    const auto uniform = runtime->find_uniform_variable(file, name);
    if (!uniform.handle) return false;
    runtime->set_uniform_value_float(uniform, &value, 1);
    return true;
}

static bool SetInt(reshade::api::effect_runtime *runtime, const char *file, const char *name, int value)
{
    const auto uniform = runtime->find_uniform_variable(file, name);
    if (!uniform.handle) return false;
    runtime->set_uniform_value_int(uniform, &value, 1);
    return true;
}

static float Parameter(const nlohmann::json &options, const char *key, float fallback, float low, float high)
{
    return (std::clamp)(options.value(key, fallback), low, high);
}

static void ApplyControl(reshade::api::effect_runtime *runtime, RuntimeState &state)
{
    nlohmann::json control = nlohmann::json::object();
    try
    {
        std::ifstream input(s_root / (IsGameProcess() ? L"asteria_runtime_control.json" : L"asteria_host_test_control.json"), std::ios::binary);
        if (input) control = nlohmann::json::parse(input);
        if (!control.is_object()) control = nlohmann::json::object();
        state.enabled = control.value("enabled", false);
        state.control_updated_at = control.value("updated_at", 0.0);
        for (size_t i = 0; i < s_effects.size(); ++i)
        {
            const auto &binding = s_effects[i];
            auto &effect = state.effects[i];
            const auto options = control.value(binding.key, nlohmann::json::object());
            bool ready = true;
            bool enabled = state.enabled && options.value("enabled", false);
            const auto bind_float = [&](const char *uniform, const char *key, float fallback, float low, float high) {
                ready = SetFloat(runtime, binding.file, uniform, Parameter(options, key, fallback, low, high)) && ready;
            };
            if (i == 0)
            {
                const float strength = Parameter(options, "strength", 0.42f, 0.0f, 1.0f);
                ready = SetFloat(runtime, binding.file, "EdgeDetectionThreshold", 0.20f - strength * 0.15f);
                enabled = enabled && strength > 0.0f;
                const auto quality = options.value("quality", std::string("medium"));
                const int steps = quality == "low" ? 8 : quality == "high" ? 32 : 16;
                const int diagonal = quality == "low" ? 0 : quality == "high" ? 16 : 8;
                ready = SetInt(runtime, binding.file, "MaxSearchSteps", steps) && ready;
                ready = SetInt(runtime, binding.file, "MaxSearchStepsDiagonal", diagonal) && ready;
                ready = SetInt(runtime, binding.file, "EdgeDetectionType", 1) && ready;
                const auto predication = runtime->find_uniform_variable(binding.file, "PredicationEnabled");
                const bool use_depth = false;
                if (predication.handle) runtime->set_uniform_value_bool(predication, &use_depth, 1);
            }
            else if (i == 1)
            {
                bind_float("Sharpening", "strength", 0.35f, 0.0f, 1.0f);
                enabled = enabled && Parameter(options, "strength", 0.35f, 0.0f, 1.0f) > 0.0f;
            }
            else if (i == 2)
            {
                bind_float("AsteriaBloomStrength", "strength", 0.35f, 0.0f, 1.0f);
                bind_float("AsteriaBloomThreshold", "threshold", 0.75f, 0.0f, 1.0f);
                bind_float("AsteriaBloomRadius", "radius", 2.0f, 0.5f, 6.0f);
            }
            else if (i == 3)
            {
                bind_float("AsteriaExposure", "exposure", 0.0f, -2.0f, 2.0f);
                bind_float("AsteriaContrast", "contrast", 1.0f, 0.5f, 1.5f);
                bind_float("AsteriaSaturation", "saturation", 1.0f, 0.0f, 2.0f);
                bind_float("AsteriaTemperature", "temperature", 0.0f, -1.0f, 1.0f);
                bind_float("AsteriaVibrance", "vibrance", 0.0f, -1.0f, 1.0f);
            }
            else if (i == 4)
            {
                bind_float("AsteriaTonemapStrength", "strength", 0.5f, 0.0f, 1.0f);
                const auto mode = options.value("mode", std::string("filmic"));
                ready = SetInt(runtime, binding.file, "AsteriaTonemapMode", mode == "aces" ? 1 : mode == "reinhard" ? 2 : 0) && ready;
            }
            else if (i == 5)
            {
                bind_float("AsteriaVignetteStrength", "strength", 0.2f, 0.0f, 1.0f);
                bind_float("AsteriaVignetteRadius", "radius", 0.75f, 0.3f, 1.0f);
                bind_float("AsteriaVignetteSoftness", "softness", 0.35f, 0.05f, 1.0f);
            }
            else if (i == 6)
            {
                bind_float("AsteriaDebandStrength", "strength", 0.3f, 0.0f, 1.0f);
                bind_float("AsteriaDebandRadius", "radius", 8.0f, 1.0f, 24.0f);
                const auto quality = options.value("quality", std::string("medium"));
                ready = SetInt(runtime, binding.file, "AsteriaDebandQuality", quality == "low" ? 0 : quality == "high" ? 2 : 1) && ready;
            }
            else
                bind_float("AsteriaMarkerStrength", "strength", 0.7f, 0.0f, 1.0f);
            const auto technique = runtime->find_technique(binding.file, binding.technique);
            if (technique != effect.technique) effect.rendered = false;
            effect.technique = technique;
            effect.ready = technique.handle != 0 && ready;
            effect.active = enabled && effect.ready;
            if (technique.handle) runtime->set_technique_state(technique, effect.active);
            if (!effect.active) effect.rendered = false;
        }
    }
    catch (...)
    {
        state.enabled = false;
        state.control_updated_at = 0.0;
        for (auto &effect : state.effects)
        {
            effect.active = effect.rendered = false;
            if (effect.technique.handle) runtime->set_technique_state(effect.technique, false);
        }
    }
}

static void WriteStatus(reshade::api::effect_runtime *runtime, RuntimeState &state)
{
    const auto status = s_root / (IsGameProcess() ? L"asteria_runtime_status.json" : L"asteria_host_test_status.json");
    const auto temporary = status.wstring() + L".tmp";
    const double frame_ms = state.frame_count ? state.frame_total / state.frame_count : 0.0;
    nlohmann::json data = state.metrics.sample(runtime);
    if (IsGameProcess()) data.erase("vram_details");
    data["protocol"] = 3;
    data["connected"] = true;
    data["api"] = ApiName(runtime->get_device()->get_api());
    data["mode"] = IsGameProcess() ? "reshade-addon" : "host-test";
    data["pid"] = GetCurrentProcessId();
    data["frame_ms"] = frame_ms;
    data["fps"] = frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0;
    data["enabled"] = state.enabled;
    data["effect_ready"] = std::all_of(state.effects.begin(), state.effects.end(), [](const EffectState &effect) { return effect.ready; });
    data["smaa_ready"] = state.effects[0].ready;
    data["cas_ready"] = state.effects[1].ready;
    data["marker_rendered"] = state.effects[7].rendered;
    data["smaa_rendered"] = state.effects[0].rendered;
    data["cas_rendered"] = state.effects[1].rendered;
    for (size_t i = 0; i < s_effects.size(); ++i)
    {
        data["effects_ready"][s_effects[i].key] = state.effects[i].ready;
        data["effects_rendered"][s_effects[i].key] = state.effects[i].rendered;
    }
    data["control_updated_at"] = state.control_updated_at;
    data["heartbeat"] = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    data["render_result"] = 0;
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << data.dump();
    output.close();
    MoveFileExW(temporary.c_str(), status.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void OnPresent(reshade::api::effect_runtime *runtime)
{
    const std::lock_guard<std::mutex> lock(s_mutex);
    if (!PrepareRoot()) return;
    auto &state = s_states[runtime];
    const auto now = Clock::now();
    if (state.last_frame.time_since_epoch().count())
    {
        const double frame_ms = std::chrono::duration<double, std::milli>(now - state.last_frame).count();
        if (frame_ms < 1000.0)
        {
            state.frame_total += frame_ms;
            ++state.frame_count;
        }
        state.metrics.add_frame(frame_ms);
    }
    state.last_frame = now;
    if (!state.last_status.time_since_epoch().count() || now - state.last_status >= std::chrono::milliseconds(250))
    {
        ApplyControl(runtime, state);
        WriteStatus(runtime, state);
        state.last_status = now;
        state.frame_count = 0;
        state.frame_total = 0.0;
    }
}

static void OnDestroyRuntime(reshade::api::effect_runtime *runtime)
{
    const std::lock_guard<std::mutex> lock(s_mutex);
    s_states.erase(runtime);
}

static void OnRenderTechnique(reshade::api::effect_runtime *runtime, reshade::api::effect_technique technique,
                             reshade::api::command_list *, reshade::api::resource_view, reshade::api::resource_view)
{
    const std::lock_guard<std::mutex> lock(s_mutex);
    auto &state = s_states[runtime];
    for (auto &effect : state.effects)
        if (effect.active && technique == effect.technique) effect.rendered = true;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (!reshade::register_addon(module)) return FALSE;
        reshade::register_event<reshade::addon_event::reshade_present>(&OnPresent);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(&OnRenderTechnique);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_event<reshade::addon_event::reshade_present>(&OnPresent);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
        reshade::unregister_event<reshade::addon_event::reshade_render_technique>(&OnRenderTechnique);
        reshade::unregister_addon(module);
    }
    return TRUE;
}
