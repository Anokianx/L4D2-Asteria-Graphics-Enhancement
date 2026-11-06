#include "ReShadeDepthSemanticBridge.h"
#include <json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

namespace api = reshade::api;
using namespace asteria_depth_capture;
using Json = nlohmann::json;
static uint32_t Checks = 0;
static void Check(bool valid, const char *message) { ++Checks; if (!valid) throw std::runtime_error(message); }

struct MockDevice
{
    uint64_t native = 0x12345678;
    api::resource resource{0xFEDCBA9876543210ull};
    api::resource_desc description{1920, 1080, 1, 1, api::format::d24_unorm_s8_uint, 1, api::memory_heap::gpu_only,
                                   api::resource_usage::shader_resource | api::resource_usage::copy_dest};
    api::resource_view_desc view{api::format::d24_unorm_s8_uint};
    uint64_t get_native() const { return native; }
    api::device_api get_api() const { return api::device_api::vulkan; }
    api::resource get_resource_from_view(api::resource_view) const { return resource; }
    api::resource_desc get_resource_desc(api::resource) const { return description; }
    api::resource_view_desc get_resource_view_desc(api::resource_view) const { return view; }
};
struct MockRuntime
{
    MockDevice device;
    bool deviceMissing = false, textureMissing = false, readyMissing = false, ready = true, bindingMissing = false;
    std::string readySource = "bufready_depth";
    api::resource_view binding{0x12345678ABCDEF01ull};
    std::map<std::string, std::string> globalDefinitions, effectDefinitions;
    MockDevice *get_device() { return deviceMissing ? nullptr : &device; }
    api::effect_texture_variable find_texture_variable(const char *effect, const char *variable) const
    {
        return {!textureMissing && std::strcmp(effect, "AsteriaDepthSemanticBridge.fx") == 0 && std::strcmp(variable, "AsteriaDepthSemantic") == 0 ? 101ull : 0ull};
    }
    api::effect_uniform_variable find_uniform_variable(const char *effect, const char *variable) const
    {
        return {!readyMissing && std::strcmp(effect, "AsteriaDepthSemanticBridge.fx") == 0 && std::strcmp(variable, "AsteriaDepthReady") == 0 ? 202ull : 0ull};
    }
    bool get_annotation_string_from_uniform_variable(api::effect_uniform_variable, const char *, char *value, size_t *size) const
    {
        if (*size <= readySource.size()) return false;
        std::memcpy(value, readySource.c_str(), readySource.size() + 1); *size = readySource.size() + 1; return true;
    }
    void get_uniform_value_bool(api::effect_uniform_variable, bool *values, size_t, size_t) const { *values = ready; }
    void get_texture_binding(api::effect_texture_variable, api::resource_view *srv, api::resource_view *srgb) const
    {
        if (!bindingMissing) *srv = *srgb = binding;
    }
    bool Definition(const std::map<std::string, std::string> &definitions, const char *name, char *value, size_t *size) const
    {
        const auto found = definitions.find(name);
        if (found == definitions.end() || *size <= found->second.size()) return false;
        std::memcpy(value, found->second.c_str(), found->second.size() + 1); *size = found->second.size() + 1; return true;
    }
    bool get_preprocessor_definition_for_effect(const char *, const char *name, char *value, size_t *size) const
    { return Definition(effectDefinitions, name, value, size); }
    bool get_preprocessor_definition(const char *name, char *value, size_t *size) const
    { return Definition(globalDefinitions, name, value, size); }
};

int main()
{
    try
    {
        Check(sizeof(void *) == 4, "semantic adapter contract is x86");
        SemanticRequest request{}; request.width = 1920; request.height = 1080;
        const auto reject = [&](const std::function<void(MockRuntime &)> &mutate, const char *expected) {
            MockRuntime runtime;
            mutate(runtime);
            ReShadeDepthSemanticBridge bridge;
            bridge.BeginEffectsAfterGenericDepth(&runtime, 51);
            const auto binding = bridge.ObserveFrom(&runtime, request);
            Check(!binding.borrowable && !binding.ValidDuringBorrow() && binding.status == expected, expected);
        };
        reject([](auto &v) { v.deviceMissing = true; }, "device_or_dimensions_missing");
        reject([](auto &v) { v.device.native = 0; }, "native_device_missing");
        reject([](auto &v) { v.textureMissing = true; }, "depth_semantic_texture_not_loaded");
        reject([](auto &v) { v.readyMissing = true; }, "depth_ready_uniform_missing");
        reject([](auto &v) { v.readySource = "some_other_boolean"; }, "depth_ready_source_unverified");
        reject([](auto &v) { v.ready = false; }, "generic_depth_not_ready");
        reject([](auto &v) { v.bindingMissing = true; }, "depth_semantic_binding_missing");
        reject([](auto &v) { v.device.resource = {}; }, "depth_semantic_resource_missing");
        reject([](auto &v) { v.device.description.type = api::resource_type::buffer; }, "depth_semantic_resource_type_unsupported");
        reject([](auto &v) { v.device.view.type = api::resource_view_type::texture_2d_array; }, "depth_semantic_resource_type_unsupported");
        reject([](auto &v) { v.device.description.texture.depth_or_layers = 2; }, "depth_semantic_resource_type_unsupported");
        reject([](auto &v) { v.device.view.texture.first_level = 1; }, "depth_semantic_resource_type_unsupported");
        reject([](auto &v) { v.device.view.texture.first_layer = 1; }, "depth_semantic_resource_type_unsupported");
        reject([](auto &v) { v.device.view.texture.level_count = 0; }, "depth_semantic_resource_type_unsupported");
        reject([](auto &v) { v.device.description.texture.width = 960; }, "depth_semantic_dimensions_mismatch");
        reject([](auto &v) { v.device.description.texture.samples = 4; }, "depth_semantic_multisample_unresolved");
        reject([](auto &v) { v.device.description.usage = api::resource_usage::copy_dest; }, "depth_semantic_format_or_usage_unsupported");
        reject([](auto &v) { v.device.view.format = api::format::r8g8b8a8_unorm; }, "depth_semantic_format_or_usage_unsupported");
        reject([](auto &v) { v.device.description.texture.format = api::format::r8g8b8a8_unorm; }, "depth_semantic_format_or_usage_unsupported");
        MockRuntime runtime;
        ReShadeDepthSemanticBridge bridge;
        Check(!bridge.ObserveFrom(&runtime, request).ValidDuringBorrow(), "read outside begin-effects borrow is rejected");
        bridge.BeginEffectsAfterGenericDepth(&runtime, 0x12345678ABCDEF01ull);
        auto binding = bridge.ObserveFrom(&runtime, request);
        Check(binding.borrowable && binding.ValidDuringBorrow(), "resolved GenericDepth binding is borrowed during declared begin-effects phase");
        Check(binding.resource.handle == runtime.device.resource.handle && binding.view.handle == runtime.binding.handle &&
              binding.frameID == 0x12345678ABCDEF01ull, "resource view and frame identities preserve full 64 bits");
        Check(!binding.mainViewConfirmed && !binding.gpuCompletionConfirmed && !binding.resourceOwnershipAcquired,
              "GenericDepth binding does not claim main view synchronization or owned lifetime");
        Check(!binding.ConventionsExplicitlyKnown(), "shader default conventions are never guessed from missing runtime definitions");
        bridge.EndBorrow();
        Check(!binding.ValidDuringBorrow(), "borrow closes before resource leaves event callback");
        bridge.BeginEffectsAfterGenericDepth(&runtime, 52);
        binding = bridge.ObserveFrom(&runtime, request);
        bridge.OnResourceDestroyed({runtime.device.resource.handle ^ (1ull << 48)});
        Check(binding.ValidDuringBorrow(), "unrelated resource identity does not retire current borrow");
        bridge.OnResourceDestroyed(runtime.device.resource);
        Check(!binding.ValidDuringBorrow(), "destroy_resource retires matching borrowed resource");
        bridge.BeginEffectsAfterGenericDepth(&runtime, 53);
        binding = bridge.ObserveFrom(&runtime, request);
        bridge.OnResourceViewDestroyed(runtime.binding);
        Check(!binding.ValidDuringBorrow(), "destroy_resource_view retires matching borrowed view");
        bridge.BeginEffectsAfterGenericDepth(&runtime, 54);
        binding = bridge.ObserveFrom(&runtime, request);
        bridge.OnEffectsReloaded();
        Check(!binding.ValidDuringBorrow(), "effect reload invalidates previous texture and uniform handles");
        bridge.BeginEffectsAfterGenericDepth(&runtime, 55);
        const auto previous = bridge.ObserveFrom(&runtime, request);
        binding = bridge.ObserveFrom(&runtime, request);
        Check(!previous.ValidDuringBorrow() && binding.ValidDuringBorrow(), "new observation retires prior borrowed binding generation");
        bridge.OnRuntimeDestroyed();
        Check(!binding.ValidDuringBorrow(), "destroy_runtime retires all borrows");
        bridge.BeginEffectsAfterGenericDepth(&runtime, 56);
        Check(!bridge.ObserveFrom(&runtime, request).borrowable, "destroyed runtime cannot be reopened by stale pointer");
        for (const auto format : {api::format::r32_float, api::format::r16_unorm, api::format::d32_float, api::format::d24_unorm_s8_uint})
        {
            MockRuntime sample;
            sample.device.description.texture.format = sample.device.view.format = format;
            ReShadeDepthSemanticBridge scoped;
            scoped.BeginEffectsAfterGenericDepth(&sample, 57);
            Check(scoped.ObserveFrom(&sample, request).ValidDuringBorrow(), "supported resolved depth sample format accepted");
        }
        MockRuntime configured;
        for (const auto &definition : SemanticDepthBinding{}.definitions)
            configured.globalDefinitions[definition.name] = std::strstr(definition.name, "SCALE") ||
                std::strstr(definition.name, "MULTIPLIER") ? "1" : std::strstr(definition.name, "FAR_PLANE") ? "5000" : "0";
        ReShadeDepthSemanticBridge configuredBridge;
        configuredBridge.BeginEffectsAfterGenericDepth(&configured, 58);
        auto explicitBinding = configuredBridge.ObserveFrom(&configured, request);
        Check(explicitBinding.ConventionsExplicitlyKnown(), "explicit complete numeric depth and UV conventions are preserved");
        configured.effectDefinitions["RESHADE_DEPTH_INPUT_IS_REVERSED"] = "1";
        explicitBinding = configuredBridge.ObserveFrom(&configured, request);
        Check(explicitBinding.definitions[1].value == 1, "effect definition overrides global convention");
        configured.effectDefinitions["RESHADE_DEPTH_INPUT_IS_REVERSED"] = "2";
        Check(!configuredBridge.ObserveFrom(&configured, request).ConventionsExplicitlyKnown(), "nonboolean direction definition is not trusted");
        configured.effectDefinitions["RESHADE_DEPTH_INPUT_IS_REVERSED"] = "nan";
        Check(!configuredBridge.ObserveFrom(&configured, request).ConventionsExplicitlyKnown(), "nonfinite direction definition is not trusted");
        configured.effectDefinitions["RESHADE_DEPTH_INPUT_IS_REVERSED"] = "(1)";
        Check(!configuredBridge.ObserveFrom(&configured, request).ConventionsExplicitlyKnown(), "uninterpreted preprocessor expression remains unknown");
        SemanticDepthBinding expired;
        {
            ReShadeDepthSemanticBridge scoped;
            scoped.BeginEffectsAfterGenericDepth(&runtime, 59);
            expired = scoped.ObserveFrom(&runtime, request);
            Check(expired.ValidDuringBorrow(), "scope-owned bridge starts a borrow");
        }
        Check(!expired.ValidDuringBorrow(), "destroying bridge ends borrowed resource lifetime");
        Json report = {{"author", "Anokianx"}, {"passed", true}, {"checks", Checks}, {"cpu_api_mock_contract", true},
            {"typed_reshade_sdk_adapter_compiled", true}, {"borrowed_resources_not_owned", true}, {"main_view_always_unconfirmed", true},
            {"unknown_depth_conventions_not_guessed", true}, {"actual_game_binding_observed", false}, {"fg_ready", false}, {"game_started", false}};
        std::filesystem::create_directories("results");
        std::ofstream("results/reshade_semantic_contract.json", std::ios::binary) << report.dump(2);
        std::cout << report.dump(2) << std::endl;
        return 0;
    }
    catch (const std::exception &failure) { std::cerr << failure.what() << std::endl; return 1; }
}
