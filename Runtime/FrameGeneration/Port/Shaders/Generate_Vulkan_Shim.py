from pathlib import Path
import hashlib
import json
import re


ROOT = Path(__file__).resolve().parent.parent
RESEARCH = ROOT.parent / "Research"
HEADERS = RESEARCH / "vulkan-headers-v1.3.250" / "include" / "vulkan"


PREFIX = r'''#include "AsteriaVulkanShim.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <stdexcept>
#include <string>

namespace
{
    std::atomic<VkInstance> active_instance{VK_NULL_HANDLE};
    std::atomic<VkDevice> active_device{VK_NULL_HANDLE};
    std::atomic<bool> prototype_fallback{true};
    struct InstanceDispatch { PFN_vkGetInstanceProcAddr next; bool prototype; };
    struct DeviceDispatch { VkInstance instance; VkPhysicalDevice physical; PFN_vkGetDeviceProcAddr next; bool prototype; };
    struct CommandOwner { VkDevice device; VkCommandPool pool; };
    std::mutex dispatch_mutex;
    std::unordered_map<VkInstance, InstanceDispatch> instances;
    std::unordered_map<VkPhysicalDevice, VkInstance> physicals;
    std::unordered_map<VkDevice, DeviceDispatch> devices;
    std::unordered_map<VkQueue, VkDevice> queues;
    std::unordered_map<VkCommandBuffer, CommandOwner> commands;
    std::atomic<uint64_t> native_present_attempts{0};
    std::atomic<uint64_t> native_present_successful_calls{0};
    std::atomic<uint64_t> native_present_failed_calls{0};
    std::atomic<uint64_t> native_present_successful_swapchains{0};
    std::atomic<AsteriaSwapchainImagesReplacementFn> swapchain_images_replacement{nullptr};
    std::atomic<AsteriaSwapchainTokenPredicateFn> swapchain_token_predicate{nullptr};

    struct SystemVulkan
    {
        HMODULE module = nullptr;
        PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
        PFN_vkGetDeviceProcAddr get_device_proc = nullptr;

        SystemVulkan()
        {
            wchar_t directory[MAX_PATH]{};
            const UINT count = GetSystemDirectoryW(directory, MAX_PATH);
            if (!count || count >= MAX_PATH)
                return;
            const std::wstring path = std::wstring(directory) + L"\\vulkan-1.dll";
            module = LoadLibraryExW(path.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module)
                return;
            get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(module, "vkGetInstanceProcAddr"));
            get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                GetProcAddress(module, "vkGetDeviceProcAddr"));
        }
    };

    SystemVulkan& Loader()
    {
        static SystemVulkan loader;
        return loader;
    }

    bool AllowsPrototypeFallback()
    {
        if (!prototype_fallback.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(dispatch_mutex);
        for (const auto& entry : instances) if (!entry.second.prototype) return false;
        for (const auto& entry : devices) if (!entry.second.prototype) return false;
        return instances.size() <= 1 && devices.size() <= 1;
    }

    PFN_vkVoidFunction ResolveGlobal(const char* name)
    {
        auto& loader = Loader();
        if (!loader.module) return nullptr;
        if (loader.get_instance_proc)
            if (const auto function = loader.get_instance_proc(VK_NULL_HANDLE, name)) return function;
        return reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(loader.module, name));
    }

    PFN_vkVoidFunction ResolveInstance(VkInstance instance, const char* name)
    {
        PFN_vkGetInstanceProcAddr next = nullptr;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex);
            const auto found = instances.find(instance);
            if (found != instances.end()) next = found->second.next;
        }
        if (next) return next(instance, name);
        if (!instance) return ResolveGlobal(name);
        if (!AllowsPrototypeFallback()) return nullptr;
        auto& loader = Loader();
        return loader.get_instance_proc ? loader.get_instance_proc(instance, name) : nullptr;
    }

    PFN_vkVoidFunction ResolveDevice(VkDevice device, const char* name)
    {
        PFN_vkGetDeviceProcAddr next = nullptr;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex);
            const auto found = devices.find(device);
            if (found != devices.end()) next = found->second.next;
        }
        if (next) return next(device, name);
        if (!device || !AllowsPrototypeFallback()) return nullptr;
        auto& loader = Loader();
        return loader.get_device_proc ? loader.get_device_proc(device, name) : nullptr;
    }

    VkInstance InstanceOf(VkPhysicalDevice physical)
    {
        std::lock_guard<std::mutex> lock(dispatch_mutex);
        const auto found = physicals.find(physical);
        return found != physicals.end() ? found->second : VK_NULL_HANDLE;
    }

    PFN_vkVoidFunction ResolvePhysical(VkPhysicalDevice physical, const char* name)
    {
        VkInstance instance = InstanceOf(physical);
        if (instance) return ResolveInstance(instance, name);
        if (!AllowsPrototypeFallback()) return nullptr;
        instance = active_instance.load(std::memory_order_acquire);
        return instance ? ResolveInstance(instance, name) : nullptr;
    }

    PFN_vkVoidFunction ResolveQueue(VkQueue queue, const char* name)
    {
        VkDevice device = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex);
            const auto found = queues.find(queue);
            if (found != queues.end()) device = found->second;
        }
        if (!device && AllowsPrototypeFallback()) device = active_device.load(std::memory_order_acquire);
        return device ? ResolveDevice(device, name) : nullptr;
    }

    PFN_vkVoidFunction ResolveCommand(VkCommandBuffer command, const char* name)
    {
        VkDevice device = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex);
            const auto found = commands.find(command);
            if (found != commands.end()) device = found->second.device;
        }
        if (!device && AllowsPrototypeFallback()) device = active_device.load(std::memory_order_acquire);
        return device ? ResolveDevice(device, name) : nullptr;
    }

    void ForgetCommandPool(VkDevice device, VkCommandPool pool)
    {
        std::lock_guard<std::mutex> lock(dispatch_mutex);
        for (auto iterator = commands.begin(); iterator != commands.end();)
            if (iterator->second.device == device && iterator->second.pool == pool) iterator = commands.erase(iterator);
            else ++iterator;
    }

    constexpr uintptr_t LoaderMagicValue = 0x01CDC0DEu;

    void PropagateLoaderDispatchTable(VkDevice device, void* child)
    {
        if (reinterpret_cast<uintptr_t>(device) <= 0x10000u || reinterpret_cast<uintptr_t>(child) <= 0x10000u)
            return;
        if (*reinterpret_cast<const uintptr_t*>(child) == LoaderMagicValue)
            *reinterpret_cast<void**>(child) = *reinterpret_cast<void* const*>(device);
    }

    void RecordCreatedInstance(VkInstance instance)
    {
        auto& loader = Loader();
        std::lock_guard<std::mutex> lock(dispatch_mutex);
        instances[instance] = {loader.get_instance_proc, true};
        active_instance.store(instance, std::memory_order_release);
    }

    void RecordCreatedDevice(VkPhysicalDevice physical, VkDevice device)
    {
        const VkInstance instance = InstanceOf(physical);
        bool prototype = true;
        PFN_vkGetInstanceProcAddr next_instance = nullptr;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex);
            const auto found = instances.find(instance);
            if (found != instances.end())
            {
                prototype = found->second.prototype;
                next_instance = found->second.next;
            }
        }
        PFN_vkGetDeviceProcAddr next_device = prototype ? Loader().get_device_proc : nullptr;
        if (!prototype && next_instance)
            next_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(next_instance(instance, "vkGetDeviceProcAddr"));
        if (!next_device) return;
        std::lock_guard<std::mutex> lock(dispatch_mutex);
        devices[device] = {instance, physical, next_device, prototype};
        if (prototype) active_device.store(device, std::memory_order_release);
    }
}

void AsteriaSetVulkanPrototypeFallback(bool enabled)
{
    prototype_fallback.store(enabled, std::memory_order_release);
}

bool AsteriaRegisterVulkanInstance(VkInstance instance, PFN_vkGetInstanceProcAddr next)
{
    if (!instance || !next || next == &vkGetInstanceProcAddr) return false;
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    instances[instance] = {next, false};
    return true;
}

bool AsteriaRegisterVulkanPhysicalDevice(VkInstance instance, VkPhysicalDevice physical)
{
    if (!instance || !physical) return false;
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    if (instances.find(instance) == instances.end()) return false;
    const auto found = physicals.find(physical);
    if (found != physicals.end() && found->second != instance) return false;
    physicals[physical] = instance;
    return true;
}

bool AsteriaRegisterVulkanDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device, PFN_vkGetDeviceProcAddr next)
{
    if (!instance || !physical || !device || !next || next == &vkGetDeviceProcAddr) return false;
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    const auto owner = physicals.find(physical);
    if (instances.find(instance) == instances.end() || owner == physicals.end() || owner->second != instance) return false;
    devices[device] = {instance, physical, next, false};
    return true;
}

bool AsteriaRegisterVulkanQueue(VkDevice device, VkQueue queue)
{
    if (!device || !queue) return false;
    PropagateLoaderDispatchTable(device, queue);
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    if (devices.find(device) == devices.end()) return false;
    const auto found = queues.find(queue);
    if (found != queues.end() && found->second != device) return false;
    queues[queue] = device;
    return true;
}

bool AsteriaRegisterVulkanCommandBuffer(VkDevice device, VkCommandBuffer command, VkCommandPool pool)
{
    if (!device || !command) return false;
    PropagateLoaderDispatchTable(device, command);
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    if (devices.find(device) == devices.end()) return false;
    const auto found = commands.find(command);
    if (found != commands.end() && found->second.device != device) return false;
    commands[command] = {device, pool};
    return true;
}

void AsteriaUnregisterVulkanCommandBuffer(VkCommandBuffer command)
{
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    commands.erase(command);
}

void AsteriaUnregisterVulkanCommandPool(VkDevice device, VkCommandPool pool)
{
    ForgetCommandPool(device, pool);
}

void AsteriaUnregisterVulkanDevice(VkDevice device)
{
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    devices.erase(device);
    for (auto iterator = queues.begin(); iterator != queues.end();)
        if (iterator->second == device) iterator = queues.erase(iterator);
        else ++iterator;
    for (auto iterator = commands.begin(); iterator != commands.end();)
        if (iterator->second.device == device) iterator = commands.erase(iterator);
        else ++iterator;
    if (active_device.load(std::memory_order_acquire) == device) active_device.store(VK_NULL_HANDLE, std::memory_order_release);
}

void AsteriaUnregisterVulkanInstance(VkInstance instance)
{
    for (;;)
    {
        VkDevice device = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex);
            for (const auto& entry : devices)
                if (entry.second.instance == instance) { device = entry.first; break; }
        }
        if (!device) break;
        AsteriaUnregisterVulkanDevice(device);
    }
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    instances.erase(instance);
    for (auto iterator = physicals.begin(); iterator != physicals.end();)
        if (iterator->second == instance) iterator = physicals.erase(iterator);
        else ++iterator;
    if (active_instance.load(std::memory_order_acquire) == instance) active_instance.store(VK_NULL_HANDLE, std::memory_order_release);
}

PFN_vkVoidFunction AsteriaResolveVulkanInstanceFunction(VkInstance instance, const char* name)
{
    return name ? ResolveInstance(instance, name) : nullptr;
}

PFN_vkVoidFunction AsteriaResolveVulkanDeviceFunction(VkDevice device, const char* name)
{
    return name ? ResolveDevice(device, name) : nullptr;
}

void AsteriaSetVulkanDispatch(VkInstance instance, VkDevice device)
{
    active_instance.store(instance, std::memory_order_release);
    active_device.store(device, std::memory_order_release);
    prototype_fallback.store(true, std::memory_order_release);
}

PFN_vkVoidFunction AsteriaResolveVulkanFunction(const char* name)
{
    if (!name || !AllowsPrototypeFallback()) return nullptr;
    const VkDevice device = active_device.load(std::memory_order_acquire);
    if (device) if (const auto function = ResolveDevice(device, name)) return function;
    const VkInstance instance = active_instance.load(std::memory_order_acquire);
    if (instance) if (const auto function = ResolveInstance(instance, name)) return function;
    return ResolveGlobal(name);
}

AsteriaNativePresentStats AsteriaGetNativePresentStats()
{
    return {native_present_attempts.load(std::memory_order_relaxed),
            native_present_successful_calls.load(std::memory_order_relaxed),
            native_present_failed_calls.load(std::memory_order_relaxed),
            native_present_successful_swapchains.load(std::memory_order_relaxed)};
}

void AsteriaResetNativePresentStats()
{
    native_present_attempts.store(0, std::memory_order_relaxed);
    native_present_successful_calls.store(0, std::memory_order_relaxed);
    native_present_failed_calls.store(0, std::memory_order_relaxed);
    native_present_successful_swapchains.store(0, std::memory_order_relaxed);
}

void AsteriaRegisterSwapchainImagesReplacement(AsteriaSwapchainImagesReplacementFn replacement,
                                               AsteriaSwapchainTokenPredicateFn predicate)
{
    swapchain_images_replacement.store(replacement, std::memory_order_release);
    swapchain_token_predicate.store(predicate, std::memory_order_release);
}

'''


def main():
    core = (HEADERS / "vulkan_core.h").read_text()
    win32 = (HEADERS / "vulkan_win32.h").read_text()
    pattern = re.compile(r"VKAPI_ATTR\s+(\w+)\s+VKAPI_CALL\s+(vk\w+)\(\s*(.*?)\);", re.S)
    prototypes = {name: (result, parameters) for result, name, parameters in pattern.findall(core + win32)}
    core10 = set(name for _, name, _ in pattern.findall(core.split("#define VK_VERSION_1_1")[0]))
    calls = set()
    files = list((ROOT / "sources").rglob("*.cpp"))
    files += list((RESEARCH / "sdk-v1.1.4" / "sdk" / "src" / "backends" / "vk").rglob("*.cpp"))
    for source in files:
        if source.name == "VulkanShim.cpp":
            continue
        calls.update(re.findall(r"(?<![.\w>])\b(vk[A-Z]\w*)\s*\(", source.read_text()))
    requested = core10 | calls | {"vkCreateWin32SurfaceKHR", "vkDestroySurfaceKHR",
                                   "vkGetPhysicalDeviceWin32PresentationSupportKHR",
                                   "vkEnumerateInstanceVersion", "vkGetSemaphoreCounterValue",
                                   "vkGetDeviceQueue2", "vkEnumeratePhysicalDeviceGroups",
                                   "vkEnumeratePhysicalDeviceGroupsKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
                                   "vkGetPhysicalDeviceSurfaceFormatsKHR", "vkGetPhysicalDeviceSurfacePresentModesKHR"}
    text = PREFIX
    emitted = []
    for name in sorted(requested & prototypes.keys()):
        result, parameters = prototypes[name]
        args = []
        if parameters.strip() != "void":
            for parameter in parameters.split(","):
                match = re.search(r"(\w+)\s*(?:\[[^]]*\])?\s*$", parameter)
                if not match:
                    raise ValueError((name, parameter))
                args.append(match[1])
        first = parameters.split(",", 1)[0].strip().split(" ", 1)[0]
        resolver = {"VkDevice": "ResolveDevice", "VkQueue": "ResolveQueue",
                    "VkCommandBuffer": "ResolveCommand", "VkInstance": "ResolveInstance",
                    "VkPhysicalDevice": "ResolvePhysical"}.get(first)
        lookup = f'{resolver}({args[0]}, "{name}")' if resolver else f'ResolveGlobal("{name}")'
        text += f'extern "C" VKAPI_ATTR {result} VKAPI_CALL {name}(\n    {parameters})\n{{\n'
        if name in ("vkGetInstanceProcAddr", "vkGetDeviceProcAddr"):
            method = "ResolveInstance" if name == "vkGetInstanceProcAddr" else "ResolveDevice"
            text += f'    return pName ? {method}({args[0]}, pName) : nullptr;\n}}\n\n'
            emitted.append(name)
            continue
        if name == "vkGetSwapchainImagesKHR":
            text += "    const auto predicate = swapchain_token_predicate.load(std::memory_order_acquire);\n"
            text += "    const auto replacement = swapchain_images_replacement.load(std::memory_order_acquire);\n"
            text += "    if (predicate && replacement && predicate(swapchain))\n"
            text += "        return replacement(device, swapchain, pSwapchainImageCount, pSwapchainImages);\n"
        text += f"    const auto function = reinterpret_cast<PFN_{name}>({lookup});\n"
        if result == "void":
            text += f'    if (!function || function == &{name})\n        throw std::runtime_error("Unavailable or recursive Vulkan function: {name}");\n'
        elif result == "VkResult":
            text += f"    if (!function || function == &{name})\n        return VK_ERROR_EXTENSION_NOT_PRESENT;\n"
        else:
            text += f"    if (!function || function == &{name})\n        return {{}};\n"
        call = f"function({', '.join(args)})"
        if name in ("vkCreateInstance", "vkCreateDevice"):
            text += f"    const VkResult result = {call};\n    if (result == VK_SUCCESS)\n"
            output = "pInstance" if name == "vkCreateInstance" else "pDevice"
            record = f"RecordCreatedInstance(*{output})" if name == "vkCreateInstance" else f"RecordCreatedDevice(physicalDevice, *{output})"
            text += f"        {record};\n    return result;\n"
        elif name in ("vkDestroyInstance", "vkDestroyDevice"):
            arg = "instance" if name == "vkDestroyInstance" else "device"
            forget = "AsteriaUnregisterVulkanInstance" if name == "vkDestroyInstance" else "AsteriaUnregisterVulkanDevice"
            text += f"    {forget}({arg});\n"
            text += f"    {call};\n"
        elif name == "vkEnumeratePhysicalDevices":
            text += f"    const VkResult result = {call};\n"
            text += "    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && pPhysicalDevices && pPhysicalDeviceCount)\n"
            text += "        for (uint32_t index = 0; index < *pPhysicalDeviceCount; ++index)\n"
            text += "            AsteriaRegisterVulkanPhysicalDevice(instance, pPhysicalDevices[index]);\n    return result;\n"
        elif name in ("vkEnumeratePhysicalDeviceGroups", "vkEnumeratePhysicalDeviceGroupsKHR"):
            text += f"    const VkResult result = {call};\n"
            text += "    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && pPhysicalDeviceGroupProperties && pPhysicalDeviceGroupCount)\n"
            text += "        for (uint32_t group = 0; group < *pPhysicalDeviceGroupCount; ++group)\n"
            text += "            for (uint32_t index = 0; index < pPhysicalDeviceGroupProperties[group].physicalDeviceCount; ++index)\n"
            text += "                AsteriaRegisterVulkanPhysicalDevice(instance, pPhysicalDeviceGroupProperties[group].physicalDevices[index]);\n    return result;\n"
        elif name in ("vkGetDeviceQueue", "vkGetDeviceQueue2"):
            text += f"    {call};\n    if (pQueue && *pQueue) AsteriaRegisterVulkanQueue(device, *pQueue);\n"
        elif name == "vkAllocateCommandBuffers":
            text += f"    const VkResult result = {call};\n"
            text += "    if (result == VK_SUCCESS && pAllocateInfo && pCommandBuffers)\n"
            text += "        for (uint32_t index = 0; index < pAllocateInfo->commandBufferCount; ++index)\n"
            text += "            AsteriaRegisterVulkanCommandBuffer(device, pCommandBuffers[index], pAllocateInfo->commandPool);\n    return result;\n"
        elif name == "vkFreeCommandBuffers":
            text += "    if (pCommandBuffers) for (uint32_t index = 0; index < commandBufferCount; ++index)\n"
            text += "        AsteriaUnregisterVulkanCommandBuffer(pCommandBuffers[index]);\n"
            text += f"    {call};\n"
        elif name == "vkDestroyCommandPool":
            text += f"    ForgetCommandPool(device, commandPool);\n    {call};\n"
        elif name == "vkQueuePresentKHR":
            text += '''    native_present_attempts.fetch_add(1, std::memory_order_relaxed);
    const VkResult result = function(queue, pPresentInfo);
    const bool successful = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
    if (successful)
        native_present_successful_calls.fetch_add(1, std::memory_order_relaxed);
    else
        native_present_failed_calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t successful_swapchains = 0;
    if (pPresentInfo)
    {
        if (pPresentInfo->pResults)
        {
            for (uint32_t index = 0; index < pPresentInfo->swapchainCount; ++index)
                if (pPresentInfo->pResults[index] == VK_SUCCESS || pPresentInfo->pResults[index] == VK_SUBOPTIMAL_KHR)
                    ++successful_swapchains;
        }
        else if (successful)
            successful_swapchains = pPresentInfo->swapchainCount;
    }
    native_present_successful_swapchains.fetch_add(successful_swapchains, std::memory_order_relaxed);
    return result;
'''
        else:
            text += f"    {'return ' if result != 'void' else ''}{call};\n"
        text += "}\n\n"
        emitted.append(name)
    target = ROOT / "sources" / "VulkanShim.cpp"
    target.write_text(text, encoding="utf-8")
    manifest = {"vulkan_headers": "v1.3.250", "scope": "registered-next-layer-dispatch-with-isolated-native-prototype-fallback",
                "forwarder_count": len(emitted), "functions": emitted,
                "vulkan_core_sha256": hashlib.sha256((HEADERS / "vulkan_core.h").read_bytes()).hexdigest(),
                "vulkan_win32_sha256": hashlib.sha256((HEADERS / "vulkan_win32.h").read_bytes()).hexdigest(),
                "shim_sha256": hashlib.sha256(target.read_bytes()).hexdigest()}
    (ROOT / "Shaders" / "vulkan-shim-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Generated {len(emitted)} standard Vulkan forwarders")


if __name__ == "__main__":
    main()
