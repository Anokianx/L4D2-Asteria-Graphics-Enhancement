#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <reshade.hpp>
#include <json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace asteria_pipeline_provenance
{
namespace api = reshade::api;
using Json = nlohmann::json;
constexpr size_t MaxPipelines = 4096, MaxShaderBytes = 64 * 1024 * 1024;
constexpr size_t MaxShaderSize = 4 * 1024 * 1024, MaxSubobjects = 256, MaxShaders = 32;
constexpr size_t MaxShaderReadBytes = 256 * 1024 * 1024;
constexpr size_t MaxAssociations = 8192, MaxResourceRecords = 8192, MaxCommands = 2048;
constexpr std::array<uint32_t, 9> ShaderStages = {0x8, 0x10, 0x20, 0x40, 0x80, 0x800, 0x80000, 0x100000, 0x200000};

inline std::string Hex(uint64_t value)
{
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << value;
    return text.str();
}

inline uint32_t ShaderStage(api::pipeline_subobject_type type)
{
    switch (type)
    {
    case api::pipeline_subobject_type::vertex_shader: return ShaderStages[0];
    case api::pipeline_subobject_type::hull_shader: return ShaderStages[1];
    case api::pipeline_subobject_type::domain_shader: return ShaderStages[2];
    case api::pipeline_subobject_type::geometry_shader: return ShaderStages[3];
    case api::pipeline_subobject_type::pixel_shader: return ShaderStages[4];
    case api::pipeline_subobject_type::compute_shader: return ShaderStages[5];
    case api::pipeline_subobject_type::amplification_shader: return ShaderStages[6];
    case api::pipeline_subobject_type::mesh_shader: return ShaderStages[7];
    case api::pipeline_subobject_type::raygen_shader:
    case api::pipeline_subobject_type::any_hit_shader:
    case api::pipeline_subobject_type::closest_hit_shader:
    case api::pipeline_subobject_type::miss_shader:
    case api::pipeline_subobject_type::intersection_shader:
    case api::pipeline_subobject_type::callable_shader: return ShaderStages[8];
    default: return 0;
    }
}

struct Blob
{
    uint64_t id = 0;
    uint64_t fast_key = 0;
    std::vector<uint8_t> bytes;
};

struct Shader
{
    uint32_t subobject_type = 0, stage = 0;
    std::shared_ptr<const Blob> blob;
    std::string entry_point;
    std::vector<std::array<uint32_t, 2>> specialization;
    bool complete = true;
};

struct Pipeline
{
    uint64_t id = 0, owner = 0, device_native = 0, handle = 0, layout = 0, initialized_frame = 0;
    uint32_t api_id = 0, stages = 0;
    std::vector<Shader> shaders;
    std::vector<uint32_t> target_formats;
    uint32_t depth_format = 0;
    bool live = true, observed = false, complete = true;
};

struct Resource
{
    uint64_t id = 0, owner = 0, device_native = 0, handle = 0, first_frame = 0;
    api::resource_desc desc{};
    bool live = true;
};

struct Command
{
    std::array<uint64_t, 9> current{}, saved{};
};

struct Association
{
    uint64_t device_native = 0, first_frame = 0, last_frame = 0, recorded_draws = 0, recorded_dispatches = 0;
};

struct Batch
{
    Json report;
    std::vector<std::shared_ptr<const Blob>> blobs;
};

class Collector
{
    std::vector<Pipeline> pipelines;
    std::vector<Resource> resources;
    std::vector<std::shared_ptr<const Blob>> blobs;
    std::map<uint64_t, std::vector<std::shared_ptr<const Blob>>> blob_keys;
    std::map<std::array<uint64_t, 2>, uint64_t> active_pipelines, active_resources;
    std::map<std::array<uint64_t, 2>, Command> commands;
    std::map<std::vector<uint64_t>, Association> associations;
    uint64_t shader_bytes = 0, shader_read_bytes = 0, pipeline_limit = 0, byte_limit = 0, read_limit = 0, subobject_limit = 0;
    uint64_t shader_limit = 0, entry_limit = 0, spec_limit = 0, resource_limit = 0, association_limit = 0;
    uint64_t command_limit = 0, unknown_pipeline_binds = 0, unknown_draw_pipelines = 0;
    uint64_t missing_target_resources = 0, shader_payload_missing = 0, pipeline_init_callbacks = 0;

    static uint64_t Owner(api::device *device) { return reinterpret_cast<uintptr_t>(device); }
    static std::array<uint64_t, 2> CommandKey(api::command_list *list) { return {Owner(list->get_device()), reinterpret_cast<uintptr_t>(list)}; }

    Command *FindCommand(api::command_list *list)
    {
        auto key = CommandKey(list);
        auto value = commands.find(key);
        if (value != commands.end()) return &value->second;
        if (commands.size() >= MaxCommands) { ++command_limit; return nullptr; }
        return &commands[key];
    }

    std::shared_ptr<const Blob> Capture(const api::shader_desc &desc)
    {
        if (!desc.code || !desc.code_size) { ++shader_payload_missing; return {}; }
        if (desc.code_size > MaxShaderSize) { ++byte_limit; return {}; }
        if (desc.code_size > MaxShaderReadBytes - shader_read_bytes) { ++read_limit; return {}; }
        shader_read_bytes += desc.code_size;
        const uint8_t *data = static_cast<const uint8_t *>(desc.code);
        uint64_t key = 1469598103934665603ULL;
        for (size_t index = 0; index < desc.code_size; ++index) { key ^= data[index]; key *= 1099511628211ULL; }
        auto matching = blob_keys.find(key);
        if (matching != blob_keys.end())
            for (const auto &blob : matching->second)
                if (blob->bytes.size() == desc.code_size && std::memcmp(blob->bytes.data(), data, desc.code_size) == 0) return blob;
        if (desc.code_size > MaxShaderBytes - shader_bytes) { ++byte_limit; return {}; }
        auto blob = std::make_shared<Blob>();
        blob->id = blobs.size() + 1;
        blob->fast_key = key;
        blob->bytes.assign(data, data + desc.code_size);
        shader_bytes += desc.code_size;
        blobs.push_back(blob);
        blob_keys[key].push_back(blob);
        return blob;
    }

public:
    std::array<uint64_t, 9> CurrentPipelineIds(api::command_list *list) const
    {
        const auto value = commands.find(CommandKey(list));
        return value == commands.end() ? std::array<uint64_t, 9>{} : value->second.current;
    }

    void InitPipeline(api::device *device, api::pipeline_layout layout, uint32_t count, const api::pipeline_subobject *subobjects, api::pipeline handle, uint64_t frame)
    {
        ++pipeline_init_callbacks;
        DestroyPipeline(device, handle);
        if (pipelines.size() >= MaxPipelines) { ++pipeline_limit; return; }
        Pipeline pipeline;
        pipeline.id = pipelines.size() + 1; pipeline.owner = Owner(device); pipeline.device_native = device->get_native();
        pipeline.handle = handle.handle; pipeline.layout = layout.handle; pipeline.initialized_frame = frame;
        pipeline.api_id = static_cast<uint32_t>(device->get_api());
        if (count > MaxSubobjects) { ++subobject_limit; pipeline.complete = false; }
        if (count && !subobjects) { ++shader_payload_missing; pipeline.complete = false; }
        for (uint32_t index = 0; subobjects && index < std::min<uint32_t>(count, MaxSubobjects); ++index)
        {
            const auto &subobject = subobjects[index];
            const uint32_t stage = ShaderStage(subobject.type);
            if (stage)
            {
                if (!subobject.data) { ++shader_payload_missing; pipeline.complete = false; continue; }
                if (subobject.count > MaxShaders || pipeline.shaders.size() + subobject.count > MaxShaders) { ++shader_limit; pipeline.complete = false; }
                const auto *descriptions = static_cast<const api::shader_desc *>(subobject.data);
                for (uint32_t shader_index = 0; shader_index < subobject.count && pipeline.shaders.size() < MaxShaders; ++shader_index)
                {
                    const auto &desc = descriptions[shader_index];
                    Shader shader;
                    shader.subobject_type = static_cast<uint32_t>(subobject.type); shader.stage = stage; shader.blob = Capture(desc);
                    if (!shader.blob) shader.complete = false;
                    if (desc.entry_point)
                    {
                        const size_t length = strnlen_s(desc.entry_point, 256);
                        shader.entry_point.assign(desc.entry_point, length);
                        if (length == 256) { ++entry_limit; shader.complete = false; }
                    }
                    if (desc.spec_constants > 256) { ++spec_limit; shader.complete = false; }
                    if (desc.spec_constants && (!desc.spec_constant_ids || !desc.spec_constant_values)) { ++shader_payload_missing; shader.complete = false; }
                    else for (uint32_t item = 0; item < std::min<uint32_t>(desc.spec_constants, 256); ++item)
                        shader.specialization.push_back({desc.spec_constant_ids[item], desc.spec_constant_values[item]});
                    pipeline.stages |= stage; pipeline.complete &= shader.complete;
                    pipeline.shaders.push_back(std::move(shader));
                }
            }
            else if (subobject.type == api::pipeline_subobject_type::render_target_formats && subobject.data)
            {
                const auto *formats = static_cast<const api::format *>(subobject.data);
                for (uint32_t item = 0; item < std::min<uint32_t>(subobject.count, 16); ++item) pipeline.target_formats.push_back(static_cast<uint32_t>(formats[item]));
                if (subobject.count > 16) { ++subobject_limit; pipeline.complete = false; }
            }
            else if (subobject.type == api::pipeline_subobject_type::depth_stencil_format && subobject.data && subobject.count)
                pipeline.depth_format = static_cast<uint32_t>(*static_cast<const api::format *>(subobject.data));
        }
        active_pipelines[{pipeline.owner, pipeline.handle}] = pipeline.id;
        pipelines.push_back(std::move(pipeline));
    }

    void DestroyPipeline(api::device *device, api::pipeline handle)
    {
        auto value = active_pipelines.find({Owner(device), handle.handle});
        if (value == active_pipelines.end()) return;
        const uint64_t id = value->second;
        pipelines[id - 1].live = false;
        for (auto &entry : commands) if (entry.first[0] == Owner(device))
        {
            for (auto &bound : entry.second.current) if (bound == id) bound = 0;
            for (auto &bound : entry.second.saved) if (bound == id) bound = 0;
        }
        active_pipelines.erase(value);
    }

    void BindPipeline(api::command_list *list, api::pipeline_stage stages, api::pipeline handle)
    {
        Command *command = FindCommand(list);
        if (!command) return;
        auto pipeline = active_pipelines.find({Owner(list->get_device()), handle.handle});
        if (handle.handle && pipeline == active_pipelines.end()) ++unknown_pipeline_binds;
        for (size_t index = 0; index < ShaderStages.size(); ++index)
            if (static_cast<uint32_t>(stages) & ShaderStages[index]) command->current[index] = pipeline == active_pipelines.end() ? 0 : pipeline->second;
    }

    void ResetCommand(api::command_list *list) { commands.erase(CommandKey(list)); }
    void BeginEffects(api::command_list *list) { if (Command *command = FindCommand(list)) command->saved = command->current; }
    void FinishEffects(api::command_list *list) { if (Command *command = FindCommand(list)) { command->current = command->saved; command->saved = {}; } }

    void DestroyResource(api::device *device, uint64_t handle)
    {
        auto value = active_resources.find({Owner(device), handle});
        if (value != active_resources.end()) { resources[value->second - 1].live = false; active_resources.erase(value); }
    }

    uint64_t ResourceId(api::device *device, uint64_t handle, const api::resource_desc *desc, uint64_t frame)
    {
        if (!handle || !desc) { ++missing_target_resources; return 0; }
        const std::array<uint64_t, 2> key{Owner(device), handle};
        auto value = active_resources.find(key);
        if (value != active_resources.end()) return value->second;
        if (resources.size() >= MaxResourceRecords) { ++resource_limit; return 0; }
        Resource resource;
        resource.id = resources.size() + 1; resource.owner = Owner(device); resource.device_native = device->get_native();
        resource.handle = handle; resource.desc = *desc; resource.first_frame = frame;
        active_resources[key] = resource.id;
        resources.push_back(std::move(resource));
        return resources.back().id;
    }

    void Draw(api::command_list *list, bool dispatch, const std::vector<uint64_t> &targets, uint64_t depth, uint64_t frame)
    {
        auto command = commands.find(CommandKey(list));
        std::vector<uint64_t> key{Owner(list->get_device()), dispatch ? 1ULL : 0ULL};
        bool known = false;
        for (size_t stage = 0; stage < ShaderStages.size(); ++stage)
        {
            const bool applicable = dispatch ? stage == 5 : stage != 5;
            uint64_t id = applicable && command != commands.end() ? command->second.current[stage] : 0;
            key.push_back(id);
            if (id) { pipelines[id - 1].observed = true; known |= (pipelines[id - 1].stages & ShaderStages[stage]) != 0; }
        }
        if (!known) ++unknown_draw_pipelines;
        key.push_back(depth);
        key.insert(key.end(), targets.begin(), targets.end());
        auto value = associations.find(key);
        if (value == associations.end() && associations.size() >= MaxAssociations) { ++association_limit; return; }
        Association &association = associations[key];
        if (!association.recorded_draws && !association.recorded_dispatches)
        {
            association.first_frame = frame;
            association.device_native = list->get_device()->get_native();
        }
        association.last_frame = frame;
        if (dispatch) ++association.recorded_dispatches; else ++association.recorded_draws;
    }

    void DestroyDevice(api::device *device)
    {
        const uint64_t owner = Owner(device);
        for (auto entry = active_pipelines.begin(); entry != active_pipelines.end();)
            if (entry->first[0] == owner) { pipelines[entry->second - 1].live = false; entry = active_pipelines.erase(entry); } else ++entry;
        for (auto entry = active_resources.begin(); entry != active_resources.end();)
            if (entry->first[0] == owner) { resources[entry->second - 1].live = false; entry = active_resources.erase(entry); } else ++entry;
        for (auto entry = commands.begin(); entry != commands.end();)
            if (entry->first[0] == owner) entry = commands.erase(entry); else ++entry;
    }

    Batch Snapshot(bool active) const
    {
        Batch batch;
        batch.report = {{"schema_version", 1}, {"author", "Anokianx"}, {"pid", GetCurrentProcessId()}, {"read_only", true}, {"enabled", true},
                        {"session_active", active}, {"resources_are_live", false}, {"motion_vectors_verified", false}, {"native_fg_ready", false},
                        {"count_semantics", "Recorded direct draw/dispatch and bound target association; not execution, pixel writes, indirect-draw coverage, or motion semantics."},
                        {"bytecode_origin", "Public ReShade init_pipeline payload; Vulkan shaders may be DXVK translations of original game D3D9 shaders."},
                        {"pipeline_hash_scope", "Shader byte hashes, stages, entry points, specialization constants and declared target/depth formats; not complete fixed-function pipeline state."},
                        {"limits", {{"pipeline_records", MaxPipelines}, {"unique_shader_bytes", MaxShaderBytes}, {"shader_bytes_examined", MaxShaderReadBytes}, {"single_shader_bytes", MaxShaderSize},
                                    {"association_records", MaxAssociations}, {"resource_records", MaxResourceRecords}, {"command_records", MaxCommands}}},
                        {"coverage", {{"pipeline_init_callbacks", pipeline_init_callbacks}, {"pipeline_records", pipelines.size()}, {"unique_shader_records", blobs.size()},
                                      {"unique_shader_bytes", shader_bytes}, {"shader_bytes_examined", shader_read_bytes}, {"command_records", commands.size()}, {"unknown_pipeline_binds", unknown_pipeline_binds},
                                      {"unknown_draw_pipelines", unknown_draw_pipelines}, {"missing_target_resources", missing_target_resources}, {"shader_payload_missing", shader_payload_missing},
                                      {"limits_hit", {{"pipeline_records", pipeline_limit}, {"shader_bytes_or_size", byte_limit}, {"shader_bytes_examined", read_limit}, {"subobjects_or_formats", subobject_limit},
                                                      {"shaders", shader_limit}, {"entry_points", entry_limit}, {"specialization_constants", spec_limit}, {"resource_records", resource_limit},
                                                      {"association_records", association_limit}, {"command_records", command_limit}}}}},
                        {"pipelines", Json::array()}, {"resources", Json::array()}, {"associations", Json::array()}};
        batch.report["heartbeat"] = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        batch.report["coverage"]["binding_association_without_capture_losses"] =
            !pipeline_limit && !byte_limit && !read_limit && !subobject_limit && !shader_limit && !entry_limit && !spec_limit &&
            !resource_limit && !association_limit && !command_limit && !unknown_pipeline_binds && !unknown_draw_pipelines &&
            !missing_target_resources && !shader_payload_missing;
        std::map<uint64_t, std::shared_ptr<const Blob>> used;
        for (const Pipeline &pipeline : pipelines)
        {
            if (!pipeline.observed) continue;
            Json record = {{"id", pipeline.id}, {"device_handle_hex", Hex(pipeline.device_native)}, {"device_handle_u64", pipeline.device_native}, {"api_id", pipeline.api_id},
                           {"handle_hex", Hex(pipeline.handle)}, {"handle_u64", pipeline.handle}, {"layout_handle_hex", Hex(pipeline.layout)}, {"layout_handle_u64", pipeline.layout},
                           {"initialized_observer_frame", pipeline.initialized_frame}, {"live_at_snapshot", active && pipeline.live}, {"capture_complete", pipeline.complete},
                           {"declared_target_format_ids", pipeline.target_formats}, {"declared_depth_format_id", pipeline.depth_format}, {"shaders", Json::array()}};
            for (const Shader &shader : pipeline.shaders)
            {
                record["shaders"].push_back({{"subobject_type_id", shader.subobject_type}, {"pipeline_stage_bits", shader.stage}, {"blob_id", shader.blob ? shader.blob->id : 0},
                                             {"entry_point", shader.entry_point}, {"specialization_constants", shader.specialization}, {"capture_complete", shader.complete}});
                if (shader.blob) used[shader.blob->id] = shader.blob;
            }
            batch.report["pipelines"].push_back(std::move(record));
        }
        for (const Resource &resource : resources)
            batch.report["resources"].push_back({{"id", resource.id}, {"device_handle_hex", Hex(resource.device_native)}, {"device_handle_u64", resource.device_native}, {"handle_hex", Hex(resource.handle)}, {"handle_u64", resource.handle},
                                                  {"first_observer_frame", resource.first_frame}, {"live_at_snapshot", active && resource.live}, {"type_id", static_cast<uint32_t>(resource.desc.type)},
                                                  {"width", resource.desc.texture.width}, {"height", resource.desc.texture.height}, {"format_id", static_cast<uint32_t>(resource.desc.texture.format)},
                                                  {"samples", resource.desc.texture.samples}, {"usage_bits", static_cast<uint32_t>(resource.desc.usage)}});
        for (const auto &entry : associations)
        {
            Json stage_bindings = Json::array();
            for (size_t stage = 0; stage < ShaderStages.size(); ++stage)
                if (entry.first[2 + stage]) stage_bindings.push_back({{"pipeline_stage_bits", ShaderStages[stage]}, {"pipeline_record_id", entry.first[2 + stage]}});
            batch.report["associations"].push_back({{"device_handle_hex", Hex(entry.second.device_native)}, {"device_handle_u64", entry.second.device_native},
                                                     {"operation", entry.first[1] ? "dispatch" : "draw"}, {"pipeline_stage_bindings", stage_bindings},
                                                     {"bound_depth_resource_id", entry.first[11]}, {"bound_render_target_resource_ids", std::vector<uint64_t>(entry.first.begin() + 12, entry.first.end())},
                                                     {"first_observer_frame", entry.second.first_frame}, {"last_observer_frame", entry.second.last_frame},
                                                     {"recorded_draws", entry.second.recorded_draws}, {"recorded_dispatches", entry.second.recorded_dispatches}});
        }
        for (const auto &blob : used) batch.blobs.push_back(blob.second);
        return batch;
    }
};

class Writer
{
    HMODULE bcrypt = nullptr;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    decltype(&BCryptCreateHash) create_hash = nullptr;
    decltype(&BCryptHashData) hash_data = nullptr;
    decltype(&BCryptFinishHash) finish_hash = nullptr;
    decltype(&BCryptDestroyHash) destroy_hash = nullptr;
    decltype(&BCryptCloseAlgorithmProvider) close_algorithm = nullptr;
    ULONG object_bytes = 0;
    std::map<uint64_t, Json> saved_blobs;

    std::string Digest(const void *data, size_t size)
    {
        if (!algorithm || size > ULONG_MAX) return {};
        std::vector<uint8_t> object(object_bytes);
        std::array<uint8_t, 32> digest{};
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (create_hash(algorithm, &hash, object.data(), object_bytes, nullptr, 0, 0) < 0) return {};
        const bool success = hash_data(hash, reinterpret_cast<PUCHAR>(const_cast<void *>(data)), static_cast<ULONG>(size), 0) >= 0 &&
                             finish_hash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
        destroy_hash(hash);
        if (!success) return {};
        std::ostringstream text;
        for (uint8_t byte : digest) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<uint32_t>(byte);
        return text.str();
    }

public:
    std::string HashBytes(const void *data, size_t size) { return Digest(data, size); }

    Writer()
    {
        wchar_t directory[MAX_PATH]{};
        const UINT size = GetSystemDirectoryW(directory, MAX_PATH);
        if (!size || size >= MAX_PATH) return;
        bcrypt = LoadLibraryExW((std::filesystem::path(directory) / L"bcrypt.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!bcrypt) return;
        auto open = reinterpret_cast<decltype(&BCryptOpenAlgorithmProvider)>(GetProcAddress(bcrypt, "BCryptOpenAlgorithmProvider"));
        auto property = reinterpret_cast<decltype(&BCryptGetProperty)>(GetProcAddress(bcrypt, "BCryptGetProperty"));
        create_hash = reinterpret_cast<decltype(&BCryptCreateHash)>(GetProcAddress(bcrypt, "BCryptCreateHash"));
        hash_data = reinterpret_cast<decltype(&BCryptHashData)>(GetProcAddress(bcrypt, "BCryptHashData"));
        finish_hash = reinterpret_cast<decltype(&BCryptFinishHash)>(GetProcAddress(bcrypt, "BCryptFinishHash"));
        destroy_hash = reinterpret_cast<decltype(&BCryptDestroyHash)>(GetProcAddress(bcrypt, "BCryptDestroyHash"));
        close_algorithm = reinterpret_cast<decltype(&BCryptCloseAlgorithmProvider)>(GetProcAddress(bcrypt, "BCryptCloseAlgorithmProvider"));
        if (!open || !property || !create_hash || !hash_data || !finish_hash || !destroy_hash || !close_algorithm) return;
        if (open(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) { algorithm = nullptr; return; }
        ULONG written = 0;
        if (property(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &written, 0) < 0 || !object_bytes)
        { close_algorithm(algorithm, 0); algorithm = nullptr; }
    }

    ~Writer() { if (algorithm) close_algorithm(algorithm, 0); if (bcrypt) FreeLibrary(bcrypt); }

    void Write(const std::filesystem::path &directory, Batch batch)
    {
        try
        {
            std::filesystem::create_directories(directory / L"Shaders");
            Json shader_records = Json::array();
            uint64_t hash_failures = 0, file_failures = 0;
            for (const auto &blob : batch.blobs)
            {
                auto saved = saved_blobs.find(blob->id);
                if (saved == saved_blobs.end())
                {
                    const std::string hash = Digest(blob->bytes.data(), blob->bytes.size());
                    if (hash.empty()) { ++hash_failures; continue; }
                    uint32_t magic = 0;
                    if (blob->bytes.size() >= sizeof(magic)) std::memcpy(&magic, blob->bytes.data(), sizeof(magic));
                    const bool spirv = magic == 0x07230203;
                    const std::string filename = hash + (spirv ? ".spv" : ".bin");
                    const auto path = directory / L"Shaders" / filename;
                    auto temporary = path; temporary += L".tmp";
                    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                    output.write(reinterpret_cast<const char *>(blob->bytes.data()), blob->bytes.size());
                    output.close();
                    if (!output || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { ++file_failures; continue; }
                    saved = saved_blobs.emplace(blob->id, Json{{"id", blob->id}, {"sha256", hash}, {"bytes", blob->bytes.size()}, {"encoding", spirv ? "SPIR-V" : "unclassified"}, {"file", "Shaders/" + filename}}).first;
                }
                shader_records.push_back(saved->second);
            }
            for (auto &pipeline : batch.report["pipelines"])
            {
                Json manifest = {{"shaders", pipeline["shaders"]}, {"declared_target_format_ids", pipeline["declared_target_format_ids"]}, {"declared_depth_format_id", pipeline["declared_depth_format_id"]}};
                bool complete = pipeline["capture_complete"].get<bool>();
                for (auto &shader : manifest["shaders"])
                {
                    auto blob = saved_blobs.find(shader["blob_id"].get<uint64_t>());
                    shader.erase("blob_id");
                    shader["sha256"] = blob == saved_blobs.end() ? Json(nullptr) : blob->second["sha256"];
                    if (blob == saved_blobs.end()) complete = false;
                }
                const std::string encoded = manifest.dump();
                const std::string hash = Digest(encoded.data(), encoded.size());
                pipeline["shader_manifest_sha256"] = hash.empty() ? Json(nullptr) : Json(hash);
                pipeline["shader_manifest_complete"] = complete && !hash.empty();
            }
            batch.report["shaders"] = std::move(shader_records);
            batch.report["writer"] = {{"hash_algorithm", "SHA256"}, {"hash_failures_this_snapshot", hash_failures}, {"file_failures_this_snapshot", file_failures}};
            const auto path = directory / L"status.json";
            auto temporary = path; temporary += L".tmp";
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << batch.report.dump();
            output.close();
            if (output) MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        catch (...) {}
    }
};
}
