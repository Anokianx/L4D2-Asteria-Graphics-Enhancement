#pragma once
#include <reshade_api.hpp>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace asteria_depth_capture
{
namespace api = reshade::api;
struct DepthDefinition
{
    const char *name;
    float value = 0;
    bool explicitValueAvailable = false;
};
struct SemanticRequest
{
    const char *effectName = "AsteriaDepthSemanticBridge.fx";
    const char *textureName = "AsteriaDepthSemantic";
    const char *readyName = "AsteriaDepthReady";
    uint32_t width = 0, height = 0;
};
struct BorrowLifetime
{
    std::atomic<uint64_t> generation{1};
    std::atomic<bool> runtimeAlive{true};
    std::atomic<bool> beginEffectsPhaseOpen{false};
};
struct SemanticDepthBinding
{
    api::resource resource{};
    api::resource_view view{};
    api::format format = api::format::unknown;
    api::device_api backend = api::device_api::d3d9;
    uint64_t nativeDevice = 0;
    uint64_t frameID = 0;
    uint32_t width = 0, height = 0, samples = 0;
    bool genericDepthReady = false;
    bool borrowable = false;
    bool mainViewConfirmed = false;
    bool gpuCompletionConfirmed = false;
    bool resourceOwnershipAcquired = false;
    std::string status = "not_observed";
    std::array<DepthDefinition, 12> definitions{{
        {"RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN"}, {"RESHADE_DEPTH_INPUT_IS_REVERSED"},
        {"RESHADE_DEPTH_INPUT_IS_MIRRORED"}, {"RESHADE_DEPTH_INPUT_IS_LOGARITHMIC"},
        {"RESHADE_DEPTH_MULTIPLIER"}, {"RESHADE_DEPTH_INPUT_X_SCALE"}, {"RESHADE_DEPTH_INPUT_Y_SCALE"},
        {"RESHADE_DEPTH_INPUT_X_OFFSET"}, {"RESHADE_DEPTH_INPUT_Y_OFFSET"},
        {"RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET"}, {"RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET"},
        {"RESHADE_DEPTH_LINEARIZATION_FAR_PLANE"}
    }};
    uint64_t generation = 0;
    std::weak_ptr<BorrowLifetime> lifetime;
    bool ValidDuringBorrow() const
    {
        const auto current = lifetime.lock();
        return borrowable && current && current->runtimeAlive.load(std::memory_order_acquire) &&
               current->beginEffectsPhaseOpen.load(std::memory_order_acquire) &&
               current->generation.load(std::memory_order_acquire) == generation;
    }
    bool ConventionsExplicitlyKnown() const
    {
        for (size_t index = 0; index < definitions.size(); ++index)
        {
            const auto &definition = definitions[index];
            if (!definition.explicitValueAvailable) return false;
            if (index < 4 && definition.value != 0 && definition.value != 1) return false;
            if ((index == 4 || index == 5 || index == 6 || index == 11) && definition.value <= 0) return false;
        }
        return true;
    }
};

class ReShadeDepthSemanticBridge
{
    std::shared_ptr<BorrowLifetime> lifetime = std::make_shared<BorrowLifetime>();
    std::atomic<const void *> runtimeIdentity{nullptr};
    std::atomic<uint64_t> frame{0};
    std::atomic<uint64_t> lastResource{0}, lastView{0};
    void Invalidate()
    {
        lifetime->beginEffectsPhaseOpen.store(false, std::memory_order_release);
        lifetime->generation.fetch_add(1, std::memory_order_acq_rel);
    }
    static bool DepthSampleFormat(api::format format)
    {
        switch (format)
        {
        case api::format::r16_unorm: case api::format::d16_unorm:
        case api::format::d24_unorm_x8_uint: case api::format::d24_unorm_s8_uint: case api::format::r24_unorm_x8_uint:
        case api::format::d32_float: case api::format::r32_float: case api::format::d32_float_s8_uint:
        case api::format::r32_float_x8_uint: case api::format::intz: return true;
        default: return false;
        }
    }
    template<typename Runtime> static void ReadDefinitions(Runtime *runtime, const SemanticRequest &request, SemanticDepthBinding &binding)
    {
        for (auto &definition : binding.definitions)
        {
            char value[256]{};
            size_t count = sizeof(value);
            bool found = runtime->get_preprocessor_definition_for_effect(request.effectName, definition.name, value, &count);
            if (!found)
            {
                count = sizeof(value);
                found = runtime->get_preprocessor_definition(definition.name, value, &count);
            }
            if (!found || count >= sizeof(value)) continue;
            char *end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || !std::isfinite(parsed)) continue;
            while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
            if (*end != '\0') continue;
            definition.value = parsed;
            definition.explicitValueAvailable = true;
        }
    }
public:
    ReShadeDepthSemanticBridge() = default;
    ReShadeDepthSemanticBridge(const ReShadeDepthSemanticBridge &) = delete;
    ReShadeDepthSemanticBridge &operator=(const ReShadeDepthSemanticBridge &) = delete;
    ~ReShadeDepthSemanticBridge() { OnRuntimeDestroyed(); }
    void BeginEffectsAfterGenericDepth(const void *runtime, uint64_t frameID)
    {
        Invalidate();
        if (!runtime || !lifetime->runtimeAlive.load(std::memory_order_acquire)) return;
        runtimeIdentity.store(runtime, std::memory_order_release);
        frame.store(frameID, std::memory_order_release);
        lifetime->beginEffectsPhaseOpen.store(true, std::memory_order_release);
    }
    void EndBorrow() { Invalidate(); }
    void OnEffectsReloaded() { Invalidate(); lastResource.store(0); lastView.store(0); }
    void OnResourceDestroyed(api::resource resource) { if (resource.handle == lastResource.load()) Invalidate(); }
    void OnResourceViewDestroyed(api::resource_view view) { if (view.handle == lastView.load()) Invalidate(); }
    void OnRuntimeDestroyed()
    {
        lifetime->runtimeAlive.store(false, std::memory_order_release);
        Invalidate();
        runtimeIdentity.store(nullptr, std::memory_order_release);
    }

    template<typename Runtime> SemanticDepthBinding ObserveFrom(Runtime *runtime, const SemanticRequest &request)
    {
        SemanticDepthBinding binding{};
        binding.frameID = frame.load(std::memory_order_acquire);
        binding.lifetime = lifetime;
        binding.generation = lifetime->generation.load(std::memory_order_acquire);
        const auto fail = [&](const char *status) { binding.status = status; return binding; };
        if (!runtime || runtimeIdentity.load(std::memory_order_acquire) != runtime || !lifetime->runtimeAlive.load(std::memory_order_acquire) ||
            !lifetime->beginEffectsPhaseOpen.load(std::memory_order_acquire)) return fail("outside_verified_begin_effects_borrow");
        binding.generation = lifetime->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        auto *device = runtime->get_device();
        if (!device || !request.width || !request.height) return fail("device_or_dimensions_missing");
        binding.nativeDevice = device->get_native();
        binding.backend = device->get_api();
        if (!binding.nativeDevice) return fail("native_device_missing");
        const auto variable = runtime->find_texture_variable(request.effectName, request.textureName);
        if (!variable.handle) return fail("depth_semantic_texture_not_loaded");
        const auto ready = runtime->find_uniform_variable(request.effectName, request.readyName);
        if (!ready.handle) return fail("depth_ready_uniform_missing");
        char source[32]{};
        size_t sourceSize = sizeof(source);
        if (!runtime->get_annotation_string_from_uniform_variable(ready, "source", source, &sourceSize) ||
            sourceSize >= sizeof(source) || std::strcmp(source, "bufready_depth") != 0) return fail("depth_ready_source_unverified");
        runtime->get_uniform_value_bool(ready, &binding.genericDepthReady, 1, 0);
        if (!binding.genericDepthReady) return fail("generic_depth_not_ready");
        api::resource_view srgb{};
        runtime->get_texture_binding(variable, &binding.view, &srgb);
        if (!binding.view.handle) return fail("depth_semantic_binding_missing");
        binding.resource = device->get_resource_from_view(binding.view);
        if (!binding.resource.handle) return fail("depth_semantic_resource_missing");
        const auto resource = device->get_resource_desc(binding.resource);
        const auto view = device->get_resource_view_desc(binding.view);
        if (resource.type != api::resource_type::texture_2d || view.type != api::resource_view_type::texture_2d ||
            resource.texture.depth_or_layers != 1 || resource.texture.levels == 0 || view.texture.first_level != 0 || view.texture.first_layer != 0 ||
            (view.texture.level_count != 1 && view.texture.level_count != UINT32_MAX) ||
            (view.texture.layer_count != 1 && view.texture.layer_count != UINT32_MAX)) return fail("depth_semantic_resource_type_unsupported");
        binding.width = resource.texture.width;
        binding.height = resource.texture.height;
        binding.samples = resource.texture.samples;
        binding.format = view.format;
        if (binding.width != request.width || binding.height != request.height) return fail("depth_semantic_dimensions_mismatch");
        if (binding.samples != 1) return fail("depth_semantic_multisample_unresolved");
        if ((resource.usage & api::resource_usage::shader_resource) == api::resource_usage::undefined ||
            !DepthSampleFormat(view.format) || api::format_to_typeless(resource.texture.format) != api::format_to_typeless(view.format)) return fail("depth_semantic_format_or_usage_unsupported");
        ReadDefinitions(runtime, request, binding);
        lastResource.store(binding.resource.handle);
        lastView.store(binding.view.handle);
        binding.borrowable = true;
        binding.status = "borrowed_generic_depth_main_view_unconfirmed";
        return binding;
    }
    SemanticDepthBinding Observe(api::effect_runtime *runtime, const SemanticRequest &request)
    {
        return ObserveFrom(runtime, request);
    }
};
}
