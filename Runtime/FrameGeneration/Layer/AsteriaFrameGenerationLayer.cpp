#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../Port/include/AsteriaVulkanShim.h"
#include "AsteriaMappedBufferApi.h"
#include "MappedBufferTracker.h"
#include "DescriptorEvidenceTracker.h"
#include "AsteriaViewportEvidenceApi.h"
#include "FrameGenerationRuntime.h"
#include "GraphicsPipelineDiagnosticShape.h"
#include "PipelineDiagnosticCache.h"
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <json.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
static constexpr const char *LayerName = "VK_LAYER_Asteria_FrameGeneration";

struct InstanceData
{
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
    PFN_GetPhysicalDeviceProcAddr next_physical_gipa = nullptr;
    uint32_t api_version = VK_API_VERSION_1_0;
};

struct DeviceData
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::shared_ptr<InstanceData> instance;
    PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
};

static std::mutex StateMutex;
static std::mutex FileMutex;
static std::unordered_map<void *, std::shared_ptr<InstanceData>> Instances;
static std::unordered_map<uint64_t, std::shared_ptr<InstanceData>> PhysicalDevices;
static std::unordered_map<void *, std::shared_ptr<DeviceData>> Devices;
static std::unordered_map<uint64_t, std::shared_ptr<DeviceData>> Queues;
static std::unordered_map<uint64_t, std::pair<VkDevice, VkCommandPool>> CommandBuffers;
static asteria_mapped_buffer::Tracker MappedBuffers;
static asteria_descriptor_evidence::Tracker DescriptorEvidence;
static asteria_fg_runtime::FgCameraState FgCamera{};
static std::unordered_map<uint64_t, asteria_fg_runtime::FgDeviceQueues> FgDeviceQueuesMap;
static std::unordered_map<uint64_t, std::unique_ptr<asteria_fg_runtime::FgSwapchainState>> FgSwapchains;
static std::unordered_map<uint64_t, uint64_t> FgSwapchainAliases;
struct FgSwapchainRuntimeOperations
{
    decltype(&asteria_fg_runtime::IsFrameGenerationRequestedByEnv) requested = &asteria_fg_runtime::IsFrameGenerationRequestedByEnv;
    decltype(&asteria_fg_runtime::InitializeSwapchainResources) initialize = &asteria_fg_runtime::InitializeSwapchainResources;
    decltype(&asteria_fg_runtime::DestroySwapchainResources) destroy = &asteria_fg_runtime::DestroySwapchainResources;
    decltype(&asteria_fg_runtime::WaitForSwapchainIdle) wait = &asteria_fg_runtime::WaitForSwapchainIdle;
};

static FgSwapchainRuntimeOperations FgSwapchainRuntime;

#include "FrameInputExports.h"

static bool CanConfigureFrameGeneration()
{
    return asteria_fg_runtime::IsFrameGenerationRequestedByEnv() &&
           bool(asteria_fg_runtime::CurrentFrameInputProvider());
}

static bool IsFrameGenerationDeviceRequested()
{
    return asteria_fg_runtime::IsFrameGenerationRequestedByEnv();
}
static bool InstanceDispatchCreated = false;

static Clock::time_point LastFlush{};
static Json State = {{"protocol", 1}, {"mode", "layer-probe"}, {"layer", LayerName},
                     {"passthrough", true}, {"frame_generation", false}, {"freeaim_plugin_pinned", false},
                     {"calls", Json::object()}, {"instances", Json::object()},
                     {"devices", Json::object()}, {"queues", Json::object()}, {"swapchains", Json::object()}};
static asteria_pipeline_diagnostic::Cache PipelineDiagnosticCache;
static uint64_t PipelineDiagnosticCreationSerial=0;
static bool PipelineDiagnosticsEnabled()
{
    wchar_t enabled[8]{};
    return GetEnvironmentVariableW(L"ASTERIA_FG_DRAW_VIEWPORT_DIAGNOSTICS",enabled,8)==1&&enabled[0]==L'1';
}
static bool NativeProxyImagesEnabled()
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"ASTERIA_FG_NATIVE_PROXY_IMAGES", value, 8) == 1 && value[0] == L'1';
}
static bool ReturnNativeSwapchainHandle()
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"ASTERIA_FG_RETURN_NATIVE_SWAPCHAIN", value, 8) == 1 && value[0] == L'1';
}
static bool NativePassthroughEnabled()
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"ASTERIA_FG_NATIVE_PASSTHROUGH", value, 8) == 1 && value[0] == L'1';
}
static std::string PipelineDiagnosticKey(uint64_t device,uint64_t deviceGeneration,uint64_t pipeline,uint64_t pipelineGeneration)
{
    return std::to_string(device)+":"+std::to_string(deviceGeneration)+":"+std::to_string(pipeline)+":"+std::to_string(pipelineGeneration);
}
static void SelectPipelineDiagnosticClosure(uint64_t device,uint64_t deviceGeneration,uint64_t pipeline,uint64_t generation,uint32_t depth,std::vector<std::string>& visited)
{
    if(!pipeline||!generation||depth>4||visited.size()>=32)return;
    const auto key=PipelineDiagnosticKey(device,deviceGeneration,pipeline,generation);
    if(std::find(visited.begin(),visited.end(),key)!=visited.end())return;visited.push_back(key);
    if(!PipelineDiagnosticCache.Select(key))return;
    const auto* source=PipelineDiagnosticCache.Peek(key);if(!source)return;
    const Json dependencies=source->value("dependencies_at_tracker_commit",Json::array());
    for(const auto& dependency:dependencies)
        if(dependency.value("observed",false))SelectPipelineDiagnosticClosure(device,deviceGeneration,dependency.value("pipeline",uint64_t(0)),
            dependency.value("pipeline_generation",uint64_t(0)),depth+1,visited);
}
static void SelectPipelineDiagnostics(uint64_t device,const AsteriaDrawViewportDiagnosticV1& value)
{
    if(!PipelineDiagnosticsEnabled())return;
    const std::lock_guard<std::mutex> lock(StateMutex);const uint64_t revision=PipelineDiagnosticCache.SelectionRevision();
    std::vector<std::string> visited;visited.reserve(16);
    SelectPipelineDiagnosticClosure(device,value.device_generation,value.pipeline,value.pipeline_generation,0,visited);
    for(const auto* poison:{&value.first_poison,&value.last_poison})
        if(poison->event)SelectPipelineDiagnosticClosure(device,value.device_generation,poison->pipeline,poison->pipeline_generation,0,visited);
    auto& state=State["graphics_pipeline_diagnostics"];
    if(!state.is_object()||PipelineDiagnosticCache.SelectionRevision()!=revision)state=PipelineDiagnosticCache.Export();
    else
    {
        auto stats=PipelineDiagnosticCache.Stats();
        for(auto it=stats.begin();it!=stats.end();++it)state[it.key()]=it.value();
    }
}

template<typename T> static uint64_t Id(T handle)
{
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(handle);
    else return static_cast<uint64_t>(handle);
}

template<typename T> static void *Key(T handle)
{
    return handle ? *reinterpret_cast<void **>(handle) : nullptr;
}

static std::string Name(uint64_t id) { return std::to_string(id); }

static void Count(const char *name)
{
    const uint64_t count = State["calls"].value(name, uint64_t(0));
    State["calls"][name] = count + 1;
}

static void TracePostSwapchainCall(const char *name, Json record)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    Count(name);
    auto &trace = State["post_swapchain_trace"];
    if (!trace.is_array()) trace = Json::array();
    if (trace.size() >= 512) trace.erase(trace.begin());
    record["call"] = name;
    trace.push_back(std::move(record));
}

static constexpr uint32_t NativeSubmitRecordCapacity = 256;
static constexpr uint32_t NativeSubmitElementCapacity = 32;
static thread_local uint32_t NativeSubmitDepth = 0;
static uint64_t NativeSubmitSerial = 0;

static uint64_t NextNativeSubmitSerial()
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    return ++NativeSubmitSerial;
}

static void AppendNativeSubmitRecord(Json record, uint64_t attemptSerial, uint64_t returnSerial, uint32_t depth, int32_t result)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    auto &observer = State["native_submit_observer"];
    if (!observer.is_object()) observer = { {"scope", "raw_native_submit_observer_only"}, {"capacity", NativeSubmitRecordCapacity}, {"records", Json::array()}, {"evictions", uint64_t(0)}, {"idle_records", Json::array()} };
    record["attempt_serial"] = attemptSerial;
    record["return_serial"] = returnSerial;
    record["call_depth"] = depth;
    record["native_result"] = result;
    auto &records = observer["records"];
    if (records.size() >= NativeSubmitRecordCapacity)
    {
        records.erase(records.begin());
        observer["evictions"] = observer.value("evictions", uint64_t(0)) + 1;
    }
    records.push_back(std::move(record));
    observer["latest_attempt_serial"] = attemptSerial;
    observer["latest_return_serial"] = returnSerial;
}

static void AppendNativeIdleRecord(const char *kind, uint64_t object, uint64_t attemptSerial, uint64_t returnSerial, int32_t result)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    auto &observer = State["native_submit_observer"];
    if (!observer.is_object()) observer = { {"scope", "raw_native_submit_observer_only"}, {"capacity", NativeSubmitRecordCapacity}, {"records", Json::array()}, {"evictions", uint64_t(0)}, {"idle_records", Json::array()} };
    auto &records = observer["idle_records"];
    if (records.size() >= NativeSubmitRecordCapacity) records.erase(records.begin());
    records.push_back({{"kind", kind}, {"object", object}, {"attempt_serial", attemptSerial}, {"return_serial", returnSerial}, {"native_result", result}});
    observer["latest_attempt_serial"] = attemptSerial;
    observer["latest_return_serial"] = returnSerial;
}

static Json NativeSubmitInfo(uint32_t submitCount, const VkSubmitInfo *submits)
{
    Json record = {{"kind", "vkQueueSubmit"}, {"submit_count", submitCount}, {"input_valid", submitCount == 0 || submits != nullptr}, {"truncated", submitCount > NativeSubmitElementCapacity}, {"submits", Json::array()}};
    if (!submits) return record;
    const uint32_t submitLimit = (std::min)(submitCount, NativeSubmitElementCapacity);
    for (uint32_t index = 0; index < submitLimit; ++index)
    {
        const auto &submit = submits[index];
        Json value = {{"command_buffer_count", submit.commandBufferCount}, {"wait_semaphore_count", submit.waitSemaphoreCount},
                      {"signal_semaphore_count", submit.signalSemaphoreCount}, {"command_buffers", Json::array()},
                      {"wait_semaphores", Json::array()}, {"signal_semaphores", Json::array()}};
        if (submit.commandBufferCount > NativeSubmitElementCapacity || submit.waitSemaphoreCount > NativeSubmitElementCapacity || submit.signalSemaphoreCount > NativeSubmitElementCapacity)
            record["truncated"] = true;
        for (uint32_t item = 0; item < (std::min)(submit.commandBufferCount, NativeSubmitElementCapacity); ++item)
            if (submit.pCommandBuffers) value["command_buffers"].push_back(Id(submit.pCommandBuffers[item]));
        for (uint32_t item = 0; item < (std::min)(submit.waitSemaphoreCount, NativeSubmitElementCapacity); ++item)
            if (submit.pWaitSemaphores) value["wait_semaphores"].push_back(Id(submit.pWaitSemaphores[item]));
        for (uint32_t item = 0; item < (std::min)(submit.signalSemaphoreCount, NativeSubmitElementCapacity); ++item)
            if (submit.pSignalSemaphores) value["signal_semaphores"].push_back(Id(submit.pSignalSemaphores[item]));
        record["submits"].push_back(std::move(value));
    }
    return record;
}

static Json NativeSubmitInfo2(uint32_t submitCount, const VkSubmitInfo2 *submits, const char *kind)
{
    Json record = {{"kind", kind}, {"submit_count", submitCount}, {"input_valid", submitCount == 0 || submits != nullptr}, {"truncated", submitCount > NativeSubmitElementCapacity}, {"submits", Json::array()}};
    if (!submits) return record;
    const uint32_t submitLimit = (std::min)(submitCount, NativeSubmitElementCapacity);
    for (uint32_t index = 0; index < submitLimit; ++index)
    {
        const auto &submit = submits[index];
        Json value = {{"command_buffer_count", submit.commandBufferInfoCount}, {"wait_semaphore_count", submit.waitSemaphoreInfoCount},
                      {"signal_semaphore_count", submit.signalSemaphoreInfoCount}, {"command_buffers", Json::array()},
                      {"wait_semaphores", Json::array()}, {"signal_semaphores", Json::array()}};
        if (submit.commandBufferInfoCount > NativeSubmitElementCapacity || submit.waitSemaphoreInfoCount > NativeSubmitElementCapacity || submit.signalSemaphoreInfoCount > NativeSubmitElementCapacity)
            record["truncated"] = true;
        for (uint32_t item = 0; item < (std::min)(submit.commandBufferInfoCount, NativeSubmitElementCapacity); ++item)
            if (submit.pCommandBufferInfos) value["command_buffers"].push_back(Id(submit.pCommandBufferInfos[item].commandBuffer));
        for (uint32_t item = 0; item < (std::min)(submit.waitSemaphoreInfoCount, NativeSubmitElementCapacity); ++item)
            if (submit.pWaitSemaphoreInfos) value["wait_semaphores"].push_back(Id(submit.pWaitSemaphoreInfos[item].semaphore));
        for (uint32_t item = 0; item < (std::min)(submit.signalSemaphoreInfoCount, NativeSubmitElementCapacity); ++item)
            if (submit.pSignalSemaphoreInfos) value["signal_semaphores"].push_back(Id(submit.pSignalSemaphoreInfos[item].semaphore));
        record["submits"].push_back(std::move(value));
    }
    return record;
}

static void UpdateFrameGenerationMode()
{
    const bool adapter = !FgSwapchains.empty();
    const bool configured = std::any_of(FgSwapchains.begin(), FgSwapchains.end(), [](const auto &entry) {
        return entry.second && bool(std::atomic_load_explicit(&entry.second->currentInputs, std::memory_order_acquire));
    });
    State["swapchain_adapter_active"] = adapter;
    State["passthrough"] = !configured;
    State["frame_generation"] = configured;
    State["mode"] = configured ? "frame-generation-configured" : adapter ? "frame-generation-adapter" : "layer-probe";
}

static std::filesystem::path OutputPath()
{
    wchar_t value[32768]{};
    DWORD length = GetEnvironmentVariableW(L"ASTERIA_FG_PROBE_OUTPUT", value, 32768);
    if (length && length < 32768) return std::filesystem::path(value);
    length = GetEnvironmentVariableW(L"LOCALAPPDATA", value, 32768);
    std::filesystem::path root;
    if (length && length < 32768) root = value;
    else
    {
        length = GetTempPathW(32768, value);
        if (length && length < 32768) root = value;
        else root = L".";
    }
    return root / L"Danyria" / L"AsteriaRuntime" / L"FrameGenerationProbe"
        / (L"asteria_fg_layer_probe_" + std::to_wstring(GetCurrentProcessId()) + L".json");
}

static void Flush(bool force = false)
{
    const std::lock_guard<std::mutex> file_lock(FileMutex);
    Json snapshot;
    {
        const std::lock_guard<std::mutex> state_lock(StateMutex);
        const auto now = Clock::now();
        if (!force && LastFlush.time_since_epoch().count() && now - LastFlush < std::chrono::milliseconds(250)) return;
        LastFlush = now;
        State["pid"] = GetCurrentProcessId();
        State["heartbeat"] = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        snapshot = State;
    }
    try
    {
        const auto output = OutputPath();
        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
        const std::filesystem::path temporary = output.wstring() + L".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << snapshot.dump(2);
        file.close();
        if (!MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING))
            DeleteFileW(temporary.c_str());
    }
    catch (...)
    {
        const auto output = OutputPath();
        const std::filesystem::path temporary = output.wstring() + L".tmp";
        DeleteFileW(temporary.c_str());
    }
}

static std::shared_ptr<InstanceData> Instance(VkInstance handle)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    const auto found = Instances.find(Key(handle));
    return found == Instances.end() ? nullptr : found->second;
}

static std::shared_ptr<InstanceData> Parent(VkPhysicalDevice handle)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    const auto physical = PhysicalDevices.find(Id(handle));
    if (physical != PhysicalDevices.end()) return physical->second;
    const auto instance = Instances.find(Key(handle));
    return instance == Instances.end() ? nullptr : instance->second;
}

static std::shared_ptr<DeviceData> Device(VkDevice handle)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    const auto found = Devices.find(Key(handle));
    return found == Devices.end() ? nullptr : found->second;
}

static std::shared_ptr<DeviceData> Queue(VkQueue handle)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    const auto found = Queues.find(Id(handle));
    if (found != Queues.end()) return found->second;
    const auto device = Devices.find(Key(handle));
    return device == Devices.end() ? nullptr : device->second;
}

template<typename T> static T Next(const std::shared_ptr<DeviceData> &data, const char *name)
{
    return data ? reinterpret_cast<T>(data->next_gdpa(data->device, name)) : nullptr;
}

template<typename T> static T Next(const std::shared_ptr<InstanceData> &data, const char *name)
{
    return data ? reinterpret_cast<T>(data->next_gipa(data->instance, name)) : nullptr;
}

static Json ChainTypes(const void *next)
{
    Json result = Json::array();
    for (auto entry = static_cast<const VkBaseInStructure *>(next); entry; entry = entry->pNext)
        result.push_back(static_cast<int>(entry->sType));
    return result;
}

static Json Extensions(uint32_t count, const char *const *names)
{
    Json result = Json::array();
    for (uint32_t i = 0; names && i < count; ++i) result.push_back(names[i] ? names[i] : "");
    return result;
}

static Json DeviceInfo(const VkDeviceCreateInfo *info)
{
    const auto core_features = [](const VkPhysicalDeviceFeatures &features) -> Json {
        return {{"shaderFloat64", bool(features.shaderFloat64)}, {"shaderInt64", bool(features.shaderInt64)},
                {"shaderInt16", bool(features.shaderInt16)},
                {"shaderStorageImageExtendedFormats", bool(features.shaderStorageImageExtendedFormats)},
                {"shaderStorageImageReadWithoutFormat", bool(features.shaderStorageImageReadWithoutFormat)},
                {"shaderStorageImageWriteWithoutFormat", bool(features.shaderStorageImageWriteWithoutFormat)}};
    };
    Json result = {{"extensions", Extensions(info->enabledExtensionCount, info->ppEnabledExtensionNames)},
                   {"pnext_types", ChainTypes(info->pNext)}, {"queue_requests", Json::array()}};
    for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
    {
        const auto &queue = info->pQueueCreateInfos[i];
        Json priorities = Json::array();
        for (uint32_t j = 0; queue.pQueuePriorities && j < queue.queueCount; ++j) priorities.push_back(queue.pQueuePriorities[j]);
        result["queue_requests"].push_back({{"family", queue.queueFamilyIndex}, {"count", queue.queueCount},
                                           {"flags", queue.flags}, {"priorities", priorities}});
    }
    if (info->pEnabledFeatures)
        result["core_features"] = core_features(*info->pEnabledFeatures);
    else result["core_features"] = nullptr;
    for (auto entry = static_cast<const VkBaseInStructure *>(info->pNext); entry; entry = entry->pNext)
    {
        if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            result["core_features"] = core_features(reinterpret_cast<const VkPhysicalDeviceFeatures2 *>(entry)->features);
        else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
        {
            const auto features = reinterpret_cast<const VkPhysicalDeviceVulkan11Features *>(entry);
            result["vulkan11_features"] = {{"storageBuffer16BitAccess", bool(features->storageBuffer16BitAccess)},
                {"uniformAndStorageBuffer16BitAccess", bool(features->uniformAndStorageBuffer16BitAccess)},
                {"storagePushConstant16", bool(features->storagePushConstant16)},
                {"storageInputOutput16", bool(features->storageInputOutput16)}};
        }
        else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        {
            const auto features = reinterpret_cast<const VkPhysicalDeviceVulkan12Features *>(entry);
            result["vulkan12_features"] = {{"shaderFloat16", bool(features->shaderFloat16)},
                                           {"shaderInt8", bool(features->shaderInt8)},
                                           {"shaderSubgroupExtendedTypes", bool(features->shaderSubgroupExtendedTypes)},
                                           {"timelineSemaphore", bool(features->timelineSemaphore)},
                                           {"bufferDeviceAddress", bool(features->bufferDeviceAddress)}};
        }
        else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
        {
            const auto features = reinterpret_cast<const VkPhysicalDeviceVulkan13Features *>(entry);
            result["vulkan13_features"] = {{"synchronization2", bool(features->synchronization2)},
                {"dynamicRendering", bool(features->dynamicRendering)}, {"maintenance4", bool(features->maintenance4)}};
        }
        else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
            result["timelineSemaphore"] = bool(reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures *>(entry)->timelineSemaphore);
        else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES)
            result["shaderFloat16"] = bool(reinterpret_cast<const VkPhysicalDeviceShaderFloat16Int8Features *>(entry)->shaderFloat16);
    }
    return result;
}

static Json SwapchainInfo(const VkSwapchainCreateInfoKHR *info)
{
    Json families = Json::array();
    if (info->imageSharingMode == VK_SHARING_MODE_CONCURRENT)
        for (uint32_t i = 0; info->pQueueFamilyIndices && i < info->queueFamilyIndexCount; ++i) families.push_back(info->pQueueFamilyIndices[i]);
    return {{"surface", Id(info->surface)}, {"old_swapchain", Id(info->oldSwapchain)},
            {"width", info->imageExtent.width}, {"height", info->imageExtent.height},
            {"image_format", int(info->imageFormat)}, {"color_space", int(info->imageColorSpace)},
            {"min_image_count", info->minImageCount}, {"image_usage", info->imageUsage},
            {"image_array_layers", info->imageArrayLayers}, {"sharing_mode", int(info->imageSharingMode)},
            {"queue_families", families}, {"present_mode", int(info->presentMode)},
            {"flags", info->flags}, {"pre_transform", info->preTransform},
            {"composite_alpha", info->compositeAlpha}, {"clipped", bool(info->clipped)},
            {"pnext_types", ChainTypes(info->pNext)}, {"live", true}, {"acquires", 0}, {"presents", 0}};
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetInstanceProcAddr(VkInstance instance, const char *name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetDeviceProcAddr(VkDevice device, const char *name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetPhysicalDeviceProcAddr(VkInstance instance, const char *name);

static VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *output)
{
    auto link = reinterpret_cast<VkLayerInstanceCreateInfo *>(const_cast<void *>(info->pNext));
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO))
        link = reinterpret_cast<VkLayerInstanceCreateInfo *>(const_cast<void *>(link->pNext));
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto next_physical = link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    const auto next = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const bool fgRequested = InstanceDispatchCreated && IsFrameGenerationDeviceRequested();
    VkInstanceCreateInfo modifiedInstanceInfo = *info;
    VkApplicationInfo modifiedAppInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if (fgRequested)
    {
        if (info->pApplicationInfo)
        {
            modifiedAppInfo = *info->pApplicationInfo;
            if (modifiedAppInfo.apiVersion < VK_API_VERSION_1_2)
                modifiedAppInfo.apiVersion = VK_API_VERSION_1_2;
        }
        else
        {
            modifiedAppInfo.pApplicationName = "Asteria FG Client";
            modifiedAppInfo.apiVersion = VK_API_VERSION_1_2;
        }
        modifiedInstanceInfo.pApplicationInfo = &modifiedAppInfo;
    }
    const VkResult result = next(fgRequested ? &modifiedInstanceInfo : info, allocator, output);
    if (result == VK_SUCCESS) InstanceDispatchCreated = true;
    const bool dispatchRegistered = result == VK_SUCCESS && AsteriaRegisterVulkanInstance(*output, next_gipa);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Count("create_instance");
        State["last_create_instance_result"] = int(result);
        if (result == VK_SUCCESS)
        {
            auto data = std::make_shared<InstanceData>();
            data->instance = *output;
            data->next_gipa = next_gipa;
            data->next_physical_gipa = next_physical;
            data->api_version = info->pApplicationInfo && info->pApplicationInfo->apiVersion ? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
            Instances[Key(*output)] = data;
            State["instances"][Name(Id(*output))] = {{"native_instance", Id(*output)}, {"api_version", data->api_version},
                {"extensions", Extensions(info->enabledExtensionCount, info->ppEnabledExtensionNames)},
                {"pnext_types", ChainTypes(info->pNext)}, {"live", true}, {"saved_next_dispatch_registered", dispatchRegistered}};
        }
    }
    Flush(true);
    return result;
}

static void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    const auto data = Instance(instance);
    const auto next = Next<PFN_vkDestroyInstance>(data, "vkDestroyInstance");
    if (!next) return;
    MappedBuffers.RemoveInstance(Id(instance));
    DescriptorEvidence.RemoveInstance(Id(instance));
    AsteriaUnregisterVulkanInstance(instance);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Count("destroy_instance");
        State["instances"][Name(Id(instance))]["live"] = false;
        for (auto item = PhysicalDevices.begin(); item != PhysicalDevices.end();)
            if (item->second == data) item = PhysicalDevices.erase(item); else ++item;
        for (auto item = Instances.begin(); item != Instances.end();)
            if (item->second == data) item = Instances.erase(item); else ++item;
    }
    next(instance, allocator);
    Flush(true);
}

static VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t *count, VkPhysicalDevice *physical)
{
    const auto data = Instance(instance);
    const auto next = Next<PFN_vkEnumeratePhysicalDevices>(data, "vkEnumeratePhysicalDevices");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = next(instance, count, physical);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && physical && count)
        for (uint32_t index = 0; index < *count; ++index) AsteriaRegisterVulkanPhysicalDevice(instance, physical[index]);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Count("enumerate_physical_devices");
        if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && physical && count)
        {
            Json ids = Json::array();
            for (uint32_t i = 0; i < *count; ++i) { PhysicalDevices[Id(physical[i])] = data; ids.push_back(Id(physical[i])); }
            State["instances"][Name(Id(instance))]["physical_devices"] = ids;
        }
    }
    Flush();
    return result;
}

static VkResult EnumerateGroups(VkInstance instance, uint32_t *count, VkPhysicalDeviceGroupProperties *groups, const char *method)
{
    const auto data = Instance(instance);
    const auto next = Next<PFN_vkEnumeratePhysicalDeviceGroups>(data, method);
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = next(instance, count, groups);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && groups && count)
        for (uint32_t index = 0; index < *count; ++index)
            for (uint32_t member = 0; member < groups[index].physicalDeviceCount; ++member)
                AsteriaRegisterVulkanPhysicalDevice(instance, groups[index].physicalDevices[member]);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Count("enumerate_physical_device_groups");
        if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && groups && count)
            for (uint32_t i = 0; i < *count; ++i)
                for (uint32_t j = 0; j < groups[i].physicalDeviceCount; ++j) PhysicalDevices[Id(groups[i].physicalDevices[j])] = data;
    }
    Flush();
    return result;
}

static VkResult VKAPI_CALL EnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t *count, VkPhysicalDeviceGroupProperties *groups)
{ return EnumerateGroups(instance, count, groups, "vkEnumeratePhysicalDeviceGroups"); }
static VkResult VKAPI_CALL EnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t *count, VkPhysicalDeviceGroupProperties *groups)
{ return EnumerateGroups(instance, count, groups, "vkEnumeratePhysicalDeviceGroupsKHR"); }

static bool IsFilteredFgDeviceExtension(const char *name)
{
    if (!name) return false;
    return std::strcmp(name, "VK_EXT_swapchain_maintenance1") == 0 ||
           std::strcmp(name, "VK_KHR_present_id") == 0 ||
           std::strcmp(name, "VK_KHR_present_wait") == 0 ||
           std::strcmp(name, "VK_EXT_hdr_metadata") == 0;
}

static VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical, const char *layerName, uint32_t *count, VkExtensionProperties *properties)
{
    const auto parent = Parent(physical);
    const auto next = Next<PFN_vkEnumerateDeviceExtensionProperties>(parent, "vkEnumerateDeviceExtensionProperties");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    if (!IsFrameGenerationDeviceRequested() || (layerName && layerName[0] != '\0'))
        return next(physical, layerName, count, properties);
    uint32_t fullCount = 0;
    VkResult res = next(physical, layerName, &fullCount, nullptr);
    if (res != VK_SUCCESS || !count) return res;
    std::vector<VkExtensionProperties> allProps(fullCount);
    res = next(physical, layerName, &fullCount, allProps.data());
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) return res;
    std::vector<VkExtensionProperties> filtered;
    filtered.reserve(fullCount);
    for (uint32_t i = 0; i < fullCount; ++i)
        if (!IsFilteredFgDeviceExtension(allProps[i].extensionName))
            filtered.push_back(allProps[i]);
    if (!properties)
    {
        *count = static_cast<uint32_t>(filtered.size());
        return VK_SUCCESS;
    }
    const uint32_t toCopy = std::min<uint32_t>(*count, static_cast<uint32_t>(filtered.size()));
    for (uint32_t i = 0; i < toCopy; ++i) properties[i] = filtered[i];
    *count = toCopy;
    return toCopy < filtered.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

#include "DeviceFeatureChain.h"

static VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *output)
{
    const auto parent = Parent(physical);
    auto link = reinterpret_cast<VkLayerDeviceCreateInfo *>(const_cast<void *>(info->pNext));
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO))
        link = reinterpret_cast<VkLayerDeviceCreateInfo *>(const_cast<void *>(link->pNext));
    if (!parent || !link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto next = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(parent->instance, "vkCreateDevice"));
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    Json metadata = DeviceInfo(info);
    const auto properties = Next<PFN_vkGetPhysicalDeviceProperties>(parent, "vkGetPhysicalDeviceProperties");
    if (properties)
    {
        VkPhysicalDeviceProperties values{};
        properties(physical, &values);
        metadata["physical_properties"] = {{"name", values.deviceName}, {"vendor_id", values.vendorID},
            {"device_id", values.deviceID}, {"api_version", values.apiVersion}, {"driver_version", values.driverVersion}};
    }
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    bool fgRequested = IsFrameGenerationDeviceRequested();
    asteria_device_features::OwnedChain featureChain;
    if (fgRequested)
    {
        try
        {
            if (!featureChain.Copy(info->pNext))
            {
                fgRequested = false;
                metadata["frame_generation_device_status"] = "unsupported_device_feature_chain";
            }
        }
        catch (const std::bad_alloc &)
        {
            fgRequested = false;
            metadata["frame_generation_device_status"] = "device_feature_chain_allocation_failed";
        }
    }
    metadata["frame_generation_requested"] = asteria_fg_runtime::IsFrameGenerationRequestedByEnv();
    metadata["frame_input_provider_available"] = bool(asteria_fg_runtime::CurrentFrameInputProvider());
    VkDeviceCreateInfo modifiedInfo = *info;
    if (fgRequested) modifiedInfo.pNext = featureChain.Head();
    std::vector<VkDeviceQueueCreateInfo> modifiedQueues;
    std::vector<float> fgPriorities(4, 1.0f);
    VkPhysicalDeviceFeatures enabledCoreFeatures{};
    VkPhysicalDevice16BitStorageFeatures added16Bit{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, nullptr, VK_TRUE, VK_TRUE, VK_FALSE, VK_FALSE};
    VkPhysicalDeviceShaderFloat16Int8Features addedFloat16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, nullptr, VK_TRUE, VK_TRUE};
    VkPhysicalDeviceTimelineSemaphoreFeatures addedTimeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, nullptr, VK_TRUE};
    VkPhysicalDeviceSubgroupSizeControlFeatures addedSubgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, nullptr, VK_TRUE, VK_TRUE};
    std::vector<const char *> modifiedExtensions;
    uint32_t fgFamily = UINT32_MAX;
    uint32_t fgQueueCount = 0;
    if (fgRequested && info->pQueueCreateInfos && info->queueCreateInfoCount > 0)
    {
        const auto getQueueFamilies = Next<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(parent, "vkGetPhysicalDeviceQueueFamilyProperties");
        std::vector<VkQueueFamilyProperties> familyProps;
        if (getQueueFamilies)
        {
            uint32_t familyCount = 0;
            getQueueFamilies(physical, &familyCount, nullptr);
            familyProps.resize(familyCount);
            if (familyCount > 0) getQueueFamilies(physical, &familyCount, familyProps.data());
        }
        modifiedQueues.assign(info->pQueueCreateInfos, info->pQueueCreateInfos + info->queueCreateInfoCount);
        for (auto &qInfo : modifiedQueues)
        {
            if (qInfo.queueFamilyIndex < familyProps.size())
            {
                const auto &fp = familyProps[qInfo.queueFamilyIndex];
                if ((fp.queueFlags & VK_QUEUE_GRAPHICS_BIT) && fp.queueCount >= 4)
                {
                    fgFamily = qInfo.queueFamilyIndex;
                    if (qInfo.queueCount < 4)
                    {
                        for (uint32_t j = 0; qInfo.pQueuePriorities && j < qInfo.queueCount && j < 4; ++j)
                            fgPriorities[j] = qInfo.pQueuePriorities[j];
                        qInfo.queueCount = 4;
                        qInfo.pQueuePriorities = fgPriorities.data();
                    }
                    fgQueueCount = qInfo.queueCount;
                    break;
                }
            }
        }
        modifiedInfo.pQueueCreateInfos = modifiedQueues.data();
        if (info->ppEnabledExtensionNames && info->enabledExtensionCount > 0)
        {
            for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
                if (!IsFilteredFgDeviceExtension(info->ppEnabledExtensionNames[i]))
                    modifiedExtensions.push_back(info->ppEnabledExtensionNames[i]);
        }
        const auto addExtIfMissing = [&](const char *extName) {
            for (const char *existing : modifiedExtensions)
                if (existing && std::strcmp(existing, extName) == 0) return;
            modifiedExtensions.push_back(extName);
        };
        addExtIfMissing(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        addExtIfMissing(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
        addExtIfMissing(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
        addExtIfMissing(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
        addExtIfMissing(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        addExtIfMissing(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);
        modifiedInfo.enabledExtensionCount = static_cast<uint32_t>(modifiedExtensions.size());
        modifiedInfo.ppEnabledExtensionNames = modifiedExtensions.data();

        bool hasFeatures2Struct = false;
        bool has16BitStruct = false;
        bool hasFloat16Struct = false;
        bool hasTimelineStruct = false;
        bool hasSubgroupStruct = false;
        for (auto entry = featureChain.Head(); entry; entry = entry->pNext)
        {
            if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            {
                auto *f2 = reinterpret_cast<VkPhysicalDeviceFeatures2 *>(entry);
                f2->features.shaderInt16 = VK_TRUE;
                f2->features.shaderStorageImageReadWithoutFormat = VK_TRUE;
                f2->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
                hasFeatures2Struct = true;
            }
            else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
            {
                auto *v11 = reinterpret_cast<VkPhysicalDeviceVulkan11Features *>(entry);
                v11->storageBuffer16BitAccess = VK_TRUE;
                v11->uniformAndStorageBuffer16BitAccess = VK_TRUE;
                has16BitStruct = true;
            }
            else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES)
            {
                auto *s16 = reinterpret_cast<VkPhysicalDevice16BitStorageFeatures *>(entry);
                s16->storageBuffer16BitAccess = VK_TRUE;
                s16->uniformAndStorageBuffer16BitAccess = VK_TRUE;
                has16BitStruct = true;
            }
            else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            {
                auto *v12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features *>(entry);
                v12->timelineSemaphore = VK_TRUE;
                v12->shaderFloat16 = VK_TRUE;
                v12->shaderInt8 = VK_TRUE;
                hasTimelineStruct = true;
                hasFloat16Struct = true;
            }
            else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES)
            {
                auto *f16 = reinterpret_cast<VkPhysicalDeviceShaderFloat16Int8Features *>(entry);
                f16->shaderFloat16 = VK_TRUE;
                f16->shaderInt8 = VK_TRUE;
                hasFloat16Struct = true;
            }
            else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
            {
                reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures *>(entry)->timelineSemaphore = VK_TRUE;
                hasTimelineStruct = true;
            }
            else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
            {
                auto *v13 = reinterpret_cast<VkPhysicalDeviceVulkan13Features *>(entry);
                v13->subgroupSizeControl = VK_TRUE;
                v13->computeFullSubgroups = VK_TRUE;
                hasSubgroupStruct = true;
            }
            else if (entry->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES)
            {
                auto *sg = reinterpret_cast<VkPhysicalDeviceSubgroupSizeControlFeatures *>(entry);
                sg->subgroupSizeControl = VK_TRUE;
                sg->computeFullSubgroups = VK_TRUE;
                hasSubgroupStruct = true;
            }
        }
        if (!hasFeatures2Struct)
        {
            if (info->pEnabledFeatures) enabledCoreFeatures = *info->pEnabledFeatures;
            enabledCoreFeatures.shaderInt16 = VK_TRUE;
            enabledCoreFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
            enabledCoreFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            modifiedInfo.pEnabledFeatures = &enabledCoreFeatures;
        }
        if (!has16BitStruct)
        {
            added16Bit.pNext = const_cast<void *>(modifiedInfo.pNext);
            modifiedInfo.pNext = &added16Bit;
        }
        if (!hasFloat16Struct)
        {
            addedFloat16.pNext = const_cast<void *>(modifiedInfo.pNext);
            modifiedInfo.pNext = &addedFloat16;
        }
        if (!hasTimelineStruct)
        {
            addedTimeline.pNext = const_cast<void *>(modifiedInfo.pNext);
            modifiedInfo.pNext = &addedTimeline;
        }
        if (!hasSubgroupStruct)
        {
            addedSubgroup.pNext = const_cast<void *>(modifiedInfo.pNext);
            modifiedInfo.pNext = &addedSubgroup;
        }
    }
    const VkResult result = next(physical, fgRequested ? &modifiedInfo : info, allocator, output);
    bool dispatchRegistered = false;
    asteria_fg_runtime::FgDeviceQueues resolvedFgQueues{};
    if (result == VK_SUCCESS)
    {
        AsteriaRegisterVulkanPhysicalDevice(parent->instance, physical);
        dispatchRegistered = AsteriaRegisterVulkanDevice(parent->instance, physical, *output, next_gdpa);
        AsteriaRegisterSwapchainImagesReplacement(&vkGetSwapchainImagesFFX, &asteria_vk::IsProxyToken);
        try { MappedBuffers.AddDevice(Id(*output), Id(parent->instance)); } catch (...) {}
        try { DescriptorEvidence.AddDevice(Id(*output), Id(parent->instance)); } catch (...) {}
        if (fgRequested && fgFamily != UINT32_MAX && fgQueueCount >= 4)
        {
            const auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(next_gdpa(*output, "vkGetDeviceQueue"));
            if (getQueue)
            {
                VkQueue q0 = VK_NULL_HANDLE, q1 = VK_NULL_HANDLE, q2 = VK_NULL_HANDLE, q3 = VK_NULL_HANDLE;
                getQueue(*output, fgFamily, 0, &q0);
                getQueue(*output, fgFamily, 1, &q1);
                getQueue(*output, fgFamily, 2, &q2);
                getQueue(*output, fgFamily, 3, &q3);
                if (q0 && q1 && q2 && q3)
                {
                    AsteriaRegisterVulkanQueue(*output, q0);
                    AsteriaRegisterVulkanQueue(*output, q1);
                    AsteriaRegisterVulkanQueue(*output, q2);
                    AsteriaRegisterVulkanQueue(*output, q3);
                    resolvedFgQueues.graphicsFamily = fgFamily;
                    resolvedFgQueues.gameQueue = q0;
                    resolvedFgQueues.asyncComputeQueue = q1;
                    resolvedFgQueues.presentQueue = q2;
                    resolvedFgQueues.imageAcquireQueue = q3;
                    resolvedFgQueues.available = true;
                }
            }
        }
    }
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Count("create_device");
        State["last_create_device_result"] = int(result);
        if (result == VK_SUCCESS)
        {
            auto data = std::make_shared<DeviceData>();
            data->device = *output;
            data->physical = physical;
            data->instance = parent;
            data->next_gdpa = next_gdpa;
            Devices[Key(*output)] = data;
            if (resolvedFgQueues.available) FgDeviceQueuesMap[Id(*output)] = resolvedFgQueues;
            metadata["native_device"] = Id(*output);
            metadata["native_physical_device"] = Id(physical);
            metadata["native_instance"] = Id(parent->instance);
            metadata["live"] = true;
            metadata["saved_next_dispatch_registered"] = dispatchRegistered;
            metadata["frame_generation_queues_available"] = resolvedFgQueues.available;
            metadata["native_frame_inputs_bound"] = false;
            State["devices"][Name(Id(*output))] = std::move(metadata);
        }
    }
    Flush(true);
    return result;
}

static void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
    const auto data = Device(device);
    const auto next = Next<PFN_vkDestroyDevice>(data, "vkDestroyDevice");
    if (!next) return;
    {
        std::vector<std::unique_ptr<asteria_fg_runtime::FgSwapchainState>> toDestroy;
        {
            const std::lock_guard<std::mutex> lock(StateMutex);
            for (auto item = FgSwapchains.begin(); item != FgSwapchains.end();)
            {
                if (item->second && item->second->device == device)
                {
                    toDestroy.push_back(std::move(item->second));
                    item = FgSwapchains.erase(item);
                }
                else ++item;
            }
            FgDeviceQueuesMap.erase(Id(device));
            UpdateFrameGenerationMode();
        }
        for (auto &fg : toDestroy)
            FgSwapchainRuntime.destroy(*fg);
        AsteriaReleaseUiCompositionResourcesVK(device);
    }
    MappedBuffers.RemoveDevice(Id(device));
    DescriptorEvidence.RemoveDevice(Id(device));
    asteria_live_scene::GlobalRuntime().OnDestroyDevice(device);
    AsteriaUnregisterVulkanDevice(device);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Count("destroy_device");
        State["devices"][Name(Id(device))]["live"] = false;
        for (auto item = Queues.begin(); item != Queues.end();)
            if (item->second == data) item = Queues.erase(item); else ++item;
        for (auto item = Devices.begin(); item != Devices.end();)
            if (item->second == data) item = Devices.erase(item); else ++item;
        for (auto item = CommandBuffers.begin(); item != CommandBuffers.end();)
            if (item->second.first == device) item = CommandBuffers.erase(item); else ++item;
    }
    next(device, allocator);
    Flush(true);
}

static void RecordQueue(const std::shared_ptr<DeviceData> &data, VkQueue queue, uint32_t family, uint32_t index, VkDeviceQueueCreateFlags flags)
{
    AsteriaRegisterVulkanQueue(data->device, queue);
    const std::lock_guard<std::mutex> lock(StateMutex);
    Queues[Id(queue)] = data;
    State["queues"][Name(Id(queue))] = {{"native_queue", Id(queue)}, {"device", Id(data->device)},
                                      {"family", family}, {"index", index}, {"flags", flags}};
}

static void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue)
{
    const auto data = Device(device);
    const auto next = Next<PFN_vkGetDeviceQueue>(data, "vkGetDeviceQueue");
    if (!next) return;
    next(device, family, index, queue);
    if (queue && *queue) RecordQueue(data, *queue, family, index, 0);
    { const std::lock_guard<std::mutex> lock(StateMutex); Count("get_device_queue"); }
    Flush();
}

static void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    const auto data = Device(device);
    const auto next = Next<PFN_vkGetDeviceQueue2>(data, "vkGetDeviceQueue2");
    if (!next) return;
    next(device, info, queue);
    if (queue && *queue) RecordQueue(data, *queue, info->queueFamilyIndex, info->queueIndex, info->flags);
    { const std::lock_guard<std::mutex> lock(StateMutex); Count("get_device_queue2"); }
    Flush();
}

static VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *buffers)
{
    const auto next = Next<PFN_vkAllocateCommandBuffers>(Device(device), "vkAllocateCommandBuffers");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = next(device, info, buffers);
    if (result == VK_SUCCESS)
    {
        try { DescriptorEvidence.AddCommands(Id(device), *info, buffers); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
        for (uint32_t index = 0; index < info->commandBufferCount; ++index)
            AsteriaRegisterVulkanCommandBuffer(device, buffers[index], info->commandPool);
        const std::lock_guard<std::mutex> lock(StateMutex);
        for (uint32_t index = 0; index < info->commandBufferCount; ++index)
            CommandBuffers[Id(buffers[index])] = {device, info->commandPool};
        Count("allocate_command_buffers");
    }
    return result;
}

static void VKAPI_CALL FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *buffers)
{
    const auto next = Next<PFN_vkFreeCommandBuffers>(Device(device), "vkFreeCommandBuffers");
    if (!next) return;
    try { DescriptorEvidence.RemoveCommands(Id(device), count, buffers); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
    asteria_live_scene::GlobalRuntime().OnFreeCommandBuffers(count, buffers);
    for (uint32_t index = 0; index < count; ++index) AsteriaUnregisterVulkanCommandBuffer(buffers[index]);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        for (uint32_t index = 0; index < count; ++index) CommandBuffers.erase(Id(buffers[index]));
        Count("free_command_buffers");
    }
    next(device, pool, count, buffers);
}

static void VKAPI_CALL DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *allocator)
{
    const auto next = Next<PFN_vkDestroyCommandPool>(Device(device), "vkDestroyCommandPool");
    if (!next) return;
    try { DescriptorEvidence.ResetPool(Id(device), Id(pool), true, true); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
    AsteriaUnregisterVulkanCommandPool(device, pool);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        for (auto item = CommandBuffers.begin(); item != CommandBuffers.end();)
        {
            if (item->second.first == device && item->second.second == pool)
            {
                item = CommandBuffers.erase(item);
            }
            else ++item;
        }
        Count("destroy_command_pool");
    }
    next(device, pool, allocator);
}

static VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
    const auto next = Next<PFN_vkAllocateMemory>(Device(device), "vkAllocateMemory");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = next(device, info, allocator, memory);
    if (result == VK_SUCCESS && info && memory)
        try { MappedBuffers.AddMemory(Id(device), Id(*memory), info->allocationSize); } catch (...) {}
    return result;
}

static void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator)
{
    const auto next = Next<PFN_vkFreeMemory>(Device(device), "vkFreeMemory");
    if (!next) return;
    MappedBuffers.RemoveMemory(Id(device), Id(memory));
    next(device, memory, allocator);
}

static VkResult VKAPI_CALL MapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void **output)
{
    const auto next = Next<PFN_vkMapMemory>(Device(device), "vkMapMemory");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = next(device, memory, offset, size, flags, output);
    if (result == VK_SUCCESS && output)
        try { MappedBuffers.Map(Id(device), Id(memory), offset, size, *output); } catch (...) {}
    return result;
}

static void VKAPI_CALL UnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    const auto next = Next<PFN_vkUnmapMemory>(Device(device), "vkUnmapMemory");
    if (!next) return;
    MappedBuffers.Unmap(Id(device), Id(memory));
    next(device, memory);
}

static VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo *info, const VkAllocationCallbacks *allocator, VkBuffer *buffer)
{
    const auto next = Next<PFN_vkCreateBuffer>(Device(device), "vkCreateBuffer");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = next(device, info, allocator, buffer);
    if (result == VK_SUCCESS && info && buffer)
    {
        try { MappedBuffers.AddBuffer(Id(device), Id(*buffer), info->size, info->usage); } catch (...) {}
        try { DescriptorEvidence.AddBuffer(Id(device), Id(*buffer), info->size); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
    }
    return result;
}

static VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *allocator, VkImageView *view)
{
    const auto next = Next<PFN_vkCreateImageView>(Device(device), "vkCreateImageView");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = next(device, info, allocator, view);
    if (result == VK_SUCCESS && view)
        asteria_live_scene::GlobalRuntime().OnCreateImageView(device, info, *view);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Count("create_image_view");
        Json record = {{"result", int(result)}, {"image", info ? Id(info->image) : uint64_t(0)},
                       {"view", view ? Id(*view) : uint64_t(0)},
                       {"format", info ? int(info->format) : 0},
                       {"type", info ? int(info->viewType) : 0},
                       {"usage_flags", info ? uint64_t(info->subresourceRange.aspectMask) : uint64_t(0)}};
        State["last_create_image_view"] = std::move(record);
    }
    Flush();
    return result;
}

static void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *allocator)
{
    const auto next = Next<PFN_vkDestroyImageView>(Device(device), "vkDestroyImageView");
    if (!next) return;
    asteria_live_scene::GlobalRuntime().OnDestroyImageView(device, view);
    next(device, view, allocator);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Count("destroy_image_view");
        State["last_destroy_image_view"] = Id(view);
    }
    Flush();
}

static VkResult VKAPI_CALL CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *info, const VkAllocationCallbacks *allocator, VkFramebuffer *framebuffer)
{
    const auto next = Next<PFN_vkCreateFramebuffer>(Device(device), "vkCreateFramebuffer");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = next(device, info, allocator, framebuffer);
    Json record = {{"result", int(result)}, {"render_pass", info ? Id(info->renderPass) : uint64_t(0)},
                   {"attachment_count", info ? info->attachmentCount : 0},
                   {"width", info ? info->width : 0}, {"height", info ? info->height : 0},
                   {"layers", info ? info->layers : 0}, {"framebuffer", framebuffer ? Id(*framebuffer) : uint64_t(0)}};
    TracePostSwapchainCall("create_framebuffer", std::move(record));
    Flush();
    return result;
}

static void VKAPI_CALL DestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *allocator)
{
    const auto next = Next<PFN_vkDestroyFramebuffer>(Device(device), "vkDestroyFramebuffer");
    if (!next) return;
    next(device, framebuffer, allocator);
    TracePostSwapchainCall("destroy_framebuffer", {{"framebuffer", Id(framebuffer)}});
    Flush();
}

static void VKAPI_CALL GetImageSubresourceLayout(VkDevice device, VkImage image, const VkImageSubresource *subresource, VkSubresourceLayout *layout)
{
    const auto next = Next<PFN_vkGetImageSubresourceLayout>(Device(device), "vkGetImageSubresourceLayout");
    if (!next) return;
    next(device, image, subresource, layout);
    TracePostSwapchainCall("get_image_subresource_layout", {{"image", Id(image)}, {"aspect_mask", subresource ? uint64_t(subresource->aspectMask) : uint64_t(0)}});
    Flush();
}

static void VKAPI_CALL GetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *requirements)
{
    const auto next = Next<PFN_vkGetImageMemoryRequirements>(Device(device), "vkGetImageMemoryRequirements");
    if (!next) return;
    next(device, image, requirements);
    TracePostSwapchainCall("get_image_memory_requirements", {{"image", Id(image)}, {"size", requirements ? uint64_t(requirements->size) : uint64_t(0)}, {"alignment", requirements ? uint64_t(requirements->alignment) : uint64_t(0)}});
    Flush();
}

static VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo *info, const VkAllocationCallbacks *allocator, VkImage *image)
{
    const auto next = Next<PFN_vkCreateImage>(Device(device), "vkCreateImage");
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = next(device, info, allocator, image);
    if (result == VK_SUCCESS && image)
        asteria_live_scene::GlobalRuntime().OnCreateImage(device, info, *image);
    TracePostSwapchainCall("create_image", {{"result", int(result)}, {"image", image ? Id(*image) : uint64_t(0)},
        {"format", info ? int(info->format) : 0}, {"width", info ? info->extent.width : 0}, {"height", info ? info->extent.height : 0},
        {"usage", info ? uint64_t(info->usage) : uint64_t(0)}, {"samples", info ? uint32_t(info->samples) : 0}});
    Flush();
    return result;
}

static void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator)
{
    const auto next = Next<PFN_vkDestroyImage>(Device(device), "vkDestroyImage");
    if (!next) return;
    asteria_live_scene::GlobalRuntime().OnDestroyImage(device, image);
    next(device, image, allocator);
    TracePostSwapchainCall("destroy_image", {{"image", Id(image)}});
    Flush();
}

static void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator)
{
    const auto next = Next<PFN_vkDestroyBuffer>(Device(device), "vkDestroyBuffer");
    if (!next) return;
    MappedBuffers.RemoveBuffer(Id(device), Id(buffer));
    DescriptorEvidence.RemoveBuffer(Id(device), Id(buffer));
    next(device, buffer, allocator);
}

static VkResult VKAPI_CALL BindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
    const auto next = Next<PFN_vkBindBufferMemory>(Device(device), "vkBindBufferMemory");
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = next(device, buffer, memory, offset);
    if (result == VK_SUCCESS)
        try { MappedBuffers.Bind(Id(device), Id(buffer), Id(memory), offset); } catch (...) {}
    return result;
}

static VkResult BindBufferMemory2Impl(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo *infos, const char *method)
{
    const auto next = Next<PFN_vkBindBufferMemory2>(Device(device), method);
    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = next(device, count, infos);
    if (result == VK_SUCCESS && infos)
        for (uint32_t index = 0; index < count; ++index)
            try { MappedBuffers.Bind(Id(device), Id(infos[index].buffer), Id(infos[index].memory), infos[index].memoryOffset); } catch (...) {}
    return result;
}

static VkResult VKAPI_CALL BindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo *infos)
{
    return BindBufferMemory2Impl(device, count, infos, "vkBindBufferMemory2");
}

static VkResult VKAPI_CALL BindBufferMemory2KHR(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo *infos)
{
    return BindBufferMemory2Impl(device, count, infos, "vkBindBufferMemory2KHR");
}

extern "C" BOOL __stdcall AsteriaCopyMappedBufferBytes(uint64_t device, uint64_t buffer, uint64_t offset, uint32_t size, void *destination)
{
    try { return MappedBuffers.Copy(device, buffer, offset, size, destination) ? TRUE : FALSE; }
    catch (...) { return FALSE; }
}

static bool DescriptorAbiInput(uint32_t size, const AsteriaBoundBufferDescriptorV1 *output) noexcept
{
    __try { return output && size == sizeof(*output) && output->struct_size == sizeof(*output) && output->api_version == 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool DescriptorAbiOutput(AsteriaBoundBufferDescriptorV1 *output, const AsteriaBoundBufferDescriptorV1 *value) noexcept
{
    __try { std::memcpy(output,value,sizeof(*value)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

extern "C" BOOL __stdcall AsteriaQueryBoundBufferDescriptor(uint64_t device, uint64_t command, uint32_t set, uint32_t binding, uint32_t element, uint32_t size, AsteriaBoundBufferDescriptorV1 *output)
{
    if (!DescriptorAbiInput(size,output)) return FALSE;
    AsteriaBoundBufferDescriptorV1 value{};
    bool success=false;
    try { success=DescriptorEvidence.Query(device,command,set,binding,element,value); }
    catch (...) { value.struct_size=sizeof(value); value.api_version=1; value.status=ASTERIA_DESCRIPTOR_CAP_EXCEEDED; }
    return DescriptorAbiOutput(output,&value) && success ? TRUE : FALSE;
}

static bool ViewportAbiInput(uint32_t size,const AsteriaNativeViewportEvidenceV1 *output) noexcept
{
    __try { return output && size==sizeof(*output) && output->struct_size==sizeof(*output) && output->api_version==1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ViewportAbiOutput(AsteriaNativeViewportEvidenceV1 *output,const AsteriaNativeViewportEvidenceV1 *value) noexcept
{
    __try { std::memcpy(output,value,sizeof(*value)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

extern "C" BOOL __stdcall AsteriaQueryNativeViewportV1(uint64_t device,uint64_t command,uint32_t size,AsteriaNativeViewportEvidenceV1 *output)
{
    if (!ViewportAbiInput(size,output)) return FALSE;
    AsteriaNativeViewportEvidenceV1 value{}; bool success=false;
    try { success=DescriptorEvidence.QueryViewport(device,command,value); }
    catch (...) { value.struct_size=sizeof(value); value.api_version=1; value.flags=ASTERIA_VIEWPORT_EVIDENCE_OBSERVED_ONLY; value.status=ASTERIA_VIEWPORT_EVIDENCE_UNSUPPORTED_COMMAND_STATE; }
    return ViewportAbiOutput(output,&value) && success ? TRUE : FALSE;
}

static bool DrawViewportAbiInput(uint32_t size,const AsteriaDrawViewportEvidenceV1 *output) noexcept
{
    __try { return output && size==sizeof(*output) && output->struct_size==sizeof(*output) && output->api_version==1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool DrawViewportAbiOutput(AsteriaDrawViewportEvidenceV1 *output,const AsteriaDrawViewportEvidenceV1 *value) noexcept
{
    __try { std::memcpy(output,value,sizeof(*value)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
extern "C" BOOL __stdcall AsteriaQueryDrawViewportV1(uint64_t device,uint64_t command,uint32_t size,AsteriaDrawViewportEvidenceV1 *output)
{
    if (!DrawViewportAbiInput(size,output)) return FALSE;
    AsteriaDrawViewportEvidenceV1 value{}; bool success=false;
    try { success=DescriptorEvidence.QueryDrawViewport(device,command,value); }
    catch (...) { value.struct_size=sizeof(value); value.api_version=1; value.flags=ASTERIA_DRAW_VIEWPORT_OBSERVER_ONLY; value.status=static_cast<uint32_t>(asteria::graphics_viewport::Status::UnsupportedCommand); }
    return DrawViewportAbiOutput(output,&value) && success ? TRUE : FALSE;
}
static bool DrawDiagnosticAbiInput(uint32_t size,const AsteriaDrawViewportDiagnosticV1* output) noexcept
{
    __try { return output&&size>=sizeof(*output)&&output->struct_size>=sizeof(*output)&&output->api_version==1; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool DrawDiagnosticAbiOutput(AsteriaDrawViewportDiagnosticV1* output,const AsteriaDrawViewportDiagnosticV1* value) noexcept
{
    __try { std::memcpy(output,value,sizeof(*value));return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
extern "C" BOOL __stdcall AsteriaQueryDrawViewportDiagnosticV1(uint64_t device,uint64_t command,uint32_t size,AsteriaDrawViewportDiagnosticV1* output)
{
    if(!DrawDiagnosticAbiInput(size,output))return FALSE;
    AsteriaDrawViewportDiagnosticV1 value{};bool observed=false;
    try
    {
        observed=DescriptorEvidence.QueryDrawViewportDiagnostic(device,command,value);
        if(PipelineDiagnosticsEnabled()){value.flags|=ASTERIA_DRAW_DIAGNOSTIC_CREATION_CACHE_ENABLED;SelectPipelineDiagnostics(device,value);}
    }
    catch(...){value={};value.struct_size=sizeof(value);value.api_version=1;value.status=ASTERIA_DRAW_VIEWPORT_UNSUPPORTED_STATE;value.flags=ASTERIA_DRAW_DIAGNOSTIC_OBSERVER_ONLY;observed=false;}
    return DrawDiagnosticAbiOutput(output,&value)&&observed?TRUE:FALSE;
}

static std::shared_ptr<DeviceData> CommandDevice(VkCommandBuffer command)
{
    VkDevice owner=VK_NULL_HANDLE;
    { const std::lock_guard<std::mutex> lock(StateMutex); const auto it=CommandBuffers.find(Id(command)); if (it!=CommandBuffers.end()) owner=it->second.first; }
    return Device(owner ? owner : reinterpret_cast<VkDevice>(command));
}

static VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice device,const VkDescriptorSetLayoutCreateInfo *info,const VkAllocationCallbacks *allocator,VkDescriptorSetLayout *output)
{
    const auto next=Next<PFN_vkCreateDescriptorSetLayout>(Device(device),"vkCreateDescriptorSetLayout"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next(device,info,allocator,output);
    if (result==VK_SUCCESS && info && output) try { DescriptorEvidence.AddLayout(Id(device),Id(*output),*info); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
    return result;
}
static void VKAPI_CALL DestroyDescriptorSetLayout(VkDevice device,VkDescriptorSetLayout layout,const VkAllocationCallbacks *allocator)
{
    const auto next=Next<PFN_vkDestroyDescriptorSetLayout>(Device(device),"vkDestroyDescriptorSetLayout"); if (!next) return;
    DescriptorEvidence.RemoveLayout(Id(device),Id(layout)); next(device,layout,allocator);
}
static VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device,const VkPipelineLayoutCreateInfo *info,const VkAllocationCallbacks *allocator,VkPipelineLayout *output)
{
    const auto next=Next<PFN_vkCreatePipelineLayout>(Device(device),"vkCreatePipelineLayout"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next(device,info,allocator,output);
    if (result==VK_SUCCESS && info && output) try { DescriptorEvidence.AddPipelineLayout(Id(device),Id(*output),*info); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
    return result;
}
static void VKAPI_CALL DestroyPipelineLayout(VkDevice device,VkPipelineLayout layout,const VkAllocationCallbacks *allocator)
{
    const auto next=Next<PFN_vkDestroyPipelineLayout>(Device(device),"vkDestroyPipelineLayout"); if (!next) return;
    DescriptorEvidence.RemovePipelineLayout(Id(device),Id(layout)); next(device,layout,allocator);
}
static VkResult VKAPI_CALL CreateDescriptorPool(VkDevice device,const VkDescriptorPoolCreateInfo *info,const VkAllocationCallbacks *allocator,VkDescriptorPool *output)
{
    const auto next=Next<PFN_vkCreateDescriptorPool>(Device(device),"vkCreateDescriptorPool"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next(device,info,allocator,output);
    if (result==VK_SUCCESS && info && output) try { DescriptorEvidence.AddPool(Id(device),Id(*output),false,!info->pNext && !(info->flags & ~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
    return result;
}
static void VKAPI_CALL DestroyDescriptorPool(VkDevice device,VkDescriptorPool pool,const VkAllocationCallbacks *allocator)
{
    const auto next=Next<PFN_vkDestroyDescriptorPool>(Device(device),"vkDestroyDescriptorPool"); if (!next) return;
    DescriptorEvidence.ResetPool(Id(device),Id(pool),false,true); next(device,pool,allocator);
}
static VkResult VKAPI_CALL ResetDescriptorPool(VkDevice device,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags)
{
    const auto next=Next<PFN_vkResetDescriptorPool>(Device(device),"vkResetDescriptorPool"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    DescriptorEvidence.ResetPool(Id(device),Id(pool),false,false); return next(device,pool,flags);
}
static VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice device,const VkDescriptorSetAllocateInfo *info,VkDescriptorSet *output)
{
    const auto next=Next<PFN_vkAllocateDescriptorSets>(Device(device),"vkAllocateDescriptorSets"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next(device,info,output);
    if (result==VK_SUCCESS && info && output) try { DescriptorEvidence.AddSets(Id(device),*info,output); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
    return result;
}
static VkResult VKAPI_CALL FreeDescriptorSets(VkDevice device,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet *sets)
{
    const auto next=Next<PFN_vkFreeDescriptorSets>(Device(device),"vkFreeDescriptorSets"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    DescriptorEvidence.RemoveSets(Id(device),count,sets); return next(device,pool,count,sets);
}
static void VKAPI_CALL UpdateDescriptorSets(VkDevice device,uint32_t write_count,const VkWriteDescriptorSet *writes,uint32_t copy_count,const VkCopyDescriptorSet *copies)
{
    const auto next=Next<PFN_vkUpdateDescriptorSets>(Device(device),"vkUpdateDescriptorSets"); if (!next) return;
    next(device,write_count,writes,copy_count,copies);
    try { DescriptorEvidence.Update(Id(device),write_count,writes,copy_count,copies); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
}
static VkResult CreateDescriptorUpdateTemplateImpl(VkDevice device,const VkDescriptorUpdateTemplateCreateInfo *info,const VkAllocationCallbacks *allocator,VkDescriptorUpdateTemplate *output,const char *method)
{
    const auto next=Next<PFN_vkCreateDescriptorUpdateTemplate>(Device(device),method); if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
    const auto result=next(device,info,allocator,output);
    if (result==VK_SUCCESS && info && output) try { DescriptorEvidence.AddTemplate(Id(device),Id(*output),*info); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
    return result;
}
static VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(VkDevice d,const VkDescriptorUpdateTemplateCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorUpdateTemplate *o) { return CreateDescriptorUpdateTemplateImpl(d,i,a,o,"vkCreateDescriptorUpdateTemplate"); }
static VkResult VKAPI_CALL CreateDescriptorUpdateTemplateKHR(VkDevice d,const VkDescriptorUpdateTemplateCreateInfo *i,const VkAllocationCallbacks *a,VkDescriptorUpdateTemplate *o) { return CreateDescriptorUpdateTemplateImpl(d,i,a,o,"vkCreateDescriptorUpdateTemplateKHR"); }
static void DestroyDescriptorUpdateTemplateImpl(VkDevice device,VkDescriptorUpdateTemplate handle,const VkAllocationCallbacks *allocator,const char *method)
{
    const auto next=Next<PFN_vkDestroyDescriptorUpdateTemplate>(Device(device),method); if (!next) return;
    DescriptorEvidence.RemoveTemplate(Id(device),Id(handle)); next(device,handle,allocator);
}
static void VKAPI_CALL DestroyDescriptorUpdateTemplate(VkDevice d,VkDescriptorUpdateTemplate t,const VkAllocationCallbacks *a) { DestroyDescriptorUpdateTemplateImpl(d,t,a,"vkDestroyDescriptorUpdateTemplate"); }
static void VKAPI_CALL DestroyDescriptorUpdateTemplateKHR(VkDevice d,VkDescriptorUpdateTemplate t,const VkAllocationCallbacks *a) { DestroyDescriptorUpdateTemplateImpl(d,t,a,"vkDestroyDescriptorUpdateTemplateKHR"); }
static void UpdateDescriptorSetWithTemplateImpl(VkDevice device,VkDescriptorSet set,VkDescriptorUpdateTemplate handle,const void *data,const char *method)
{
    const auto next=Next<PFN_vkUpdateDescriptorSetWithTemplate>(Device(device),method); if (!next) return;
    next(device,set,handle,data);
    try { DescriptorEvidence.UpdateTemplate(Id(device),Id(set),Id(handle),data); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
}
static void VKAPI_CALL UpdateDescriptorSetWithTemplate(VkDevice d,VkDescriptorSet s,VkDescriptorUpdateTemplate t,const void *p) { UpdateDescriptorSetWithTemplateImpl(d,s,t,p,"vkUpdateDescriptorSetWithTemplate"); }
static void VKAPI_CALL UpdateDescriptorSetWithTemplateKHR(VkDevice d,VkDescriptorSet s,VkDescriptorUpdateTemplate t,const void *p) { UpdateDescriptorSetWithTemplateImpl(d,s,t,p,"vkUpdateDescriptorSetWithTemplateKHR"); }
static void ObservePipelineCreationShape(uint32_t count,const VkGraphicsPipelineCreateInfo *infos,const VkPipeline *output)
{
    wchar_t enabled[8]{};
    if (GetEnvironmentVariableW(L"ASTERIA_FG_DRAW_VIEWPORT_DIAGNOSTICS",enabled,8)!=1 || enabled[0]!=L'1') return;
    for (uint32_t index=0; index<count; ++index)
    {
        if (!output[index]) continue;
        const auto &info=infos[index];
        Json shape={{"flags",info.flags},{"render_pass_nonnull",bool(info.renderPass)},{"stage_count",info.stageCount},
                    {"viewport_state_present",bool(info.pViewportState)},{"dynamic_states",Json::array()},{"pnext_types",Json::array()}};
        const auto *entry=static_cast<const VkBaseInStructure *>(info.pNext);
        uint32_t nodes=0;
        for (; entry && nodes<16; entry=entry->pNext,++nodes) shape["pnext_types"].push_back(uint32_t(entry->sType));
        shape["pnext_chain_truncated"]=entry!=nullptr;
        if (info.pViewportState)
        {
            shape["viewport_count"]=info.pViewportState->viewportCount; shape["scissor_count"]=info.pViewportState->scissorCount;
            shape["static_viewport_pointer_present"]=info.pViewportState->pViewports!=nullptr;
            shape["static_scissor_pointer_present"]=info.pViewportState->pScissors!=nullptr;
        }
        if (info.pDynamicState && info.pDynamicState->pDynamicStates)
            for (uint32_t state=0; state<std::min<uint32_t>(info.pDynamicState->dynamicStateCount,128); ++state)
                shape["dynamic_states"].push_back(uint32_t(info.pDynamicState->pDynamicStates[state]));
        const std::string key=shape.dump();
        const std::lock_guard<std::mutex> lock(StateMutex);
        auto &diagnostics=State["graphics_pipeline_creation_observations"];
        if (!diagnostics.is_object()) diagnostics={{"scope","native_create_info_shape_only"},{"shapes",Json::object()},{"shape_limit_hits",0}};
        auto &shapes=diagnostics["shapes"];
        if (!shapes.contains(key) && shapes.size()>=64) { diagnostics["shape_limit_hits"]=diagnostics["shape_limit_hits"].get<uint64_t>()+1; continue; }
        auto &record=shapes[key];
        const auto observed=record.is_object()?record.value("count",uint64_t(0)):0;
        record={{"shape",shape},{"count",observed+1},{"last_pipeline",Id(output[index])}};
    }
}
static void ObserveDetailedPipelineCreations(VkDevice device,uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkPipeline* output,VkResult result,uint64_t epoch,
    uint64_t deviceGeneration,const std::vector<asteria::graphics_viewport::PipelineObservation>& observations)
{
    if(!PipelineDiagnosticsEnabled()||observations.size()!=count||!deviceGeneration)return;
    for(uint32_t index=0;index<count;++index)
    {
        if(!output[index])continue;
        const auto& observed=observations[index];if(observed.pipeline!=Id(output[index])||!observed.generation)continue;
        Json record={{"scope","raw_native_create_and_tracker_observation_only"},{"native_device",Id(device)},{"device_generation",deviceGeneration},
            {"pipeline",Id(output[index])},{"pipeline_generation",observed.generation},{"native_result",int32_t(result)},{"batch_index",index},{"batch_count",count},
            {"epoch_before_native",epoch},{"epoch_at_commit",observed.epoch},{"epoch_stable",observed.epoch_stable},
            {"epoch_guard_required",observed.epoch_guard_required},
            {"creation_rejection",asteria_descriptor_evidence::DrawViewportAbi(observed.rejection)},{"creation_mode",asteria_descriptor_evidence::DrawViewportAbi(observed.mode)},
            {"creation_accepted",observed.valid},{"is_library",observed.is_library},{"library_subset_flags",observed.library_flags},
            {"payload_issue_hint",observed.detail},{"payload_hint_index",observed.detail_index},
            {"payload_hint_scope","post_validation_scan_not_exact_rejection_branch"},
            {"dependencies_at_tracker_commit",Json::array()},{"dependency_count",observed.dependency_count},{"dependencies_truncated",observed.dependencies_truncated},
            {"create_info",asteria_graphics_pipeline_diagnostic::DescribeGraphicsPipelineCreateInfo(infos[index])}};
        for(uint32_t n=0;n<(std::min)(observed.dependency_count,uint32_t(observed.dependencies.size()));++n)
        {
            const auto& dependency=observed.dependencies[n];
            record["dependencies_at_tracker_commit"].push_back({{"pipeline",dependency.pipeline},{"pipeline_generation",dependency.generation},
                {"observed",dependency.observed},{"valid",dependency.valid},{"is_library",dependency.is_library},{"subset_flags",dependency.library_flags},
                {"creation_rejection",asteria_descriptor_evidence::DrawViewportAbi(dependency.rejection)}});
        }
        const auto key=PipelineDiagnosticKey(Id(device),deviceGeneration,Id(output[index]),observed.generation);
        const std::lock_guard<std::mutex> lock(StateMutex);record["creation_record_serial"]=++PipelineDiagnosticCreationSerial;
        PipelineDiagnosticCache.Put(key,record);
    }
}
static VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *output)
{
    const auto next=Next<PFN_vkCreateGraphicsPipelines>(Device(device),"vkCreateGraphicsPipelines"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t epoch=0;
    uint64_t deviceGeneration=0;std::vector<asteria::graphics_viewport::PipelineObservation> observations;
    const bool diagnosticEnabled=PipelineDiagnosticsEnabled();
    try { epoch=DescriptorEvidence.GraphicsPipelineMutationEpoch(Id(device)); } catch (...) {}
    const auto result=next(device,cache,count,infos,allocator,output);
    if (count>32768) DescriptorEvidence.Poison(Id(device));
    else if (infos && output && (result==VK_SUCCESS || result==VK_PIPELINE_COMPILE_REQUIRED_EXT))
        try { if (!epoch) DescriptorEvidence.Poison(Id(device)); else DescriptorEvidence.AddGraphicsPipelinesAtEpoch(Id(device),count,infos,output,epoch,
            diagnosticEnabled?&observations:nullptr,diagnosticEnabled?&deviceGeneration:nullptr); }
        catch (...) { DescriptorEvidence.Poison(Id(device)); }
    if (count<=32768 && infos && output && result==VK_SUCCESS) try { ObservePipelineCreationShape(count,infos,output); } catch (...) {}
    if (count<=32768 && infos && output && (result==VK_SUCCESS||result==VK_PIPELINE_COMPILE_REQUIRED_EXT))
        try { ObserveDetailedPipelineCreations(device,count,infos,output,result,epoch,deviceGeneration,observations); } catch (...) {}
    return result;
}
static void VKAPI_CALL DestroyPipeline(VkDevice device,VkPipeline pipeline,const VkAllocationCallbacks *allocator)
{
    const auto next=Next<PFN_vkDestroyPipeline>(Device(device),"vkDestroyPipeline"); if (!next) return;
    DescriptorEvidence.RemovePipeline(Id(device),Id(pipeline)); next(device,pipeline,allocator);
}
static VkResult VKAPI_CALL CreateCommandPool(VkDevice device,const VkCommandPoolCreateInfo *info,const VkAllocationCallbacks *allocator,VkCommandPool *output)
{
    const auto next=Next<PFN_vkCreateCommandPool>(Device(device),"vkCreateCommandPool"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=next(device,info,allocator,output);
    if (result==VK_SUCCESS && info && output) try { DescriptorEvidence.AddPool(Id(device),Id(*output),true,!info->pNext); } catch (...) { DescriptorEvidence.Poison(Id(device)); }
    return result;
}
static VkResult VKAPI_CALL ResetCommandPool(VkDevice device,VkCommandPool pool,VkCommandPoolResetFlags flags)
{
    const auto next=Next<PFN_vkResetCommandPool>(Device(device),"vkResetCommandPool"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    DescriptorEvidence.ResetPool(Id(device),Id(pool),true,false); return next(device,pool,flags);
}
static VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer command,const VkCommandBufferBeginInfo *info)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkBeginCommandBuffer>(data,"vkBeginCommandBuffer"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    asteria_live_scene::GlobalRuntime().OnResetCommandBuffer(command);
    DescriptorEvidence.Begin(Id(data->device),Id(command),false); const auto result=next(command,info);
    if (result==VK_SUCCESS) DescriptorEvidence.Begin(Id(data->device),Id(command),true);
    if (info && (info->pNext || info->pInheritanceInfo)) DescriptorEvidence.Unsupported(Id(data->device),Id(command));
    return result;
}
static VkResult VKAPI_CALL EndCommandBuffer(VkCommandBuffer command)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkEndCommandBuffer>(data,"vkEndCommandBuffer"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    DescriptorEvidence.End(Id(data->device),Id(command)); return next(command);
}
static VkResult VKAPI_CALL QueueSubmit(VkQueue queue,uint32_t submitCount,const VkSubmitInfo *submits,VkFence fence)
{
    const auto data=Queue(queue); const auto next=Next<PFN_vkQueueSubmit>(data,"vkQueueSubmit");
    const uint64_t attempt=NextNativeSubmitSerial(); const uint32_t depth=++NativeSubmitDepth;
    Json record=NativeSubmitInfo(submitCount,submits); record["queue"]=Id(queue); record["device"]=data?Id(data->device):0; record["fence"]=Id(fence);
    VkResult result=VK_ERROR_EXTENSION_NOT_PRESENT;
    if(next) result=next(queue,submitCount,submits,fence);
    const uint64_t returned=NextNativeSubmitSerial(); --NativeSubmitDepth;
    AppendNativeSubmitRecord(std::move(record),attempt,returned,depth,int32_t(result)); Flush(); return result;
}
static VkResult VKAPI_CALL QueueSubmit2(VkQueue queue,uint32_t submitCount,const VkSubmitInfo2 *submits,VkFence fence)
{
    const auto data=Queue(queue); const auto next=Next<PFN_vkQueueSubmit2>(data,"vkQueueSubmit2");
    const uint64_t attempt=NextNativeSubmitSerial(); const uint32_t depth=++NativeSubmitDepth;
    Json record=NativeSubmitInfo2(submitCount,submits,"vkQueueSubmit2"); record["queue"]=Id(queue); record["device"]=data?Id(data->device):0; record["fence"]=Id(fence);
    VkResult result=VK_ERROR_EXTENSION_NOT_PRESENT;
    if(next) result=next(queue,submitCount,submits,fence);
    const uint64_t returned=NextNativeSubmitSerial(); --NativeSubmitDepth;
    AppendNativeSubmitRecord(std::move(record),attempt,returned,depth,int32_t(result)); Flush(); return result;
}
static VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue,uint32_t submitCount,const VkSubmitInfo2 *submits,VkFence fence)
{
    const auto data=Queue(queue); const auto next=Next<PFN_vkQueueSubmit2KHR>(data,"vkQueueSubmit2KHR");
    const uint64_t attempt=NextNativeSubmitSerial(); const uint32_t depth=++NativeSubmitDepth;
    Json record=NativeSubmitInfo2(submitCount,submits,"vkQueueSubmit2KHR"); record["queue"]=Id(queue); record["device"]=data?Id(data->device):0; record["fence"]=Id(fence);
    VkResult result=VK_ERROR_EXTENSION_NOT_PRESENT;
    if(next) result=next(queue,submitCount,submits,fence);
    const uint64_t returned=NextNativeSubmitSerial(); --NativeSubmitDepth;
    AppendNativeSubmitRecord(std::move(record),attempt,returned,depth,int32_t(result)); Flush(); return result;
}
static VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue)
{
    const auto data=Queue(queue); const auto next=Next<PFN_vkQueueWaitIdle>(data,"vkQueueWaitIdle");
    const uint64_t attempt=NextNativeSubmitSerial(); VkResult result=next?next(queue):VK_ERROR_EXTENSION_NOT_PRESENT; const uint64_t returned=NextNativeSubmitSerial();
    AppendNativeIdleRecord("vkQueueWaitIdle",Id(queue),attempt,returned,int32_t(result)); Flush(); return result;
}
static VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device)
{
    const auto data=Device(device); const auto next=Next<PFN_vkDeviceWaitIdle>(data,"vkDeviceWaitIdle");
    const uint64_t attempt=NextNativeSubmitSerial(); VkResult result=next?next(device):VK_ERROR_EXTENSION_NOT_PRESENT; const uint64_t returned=NextNativeSubmitSerial();
    AppendNativeIdleRecord("vkDeviceWaitIdle",Id(device),attempt,returned,int32_t(result)); Flush(); return result;
}
static VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer command,VkCommandBufferResetFlags flags)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkResetCommandBuffer>(data,"vkResetCommandBuffer"); if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    asteria_live_scene::GlobalRuntime().OnResetCommandBuffer(command);
    DescriptorEvidence.Begin(Id(data->device),Id(command),false); return next(command,flags);
}
static void VKAPI_CALL CmdBindPipeline(VkCommandBuffer command,VkPipelineBindPoint point,VkPipeline pipeline)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdBindPipeline>(data,"vkCmdBindPipeline"); if (!next) return;
    next(command,point,pipeline);
    try { DescriptorEvidence.BindPipeline(Id(data->device),Id(command),point,Id(pipeline)); } catch (...) { DescriptorEvidence.Poison(Id(data->device)); }
}
static void VKAPI_CALL CmdBindDescriptorSets(VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet *sets,uint32_t dynamic_count,const uint32_t *dynamic)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdBindDescriptorSets>(data,"vkCmdBindDescriptorSets"); if (!next) return;
    next(command,point,layout,first,count,sets,dynamic_count,dynamic);
    try { DescriptorEvidence.Bind(Id(data->device),Id(command),point,Id(layout),first,count,sets,dynamic_count,dynamic); } catch (...) { DescriptorEvidence.Poison(Id(data->device)); }
}

static bool CopyViewportValues(const VkViewport *input,float output[6]) noexcept
{
    __try
    {
        if (!input) return false;
        output[0]=input->x; output[1]=input->y; output[2]=input->width;
        output[3]=input->height; output[4]=input->minDepth; output[5]=input->maxDepth;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void ObserveNativeViewport(VkCommandBuffer command,uint32_t source,uint32_t first,uint32_t count,const VkViewport *viewports)
{
    if (count!=1) { try { const auto data=CommandDevice(command); if(data) DescriptorEvidence.SetViewport(Id(data->device),Id(command),source,first,count,nullptr,false); } catch (...) {} return; }
    float values[6]{}; const bool readable=CopyViewportValues(viewports,values);
    try { const auto data=CommandDevice(command); if(data) DescriptorEvidence.SetViewport(Id(data->device),Id(command),source,first,count,values,readable); } catch (...) {}
}

static void VKAPI_CALL CmdSetViewport(VkCommandBuffer command,uint32_t first,uint32_t count,const VkViewport *viewports)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdSetViewport>(data,"vkCmdSetViewport"); if (!next) return;
    next(command,first,count,viewports);
    ObserveNativeViewport(command,ASTERIA_VIEWPORT_SOURCE_LEGACY,first,count,viewports);
}

static void VKAPI_CALL CmdSetViewportWithCount(VkCommandBuffer command,uint32_t count,const VkViewport *viewports)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdSetViewportWithCount>(data,"vkCmdSetViewportWithCount"); if (!next) return;
    next(command,count,viewports);
    ObserveNativeViewport(command,ASTERIA_VIEWPORT_SOURCE_CORE_WITH_COUNT,0,count,viewports);
}

static void VKAPI_CALL CmdSetViewportWithCountEXT(VkCommandBuffer command,uint32_t count,const VkViewport *viewports)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdSetViewportWithCountEXT>(data,"vkCmdSetViewportWithCountEXT"); if (!next) return;
    next(command,count,viewports);
    ObserveNativeViewport(command,ASTERIA_VIEWPORT_SOURCE_EXT_WITH_COUNT,0,count,viewports);
}
static void VKAPI_CALL CmdBindShadersEXT(VkCommandBuffer command,uint32_t count,const VkShaderStageFlagBits *stages,const VkShaderEXT *shaders)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdBindShadersEXT>(data,"vkCmdBindShadersEXT"); if (!next) return;
    next(command,count,stages,shaders);
    DescriptorEvidence.Unsupported(Id(data->device),Id(command));
}
static void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer command,uint32_t count,const VkCommandBuffer *commands)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdExecuteCommands>(data,"vkCmdExecuteCommands"); if (!next) return;
    DescriptorEvidence.Unsupported(Id(data->device),Id(command)); next(command,count,commands);
}
static void VKAPI_CALL CmdPushDescriptorSetKHR(VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,uint32_t set,uint32_t count,const VkWriteDescriptorSet *writes)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdPushDescriptorSetKHR>(data,"vkCmdPushDescriptorSetKHR"); if (!next) return;
    if (point==VK_PIPELINE_BIND_POINT_GRAPHICS) DescriptorEvidence.Unsupported(Id(data->device),Id(command)); next(command,point,layout,set,count,writes);
}
static void VKAPI_CALL CmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer command,VkDescriptorUpdateTemplate handle,VkPipelineLayout layout,uint32_t set,const void *values)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdPushDescriptorSetWithTemplateKHR>(data,"vkCmdPushDescriptorSetWithTemplateKHR"); if (!next) return;
    DescriptorEvidence.Unsupported(Id(data->device),Id(command)); next(command,handle,layout,set,values);
}
static void VKAPI_CALL CmdBindDescriptorSets2KHR(VkCommandBuffer command,const VkBindDescriptorSetsInfoKHR *info)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdBindDescriptorSets2KHR>(data,"vkCmdBindDescriptorSets2KHR"); if (!next) return;
    DescriptorEvidence.Unsupported(Id(data->device),Id(command)); next(command,info);
}
static void VKAPI_CALL CmdBindDescriptorBuffersEXT(VkCommandBuffer command,uint32_t count,const VkDescriptorBufferBindingInfoEXT *info)
{
    const auto data=CommandDevice(command); const auto next=Next<PFN_vkCmdBindDescriptorBuffersEXT>(data,"vkCmdBindDescriptorBuffersEXT"); if (!next) return;
    DescriptorEvidence.Unsupported(Id(data->device),Id(command)); next(command,count,info);
}

static void VKAPI_CALL CmdBeginRendering(VkCommandBuffer command, const VkRenderingInfo *renderingInfo)
{
    const auto data = CommandDevice(command);
    const auto next = Next<PFN_vkCmdBeginRendering>(data, "vkCmdBeginRendering");
    if (!next) return;
    if (data) asteria_live_scene::GlobalRuntime().OnBeginRendering(data->device, data->next_gdpa, command, renderingInfo, false);
    next(command, renderingInfo);
}

static void VKAPI_CALL CmdBeginRenderingKHR(VkCommandBuffer command, const VkRenderingInfo *renderingInfo)
{
    const auto data = CommandDevice(command);
    const auto next = Next<PFN_vkCmdBeginRenderingKHR>(data, "vkCmdBeginRenderingKHR");
    if (!next) return;
    if (data) asteria_live_scene::GlobalRuntime().OnBeginRendering(data->device, data->next_gdpa, command, renderingInfo, true);
    next(command, renderingInfo);
}

static void VKAPI_CALL CmdEndRendering(VkCommandBuffer command)
{
    const auto data = CommandDevice(command);
    const auto next = Next<PFN_vkCmdEndRendering>(data, "vkCmdEndRendering");
    if (!next) return;
    next(command);
    if (data) asteria_live_scene::GlobalRuntime().OnAfterEndRendering(data->device, command);
}

static void VKAPI_CALL CmdEndRenderingKHR(VkCommandBuffer command)
{
    const auto data = CommandDevice(command);
    const auto next = Next<PFN_vkCmdEndRenderingKHR>(data, "vkCmdEndRenderingKHR");
    if (!next) return;
    next(command);
    if (data) asteria_live_scene::GlobalRuntime().OnAfterEndRendering(data->device, command);
}

static void VKAPI_CALL CmdClearAttachments(VkCommandBuffer command, uint32_t attachmentCount, const VkClearAttachment *pAttachments, uint32_t rectCount, const VkClearRect *pRects)
{
    const auto data = CommandDevice(command);
    const auto next = Next<PFN_vkCmdClearAttachments>(data, "vkCmdClearAttachments");
    if (!next) return;
    if (data) asteria_live_scene::GlobalRuntime().OnBeforeClearAttachments(data->device, data->next_gdpa, command, attachmentCount, pAttachments, rectCount, pRects);
    next(command, attachmentCount, pAttachments, rectCount, pRects);
}

static void VKAPI_CALL CmdDraw(VkCommandBuffer command, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    const auto data = CommandDevice(command);
    const auto next = Next<PFN_vkCmdDraw>(data, "vkCmdDraw");
    if (!next) return;
    if (data) asteria_live_scene::GlobalRuntime().OnDraw(data->device, command, DescriptorEvidence, MappedBuffers);
    next(command, vertexCount, instanceCount, firstVertex, firstInstance);
}

static void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer command, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    const auto data = CommandDevice(command);
    const auto next = Next<PFN_vkCmdDrawIndexed>(data, "vkCmdDrawIndexed");
    if (!next) return;
    if (data) asteria_live_scene::GlobalRuntime().OnDraw(data->device, command, DescriptorEvidence, MappedBuffers);
    next(command, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL AsteriaValidateNextDispatch(VkDevice device, VkQueue queue)
{
    try
    {
        const auto data = Device(device);
        const auto queueData = Queue(queue);
        if (!data || queueData != data) return VK_ERROR_INITIALIZATION_FAILED;
        const auto saved = Next<PFN_vkQueueWaitIdle>(data, "vkQueueWaitIdle");
        const auto resolved = reinterpret_cast<PFN_vkQueueWaitIdle>(AsteriaResolveVulkanDeviceFunction(device, "vkQueueWaitIdle"));
        if (!saved || saved != resolved) return VK_ERROR_INITIALIZATION_FAILED;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(data->physical, &properties);
        const auto result = vkQueueWaitIdle(queue);
        {
            const std::lock_guard<std::mutex> lock(StateMutex);
            State["devices"][Name(Id(device))]["saved_next_dispatch_verified"] = result == VK_SUCCESS && properties.deviceID != 0;
            State["devices"][Name(Id(device))]["dispatch_probe_gpu"] = properties.deviceName;
            State["devices"][Name(Id(device))]["dispatch_probe_result"] = int(result);
        }
        Flush(true);
        return result;
    }
    catch (...)
    {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
}

static bool CopyFrameGenerationCameraInput(const AsteriaFrameGenerationCameraV1 *camera, AsteriaFrameGenerationCameraV1 *outCopy)
{
    __try
    {
        if (!camera || camera->struct_size != sizeof(AsteriaFrameGenerationCameraV1) || camera->api_version != ASTERIA_FG_CAMERA_API_VERSION_1)
            return false;
        *outCopy = *camera;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

extern "C" BOOL __stdcall AsteriaSubmitFrameGenerationCamera(const AsteriaFrameGenerationCameraV1 *camera)
{
    AsteriaFrameGenerationCameraV1 local{};
    if (!CopyFrameGenerationCameraInput(camera, &local))
        return FALSE;
    const std::lock_guard<std::mutex> lock(FgCamera.mutex);
    FgCamera.valid = local.valid != 0;
    FgCamera.inGame = local.in_game != 0;
    FgCamera.reset = local.reset != 0;
    FgCamera.nearPlane = local.camera_near;
    FgCamera.farPlane = local.camera_far;
    FgCamera.fovVerticalRadians = local.camera_fov_vertical_radians;
    std::memcpy(FgCamera.position, local.camera_position, sizeof(FgCamera.position));
    std::memcpy(FgCamera.up, local.camera_up, sizeof(FgCamera.up));
    std::memcpy(FgCamera.right, local.camera_right, sizeof(FgCamera.right));
    std::memcpy(FgCamera.forward, local.camera_forward, sizeof(FgCamera.forward));
    FgCamera.sampleCount += 1;
    return TRUE;
}

static bool IsFgSwapchainHandle(VkSwapchainKHR swapchain)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    const auto alias = FgSwapchainAliases.find(Id(swapchain));
    const uint64_t key = alias == FgSwapchainAliases.end() ? Id(swapchain) : alias->second;
    return FgSwapchains.find(key) != FgSwapchains.end();
}

static bool IsRetiredFgSwapchain(VkSwapchainKHR swapchain)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    const auto alias = FgSwapchainAliases.find(Id(swapchain));
    const uint64_t key = alias == FgSwapchainAliases.end() ? Id(swapchain) : alias->second;
    const auto found = FgSwapchains.find(key);
    return found != FgSwapchains.end() && found->second->retiredForAcquisition;
}

static VkSwapchainKHR ProxySwapchainFor(VkSwapchainKHR swapchain)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    const auto alias = FgSwapchainAliases.find(Id(swapchain));
    const uint64_t key = alias == FgSwapchainAliases.end() ? Id(swapchain) : alias->second;
    const auto found = FgSwapchains.find(key);
    return found == FgSwapchains.end() || !found->second ? swapchain : found->second->proxySwapchain;
}

static void RecordAcquireException(VkSwapchainKHR swapchain)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    State["last_fg_acquire_exception"] = true;
    State["last_fg_acquire_swapchain"] = Id(swapchain);
}

static VkResult SafeAcquireNextImageFFX(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
    __try { return vkAcquireNextImageFFX(device, swapchain, timeout, semaphore, fence, index); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        RecordAcquireException(swapchain);
        return VK_ERROR_DEVICE_LOST;
    }
}

static void RecordGetImagesException(VkSwapchainKHR swapchain)
{
    const std::lock_guard<std::mutex> lock(StateMutex);
    State["last_fg_get_images_exception"] = true;
    State["last_fg_get_images_swapchain"] = Id(swapchain);
}

static VkResult SafeGetSwapchainImagesFFX(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)
{
    __try { return vkGetSwapchainImagesFFX(device, swapchain, count, images); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        RecordGetImagesException(swapchain);
        return VK_ERROR_DEVICE_LOST;
    }
}

static VkResult VKAPI_CALL CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain)

{

    if (!info || !swapchain) return VK_ERROR_INITIALIZATION_FAILED;

    const auto data = Device(device);

    const auto next = Next<PFN_vkCreateSwapchainKHR>(data, "vkCreateSwapchainKHR");

    if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;

    Json metadata = SwapchainInfo(info);

    metadata["frame_input_provider_available"] = bool(asteria_fg_runtime::CurrentFrameInputProvider());

    metadata["frame_input_status"] = asteria_fg_runtime::CurrentFrameInputProvider() ?

        "awaiting_valid_frame_inputs" : "valid_frame_input_provider_unavailable";

    bool oldFg = false;

    asteria_fg_runtime::FgSwapchainState *oldState = nullptr;

    asteria_fg_runtime::FgDeviceQueues fgQueues{};

    {

        const std::lock_guard<std::mutex> lock(StateMutex);

        const auto oldAlias = FgSwapchainAliases.find(Id(info->oldSwapchain));
        const uint64_t oldKey = oldAlias == FgSwapchainAliases.end() ? Id(info->oldSwapchain) : oldAlias->second;
        const auto oldIt = FgSwapchains.find(oldKey);

        if (oldIt != FgSwapchains.end())

        {

            if (!oldIt->second || oldIt->second->device != device)

                return VK_ERROR_SURFACE_LOST_KHR;

            oldFg = true;

            oldState = oldIt->second.get();

            if (oldState->retiredForAcquisition) return VK_ERROR_OUT_OF_DATE_KHR;

        }
        else if (asteria_vk::IsProxyToken(info->oldSwapchain))
        {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        const auto qIt = FgDeviceQueuesMap.find(Id(device));

        if (qIt != FgDeviceQueuesMap.end()) fgQueues = qIt->second;

    }
if (oldState && !FgSwapchainRuntime.wait(*oldState)) return VK_ERROR_DEVICE_LOST;

    const auto retireOld = [&] {

        if (!oldState) return;

        oldState->retiredForAcquisition = true;

        if (oldState->inputSession) oldState->inputSession->RetireAfterGpuIdle();

        const std::lock_guard<std::mutex> lock(StateMutex);

        State["swapchains"][Name(Id(info->oldSwapchain))]["native_retired"] = true;

    };

    metadata["old_proxy_preserved_until_destroy"] = oldFg;

    VkResult fgResult = VK_ERROR_FEATURE_NOT_PRESENT;

    if (FgSwapchainRuntime.requested() && fgQueues.available &&

        info->imageExtent.width >= 64 && info->imageExtent.height >= 64)

    {

        VkSwapchainCreateInfoKHR localInfo = *info;

        localInfo.minImageCount = std::clamp(localInfo.minImageCount, 2u, uint32_t(FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_BUFFER_COUNT));

        localInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};

        localInfo.pNext = nullptr;

        for (auto entry = static_cast<const VkBaseInStructure *>(info->pNext); entry; entry = entry->pNext)

        {

            if (entry->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)

            {

                const auto *srcList = reinterpret_cast<const VkImageFormatListCreateInfo *>(entry);

                formatList.viewFormatCount = srcList->viewFormatCount;

                formatList.pViewFormats = srcList->pViewFormats;

                formatList.pNext = nullptr;

                localInfo.pNext = &formatList;

                break;

            }

        }

        std::unique_ptr<asteria_fg_runtime::FgSwapchainState> fgState;

        try

        {

            fgState = std::make_unique<asteria_fg_runtime::FgSwapchainState>();

            fgResult = FgSwapchainRuntime.initialize(

                *fgState, device, data->physical, data->next_gdpa, fgQueues, &localInfo, allocator, &FgCamera);

        }

        catch (const std::bad_alloc &)

        {

            fgResult = VK_ERROR_OUT_OF_HOST_MEMORY;

        }

        const bool nativeAttempted = fgState && fgState->nativeCreateAttempted;

        if (nativeAttempted && oldFg) retireOld();

        if (fgResult == VK_SUCCESS && fgState && fgState->proxySwapchain != VK_NULL_HANDLE)

        {

            *swapchain = ReturnNativeSwapchainHandle() && fgState->nativeSwapchain ? fgState->nativeSwapchain : fgState->proxySwapchain;

            {

                const std::lock_guard<std::mutex> lock(StateMutex);

                Count("create_swapchain");

                State["last_create_swapchain_result"] = int(fgResult);

                metadata["native_swapchain"] = Id(*swapchain);

                metadata["device"] = Id(device);

                metadata["swapchain_adapter_active"] = true;

                metadata["frame_generation_active"] = false;

                State["swapchains"][Name(Id(*swapchain))] = std::move(metadata);

                if (info->oldSwapchain != VK_NULL_HANDLE) Count("recreate_swapchain");

                const uint64_t proxyKey = Id(fgState->proxySwapchain);
                const uint64_t returnedKey = Id(*swapchain);
                FgSwapchains.emplace(proxyKey, std::move(fgState));
                if (returnedKey != proxyKey) FgSwapchainAliases[returnedKey] = proxyKey;

                UpdateFrameGenerationMode();

            }

            Flush(true);

            return VK_SUCCESS;

        }

        if (fgState) FgSwapchainRuntime.destroy(*fgState);

        if (fgResult == VK_SUCCESS) fgResult = VK_ERROR_INITIALIZATION_FAILED;

        if (nativeAttempted && info->oldSwapchain != VK_NULL_HANDLE)

        {

            {

                const std::lock_guard<std::mutex> lock(StateMutex);

                State["last_create_swapchain_result"] = int(fgResult);

                State["last_fg_create_swapchain_result"] = int(fgResult);

            }

            Flush(true);

            return fgResult;

        }

    }

    VkSwapchainCreateInfoKHR nativeInfo = *info;

    if (oldFg)

    {

        nativeInfo.oldSwapchain = oldState->nativeSwapchain;

        if (nativeInfo.oldSwapchain == VK_NULL_HANDLE) return VK_ERROR_SURFACE_LOST_KHR;

        retireOld();

    }

    VkSwapchainKHR nativeSwapchain = VK_NULL_HANDLE;

    const VkResult result = next(device, &nativeInfo, allocator, &nativeSwapchain);

    {

        const std::lock_guard<std::mutex> lock(StateMutex);

        Count("create_swapchain");

        State["last_create_swapchain_result"] = int(result);

        State["last_fg_create_swapchain_result"] = int(fgResult);

        if (result == VK_SUCCESS)

        {

            *swapchain = nativeSwapchain;

            metadata["native_swapchain"] = Id(*swapchain);

            metadata["device"] = Id(device);

            metadata["frame_generation_active"] = false;

            metadata["native_old_swapchain"] = Id(nativeInfo.oldSwapchain);

            State["swapchains"][Name(Id(*swapchain))] = std::move(metadata);

            if (info->oldSwapchain != VK_NULL_HANDLE) Count("recreate_swapchain");

        }

        UpdateFrameGenerationMode();

    }

    Flush(true);

    return result;

}



static void VKAPI_CALL DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator)

{

    std::unique_ptr<asteria_fg_runtime::FgSwapchainState> fgState;

    bool wasFg = false;

    {

        const std::lock_guard<std::mutex> lock(StateMutex);

        const auto alias = FgSwapchainAliases.find(Id(swapchain));
        const uint64_t key = alias == FgSwapchainAliases.end() ? Id(swapchain) : alias->second;
        const auto it = FgSwapchains.find(key);

        if (it != FgSwapchains.end())

        {

            wasFg = true;

            fgState = std::move(it->second);

            FgSwapchains.erase(it);
            for (auto aliasIt = FgSwapchainAliases.begin(); aliasIt != FgSwapchainAliases.end(); )
                if (aliasIt->second == key) aliasIt = FgSwapchainAliases.erase(aliasIt); else ++aliasIt;

            UpdateFrameGenerationMode();

        }

    }

    if (wasFg)

    {

        if (fgState && fgState->proxySwapchain != VK_NULL_HANDLE)

            FgSwapchainRuntime.destroy(*fgState);

        { const std::lock_guard<std::mutex> lock(StateMutex); Count("destroy_swapchain"); State["swapchains"][Name(Id(swapchain))]["live"] = false; }

        Flush(true);

        return;

    }

    const auto next = Next<PFN_vkDestroySwapchainKHR>(Device(device), "vkDestroySwapchainKHR");

    if (!next) return;

    next(device, swapchain, allocator);

    { const std::lock_guard<std::mutex> lock(StateMutex); Count("destroy_swapchain"); State["swapchains"][Name(Id(swapchain))]["live"] = false; }

    Flush(true);

}



static VkResult VKAPI_CALL GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)

{

    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;

    if (IsFgSwapchainHandle(swapchain))

    {

        result = SafeGetSwapchainImagesFFX(device, ProxySwapchainFor(swapchain), count, images);

    }

    else

    {

        const auto next = Next<PFN_vkGetSwapchainImagesKHR>(Device(device), "vkGetSwapchainImagesKHR");

        if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;

        result = next(device, swapchain, count, images);

    }

    {

        const std::lock_guard<std::mutex> lock(StateMutex);

        Count("get_swapchain_images");

        if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count)

        {

            auto &record = State["swapchains"][Name(Id(swapchain))];

            record["image_count"] = *count;

            if (images)

            {

                record["images"] = Json::array();

                for (uint32_t i = 0; i < *count; ++i) record["images"].push_back(Id(images[i]));

            }

        }

    }

    Flush();

    return result;

}



static void RecordAcquire(const char *method, VkSwapchainKHR swapchain, VkSemaphore semaphore, VkFence fence, uint64_t timeout, VkResult result, const uint32_t *index)

{

    std::shared_ptr<asteria_fg_runtime::FrameInputSession> session;

    {

        const std::lock_guard<std::mutex> lock(StateMutex);

        Count(method);

        State["last_acquire_result"] = int(result);

        auto &record = State["swapchains"][Name(Id(swapchain))];

        record["last_acquire_semaphore"] = Id(semaphore);

        record["last_acquire_fence"] = Id(fence);

        record["last_acquire_timeout"] = timeout;

        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)

        {

            Count("successful_acquires");

            record["acquires"] = record.value("acquires", uint64_t(0)) + 1;

            if (index)

            {

                record["last_acquired_index"] = *index;

                const auto fg = FgSwapchains.find(Id(swapchain));

                if (fg != FgSwapchains.end()) session = fg->second->inputSession;

            }

        }

    }

    if (session && index) session->RecordAcquire(*index);

}




static VkResult VKAPI_CALL AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;
    if (IsFgSwapchainHandle(swapchain))
    {
        if (NativePassthroughEnabled() && !asteria_fg_runtime::IsEstimatedInternalInputsRequested())
        {
            VkSwapchainKHR native = VK_NULL_HANDLE;
            { const std::lock_guard<std::mutex> lock(StateMutex); const auto alias = FgSwapchainAliases.find(Id(swapchain)); const uint64_t key = alias == FgSwapchainAliases.end() ? Id(swapchain) : alias->second; const auto found = FgSwapchains.find(key); if (found != FgSwapchains.end()) native = found->second->nativeSwapchain; }
            const auto next = Next<PFN_vkAcquireNextImageKHR>(Device(device), "vkAcquireNextImageKHR");
            result = native && next ? next(device, native, timeout, semaphore, fence, index) : VK_ERROR_SURFACE_LOST_KHR;
        }
        else result = IsRetiredFgSwapchain(swapchain) ? VK_ERROR_OUT_OF_DATE_KHR :
            SafeAcquireNextImageFFX(device, ProxySwapchainFor(swapchain), timeout, semaphore, fence, index);
    }
    else
    {
        const auto next = Next<PFN_vkAcquireNextImageKHR>(Device(device), "vkAcquireNextImageKHR");
        if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
        result = next(device, swapchain, timeout, semaphore, fence, index);
    }
    RecordAcquire("acquire_next_image", swapchain, semaphore, fence, timeout, result, index);
    Flush();
    return result;
}

static VkResult VKAPI_CALL AcquireNextImage2(VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *index)
{
    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;
    if (info && IsFgSwapchainHandle(info->swapchain))
    {
        result = IsRetiredFgSwapchain(info->swapchain) ? VK_ERROR_OUT_OF_DATE_KHR :
            SafeAcquireNextImageFFX(device, ProxySwapchainFor(info->swapchain), info->timeout, info->semaphore, info->fence, index);
    }
    else
    {
        const auto next = Next<PFN_vkAcquireNextImage2KHR>(Device(device), "vkAcquireNextImage2KHR");
        if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
        result = next(device, info, index);
    }
    RecordAcquire("acquire_next_image2", info->swapchain, info->semaphore, info->fence, info->timeout, result, index);
    { const std::lock_guard<std::mutex> lock(StateMutex); State["swapchains"][Name(Id(info->swapchain))]["last_acquire_device_mask"] = info->deviceMask; }
    Flush();
    return result;
}

static VkResult VKAPI_CALL QueuePresent(VkQueue queue, const VkPresentInfoKHR *info)
{
    if (!info || !info->pSwapchains || !info->pImageIndices) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;
    uint64_t inputAttempts = 0;
    uint64_t interpolationDispatches = 0;
    uint64_t ffxPresentCount = 0;
    asteria_fg_runtime::FgSwapchainState *fg = nullptr;
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        for (uint32_t index = 0; index < info->swapchainCount; ++index)
        {
            const auto alias = FgSwapchainAliases.find(Id(info->pSwapchains[index]));
            const uint64_t key = alias == FgSwapchainAliases.end() ? Id(info->pSwapchains[index]) : alias->second;
            const auto it = FgSwapchains.find(key);
            if (it != FgSwapchains.end())
            {
                if (info->swapchainCount != 1) return VK_ERROR_FEATURE_NOT_PRESENT;
                fg = it->second.get();
            }
            else if (asteria_vk::IsProxyToken(info->pSwapchains[index]))
                return VK_ERROR_SURFACE_LOST_KHR;
        }
    }
    const bool isFgPresent = fg != nullptr;
    if (fg && info->pNext && NativePassthroughEnabled())
    {
        VkPresentInfoKHR nativeInfo = *info;
        VkSwapchainKHR nativeSwapchain = fg->nativeSwapchain;
        const auto next = Next<PFN_vkQueuePresentKHR>(Queue(queue), "vkQueuePresentKHR");
        if (!next || !nativeSwapchain) return VK_ERROR_EXTENSION_NOT_PRESENT;
        nativeInfo.pSwapchains = &nativeSwapchain;
        result = next(queue, &nativeInfo);
        if (info->pResults) info->pResults[0] = result;
        {
            const std::lock_guard<std::mutex> lock(StateMutex);
            Count("queue_present");
            State["last_present_result"] = int(result);
            State["successful_present_calls"] = State.value("successful_present_calls", uint64_t(0)) +
                (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ? 1 : 0);
        }
        Flush();
        return result;
    }
    bool inputReady = false;
    bool callbackRecorded = false;
    uint32_t motionSource = 0;
    if (isFgPresent)
    {
        if (info->pNext && !NativePassthroughEnabled()) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkPresentInfoKHR ffxPresent = *info;
        ffxPresent.pResults = nullptr;
        if (NativePassthroughEnabled()) ffxPresent.pNext = nullptr;
        VkSwapchainKHR proxySwapchain = fg->proxySwapchain;
        ffxPresent.pSwapchains = &proxySwapchain;
        if (asteria_fg_runtime::AdvanceSwapchainFrameConfig(*fg, info->pImageIndices[0]))
        {
            const auto input = std::atomic_load_explicit(&fg->currentInputs, std::memory_order_acquire);
            inputReady = bool(input);
            if (input) motionSource = uint32_t(input->motionSource);
            const auto before = fg->interpolationDispatches.load();
            result = vkQueuePresentFFX(queue, &ffxPresent);
            callbackRecorded = fg->interpolationDispatches.load() > before;
            ffxPresentCount = getLastPresentCountFFX(proxySwapchain);
        }
        else result = VK_ERROR_INITIALIZATION_FAILED;
        if (info->pResults) info->pResults[0] = result;
        inputAttempts = fg->frameID;
        interpolationDispatches = fg->interpolationDispatches.load();
    }
    else
    {
        const auto next = Next<PFN_vkQueuePresentKHR>(Queue(queue), "vkQueuePresentKHR");
        if (!next) return VK_ERROR_EXTENSION_NOT_PRESENT;
        result = next(queue, info);
    }
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Count("queue_present");
        State["last_present_result"] = int(result);
        State["freeaim_plugin_pinned"] = false;
        UpdateFrameGenerationMode();
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) Count("successful_present_calls");
        if (isFgPresent)
        {
            const auto nativeStats = AsteriaGetNativePresentStats();
            State["native_present_calls"] = nativeStats.attempts;
            State["native_present_successes"] = nativeStats.successful_calls;
            State["native_present_failures"] = nativeStats.failed_calls;
            State["native_present_swapchains"] = nativeStats.successful_swapchains;
            const std::lock_guard<std::mutex> camLock(FgCamera.mutex);
            State["fg_camera_valid"] = FgCamera.valid;
            State["fg_camera_in_game"] = FgCamera.inGame;
            State["fg_camera_sample_count"] = FgCamera.sampleCount;
        }
        Json waits = Json::array();
        for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) waits.push_back(Id(info->pWaitSemaphores[i]));
        State["last_present_wait_semaphores"] = waits;
        for (uint32_t i = 0; i < info->swapchainCount; ++i)
        {
            auto &record = State["swapchains"][Name(Id(info->pSwapchains[i]))];
            const VkResult single_result = info->pResults ? info->pResults[i] : result;
            record["last_present_result"] = int(single_result);
            record["last_presented_index"] = info->pImageIndices[i];
            record["last_present_queue"] = Id(queue);
            if (isFgPresent)
            {
                record["frame_input_attempts"] = inputAttempts;
                record["interpolation_dispatches"] = interpolationDispatches;
                record["frame_generation_configured"] = inputReady;
                record["frame_generation_active"] = inputReady && callbackRecorded &&
                    (single_result == VK_SUCCESS || single_result == VK_SUBOPTIMAL_KHR);
                record["present_count_ffx"] = ffxPresentCount;
                record["frame_input_ready"] = inputReady;
                record["motion_input_source"] = motionSource;
                record["frame_input_status"] = inputReady ? "validated_frame_inputs" : "frame_inputs_unavailable";
                const auto liveDiag = asteria_live_scene::GlobalRuntime().QueryDiagnostics();
                record["live_depth_snapshots_recorded"] = liveDiag.depthSnapshotsRecorded;
                record["live_depth_resolves_dispatched"] = liveDiag.depthResolvesDispatched;
                record["live_motion_reconstructions_dispatched"] = liveDiag.motionReconstructionsDispatched;
                record["live_camera_frames_committed"] = liveDiag.cameraFramesCommitted;
                record["live_last_main_world_draws"] = liveDiag.lastMainWorldDraws;
                record["live_last_near_plane"] = liveDiag.lastNearPlane;
                record["live_last_far_plane"] = liveDiag.lastFarPlane;
            }
            if (single_result == VK_SUCCESS || single_result == VK_SUBOPTIMAL_KHR)
            {
                record["presents"] = record.value("presents", uint64_t(0)) + 1;
                Count("successful_presented_swapchains");
            }
        }
    }
    Flush();
    return result;
}

static VkResult VKAPI_CALL AcquireFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain)
{
    if (IsFgSwapchainHandle(swapchain)) return VK_SUCCESS;
    if (asteria_vk::IsProxyToken(swapchain)) return VK_ERROR_SURFACE_LOST_KHR;
    const auto next = Next<PFN_vkAcquireFullScreenExclusiveModeEXT>(Device(device), "vkAcquireFullScreenExclusiveModeEXT");
    return next ? next(device, swapchain) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VkResult VKAPI_CALL ReleaseFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain)
{
    if (IsFgSwapchainHandle(swapchain)) return VK_SUCCESS;
    if (asteria_vk::IsProxyToken(swapchain)) return VK_ERROR_SURFACE_LOST_KHR;
    const auto next = Next<PFN_vkReleaseFullScreenExclusiveModeEXT>(Device(device), "vkReleaseFullScreenExclusiveModeEXT");
    return next ? next(device, swapchain) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&LayerGetDeviceProcAddr);
    const auto data = Device(device);
    const auto native = data ? data->next_gdpa(device, name) : nullptr;
    if (!native) return nullptr;
#define HOOK(native, local) if (std::strcmp(name, #native) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&local)
    HOOK(vkDestroyDevice, DestroyDevice);
    HOOK(vkGetDeviceQueue, GetDeviceQueue);
    HOOK(vkGetDeviceQueue2, GetDeviceQueue2);
    HOOK(vkCreateDescriptorSetLayout, CreateDescriptorSetLayout);
    HOOK(vkDestroyDescriptorSetLayout, DestroyDescriptorSetLayout);
    HOOK(vkCreatePipelineLayout, CreatePipelineLayout);
    HOOK(vkDestroyPipelineLayout, DestroyPipelineLayout);
    HOOK(vkCreateDescriptorPool, CreateDescriptorPool);
    HOOK(vkDestroyDescriptorPool, DestroyDescriptorPool);
    HOOK(vkResetDescriptorPool, ResetDescriptorPool);
    HOOK(vkAllocateDescriptorSets, AllocateDescriptorSets);
    HOOK(vkFreeDescriptorSets, FreeDescriptorSets);
    HOOK(vkUpdateDescriptorSets, UpdateDescriptorSets);
    HOOK(vkCreateDescriptorUpdateTemplate, CreateDescriptorUpdateTemplate);
    HOOK(vkCreateDescriptorUpdateTemplateKHR, CreateDescriptorUpdateTemplateKHR);
    HOOK(vkDestroyDescriptorUpdateTemplate, DestroyDescriptorUpdateTemplate);
    HOOK(vkDestroyDescriptorUpdateTemplateKHR, DestroyDescriptorUpdateTemplateKHR);
    HOOK(vkUpdateDescriptorSetWithTemplate, UpdateDescriptorSetWithTemplate);
    HOOK(vkUpdateDescriptorSetWithTemplateKHR, UpdateDescriptorSetWithTemplateKHR);
    HOOK(vkCreateGraphicsPipelines, CreateGraphicsPipelines);
    HOOK(vkDestroyPipeline, DestroyPipeline);
    HOOK(vkCreateCommandPool, CreateCommandPool);
    HOOK(vkResetCommandPool, ResetCommandPool);
    HOOK(vkBeginCommandBuffer, BeginCommandBuffer);
    HOOK(vkEndCommandBuffer, EndCommandBuffer);
    HOOK(vkQueueSubmit, QueueSubmit);
    HOOK(vkQueueSubmit2, QueueSubmit2);
    HOOK(vkQueueSubmit2KHR, QueueSubmit2KHR);
    HOOK(vkQueueWaitIdle, QueueWaitIdle);
    HOOK(vkDeviceWaitIdle, DeviceWaitIdle);
    HOOK(vkResetCommandBuffer, ResetCommandBuffer);
    HOOK(vkCmdBindPipeline, CmdBindPipeline);
    HOOK(vkCmdSetViewport, CmdSetViewport);
    HOOK(vkCmdSetViewportWithCount, CmdSetViewportWithCount);
    HOOK(vkCmdSetViewportWithCountEXT, CmdSetViewportWithCountEXT);
    HOOK(vkCmdBindShadersEXT, CmdBindShadersEXT);
    HOOK(vkCmdBindDescriptorSets, CmdBindDescriptorSets);
    HOOK(vkCmdExecuteCommands, CmdExecuteCommands);
    HOOK(vkCmdPushDescriptorSetKHR, CmdPushDescriptorSetKHR);
    HOOK(vkCmdPushDescriptorSetWithTemplateKHR, CmdPushDescriptorSetWithTemplateKHR);
    HOOK(vkCmdBindDescriptorSets2KHR, CmdBindDescriptorSets2KHR);
    HOOK(vkCmdBindDescriptorBuffersEXT, CmdBindDescriptorBuffersEXT);
    HOOK(vkCmdBeginRendering, CmdBeginRendering);
    HOOK(vkCmdBeginRenderingKHR, CmdBeginRenderingKHR);
    HOOK(vkCmdEndRendering, CmdEndRendering);
    HOOK(vkCmdEndRenderingKHR, CmdEndRenderingKHR);
    HOOK(vkCmdClearAttachments, CmdClearAttachments);
    HOOK(vkCmdDraw, CmdDraw);
    HOOK(vkCmdDrawIndexed, CmdDrawIndexed);
    HOOK(vkAllocateCommandBuffers, AllocateCommandBuffers);
    HOOK(vkFreeCommandBuffers, FreeCommandBuffers);
    HOOK(vkDestroyCommandPool, DestroyCommandPool);
    HOOK(vkAllocateMemory, AllocateMemory);
    HOOK(vkFreeMemory, FreeMemory);
    HOOK(vkMapMemory, MapMemory);
    HOOK(vkUnmapMemory, UnmapMemory);
    HOOK(vkCreateBuffer, CreateBuffer);
    HOOK(vkDestroyBuffer, DestroyBuffer);
    HOOK(vkCreateImageView, CreateImageView);
    HOOK(vkDestroyImageView, DestroyImageView);
    HOOK(vkCreateImage, CreateImage);
    HOOK(vkDestroyImage, DestroyImage);
    HOOK(vkCreateFramebuffer, CreateFramebuffer);
    HOOK(vkDestroyFramebuffer, DestroyFramebuffer);
    HOOK(vkGetImageSubresourceLayout, GetImageSubresourceLayout);
    HOOK(vkGetImageMemoryRequirements, GetImageMemoryRequirements);
    HOOK(vkBindBufferMemory, BindBufferMemory);
    HOOK(vkBindBufferMemory2, BindBufferMemory2);
    HOOK(vkBindBufferMemory2KHR, BindBufferMemory2KHR);
    HOOK(vkCreateSwapchainKHR, CreateSwapchain);
    HOOK(vkDestroySwapchainKHR, DestroySwapchain);
    HOOK(vkGetSwapchainImagesKHR, GetSwapchainImages);
    HOOK(vkAcquireNextImageKHR, AcquireNextImage);
    HOOK(vkAcquireNextImage2KHR, AcquireNextImage2);
    HOOK(vkQueuePresentKHR, QueuePresent);
    HOOK(vkAcquireFullScreenExclusiveModeEXT, AcquireFullScreenExclusiveModeEXT);
    HOOK(vkReleaseFullScreenExclusiveModeEXT, ReleaseFullScreenExclusiveModeEXT);
#undef HOOK
    return native;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&LayerGetInstanceProcAddr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&LayerGetDeviceProcAddr);
    if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
    const auto data = Instance(instance);
    const auto native = data ? data->next_gipa(instance, name) : nullptr;
    if (!native) return nullptr;
#define HOOK(native, local) if (std::strcmp(name, #native) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&local)
    HOOK(vkDestroyInstance, DestroyInstance);
    HOOK(vkEnumeratePhysicalDevices, EnumeratePhysicalDevices);
    HOOK(vkEnumeratePhysicalDeviceGroups, EnumeratePhysicalDeviceGroups);
    HOOK(vkEnumeratePhysicalDeviceGroupsKHR, EnumeratePhysicalDeviceGroupsKHR);
    HOOK(vkEnumerateDeviceExtensionProperties, EnumerateDeviceExtensionProperties);
    HOOK(vkCreateDevice, CreateDevice);
    HOOK(vkDestroyDevice, DestroyDevice);
    HOOK(vkGetDeviceQueue, GetDeviceQueue);
    HOOK(vkGetDeviceQueue2, GetDeviceQueue2);
    HOOK(vkCreateDescriptorSetLayout, CreateDescriptorSetLayout);
    HOOK(vkDestroyDescriptorSetLayout, DestroyDescriptorSetLayout);
    HOOK(vkCreatePipelineLayout, CreatePipelineLayout);
    HOOK(vkDestroyPipelineLayout, DestroyPipelineLayout);
    HOOK(vkCreateDescriptorPool, CreateDescriptorPool);
    HOOK(vkDestroyDescriptorPool, DestroyDescriptorPool);
    HOOK(vkResetDescriptorPool, ResetDescriptorPool);
    HOOK(vkAllocateDescriptorSets, AllocateDescriptorSets);
    HOOK(vkFreeDescriptorSets, FreeDescriptorSets);
    HOOK(vkUpdateDescriptorSets, UpdateDescriptorSets);
    HOOK(vkCreateDescriptorUpdateTemplate, CreateDescriptorUpdateTemplate);
    HOOK(vkCreateDescriptorUpdateTemplateKHR, CreateDescriptorUpdateTemplateKHR);
    HOOK(vkDestroyDescriptorUpdateTemplate, DestroyDescriptorUpdateTemplate);
    HOOK(vkDestroyDescriptorUpdateTemplateKHR, DestroyDescriptorUpdateTemplateKHR);
    HOOK(vkUpdateDescriptorSetWithTemplate, UpdateDescriptorSetWithTemplate);
    HOOK(vkUpdateDescriptorSetWithTemplateKHR, UpdateDescriptorSetWithTemplateKHR);
    HOOK(vkCreateGraphicsPipelines, CreateGraphicsPipelines);
    HOOK(vkDestroyPipeline, DestroyPipeline);
    HOOK(vkCreateCommandPool, CreateCommandPool);
    HOOK(vkResetCommandPool, ResetCommandPool);
    HOOK(vkBeginCommandBuffer, BeginCommandBuffer);
    HOOK(vkEndCommandBuffer, EndCommandBuffer);
    HOOK(vkQueueSubmit, QueueSubmit);
    HOOK(vkQueueSubmit2, QueueSubmit2);
    HOOK(vkQueueSubmit2KHR, QueueSubmit2KHR);
    HOOK(vkQueueWaitIdle, QueueWaitIdle);
    HOOK(vkDeviceWaitIdle, DeviceWaitIdle);
    HOOK(vkResetCommandBuffer, ResetCommandBuffer);
    HOOK(vkCmdBindPipeline, CmdBindPipeline);
    HOOK(vkCmdSetViewport, CmdSetViewport);
    HOOK(vkCmdSetViewportWithCount, CmdSetViewportWithCount);
    HOOK(vkCmdSetViewportWithCountEXT, CmdSetViewportWithCountEXT);
    HOOK(vkCmdBindShadersEXT, CmdBindShadersEXT);
    HOOK(vkCmdBindDescriptorSets, CmdBindDescriptorSets);
    HOOK(vkCmdExecuteCommands, CmdExecuteCommands);
    HOOK(vkCmdPushDescriptorSetKHR, CmdPushDescriptorSetKHR);
    HOOK(vkCmdPushDescriptorSetWithTemplateKHR, CmdPushDescriptorSetWithTemplateKHR);
    HOOK(vkCmdBindDescriptorSets2KHR, CmdBindDescriptorSets2KHR);
    HOOK(vkCmdBindDescriptorBuffersEXT, CmdBindDescriptorBuffersEXT);
    HOOK(vkCmdBeginRendering, CmdBeginRendering);
    HOOK(vkCmdBeginRenderingKHR, CmdBeginRenderingKHR);
    HOOK(vkCmdEndRendering, CmdEndRendering);
    HOOK(vkCmdEndRenderingKHR, CmdEndRenderingKHR);
    HOOK(vkCmdClearAttachments, CmdClearAttachments);
    HOOK(vkCmdDraw, CmdDraw);
    HOOK(vkCmdDrawIndexed, CmdDrawIndexed);
    HOOK(vkAllocateCommandBuffers, AllocateCommandBuffers);
    HOOK(vkFreeCommandBuffers, FreeCommandBuffers);
    HOOK(vkDestroyCommandPool, DestroyCommandPool);
    HOOK(vkAllocateMemory, AllocateMemory);
    HOOK(vkFreeMemory, FreeMemory);
    HOOK(vkMapMemory, MapMemory);
    HOOK(vkUnmapMemory, UnmapMemory);
    HOOK(vkCreateBuffer, CreateBuffer);
    HOOK(vkDestroyBuffer, DestroyBuffer);
    HOOK(vkCreateImageView, CreateImageView);
    HOOK(vkDestroyImageView, DestroyImageView);
    HOOK(vkCreateImage, CreateImage);
    HOOK(vkDestroyImage, DestroyImage);
    HOOK(vkCreateFramebuffer, CreateFramebuffer);
    HOOK(vkDestroyFramebuffer, DestroyFramebuffer);
    HOOK(vkGetImageSubresourceLayout, GetImageSubresourceLayout);
    HOOK(vkGetImageMemoryRequirements, GetImageMemoryRequirements);
    HOOK(vkBindBufferMemory, BindBufferMemory);
    HOOK(vkBindBufferMemory2, BindBufferMemory2);
    HOOK(vkBindBufferMemory2KHR, BindBufferMemory2KHR);
    HOOK(vkCreateSwapchainKHR, CreateSwapchain);
    HOOK(vkDestroySwapchainKHR, DestroySwapchain);
    HOOK(vkGetSwapchainImagesKHR, GetSwapchainImages);
    HOOK(vkAcquireNextImageKHR, AcquireNextImage);
    HOOK(vkAcquireNextImage2KHR, AcquireNextImage2);
    HOOK(vkQueuePresentKHR, QueuePresent);
    HOOK(vkAcquireFullScreenExclusiveModeEXT, AcquireFullScreenExclusiveModeEXT);
    HOOK(vkReleaseFullScreenExclusiveModeEXT, ReleaseFullScreenExclusiveModeEXT);
#undef HOOK
    return native;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetPhysicalDeviceProcAddr(VkInstance instance, const char *name)
{
    if (!name) return nullptr;
    const auto data = Instance(instance);
    if (!data) return nullptr;
    const auto native = data->next_physical_gipa ? data->next_physical_gipa(instance, name) : data->next_gipa(instance, name);
    if (native && std::strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice);
    if (native && std::strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&EnumerateDeviceExtensionProperties);
    return native;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version)
{
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || !version->loaderLayerInterfaceVersion)
        return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = std::min(version->loaderLayerInterfaceVersion, 2u);
    version->pfnGetInstanceProcAddr = &LayerGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = &LayerGetDeviceProcAddr;
    if (version->loaderLayerInterfaceVersion >= 2) version->pfnGetPhysicalDeviceProcAddr = &LayerGetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        AsteriaSetVulkanPrototypeFallback(false);
    }
    return TRUE;
}
