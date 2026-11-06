#include "AsteriaVulkanShim.h"
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

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    uint64_t                                    timeout,
    VkSemaphore                                 semaphore,
    VkFence                                     fence,
    uint32_t*                                   pImageIndex)
{
    const auto function = reinterpret_cast<PFN_vkAcquireNextImageKHR>(ResolveDevice(device, "vkAcquireNextImageKHR"));
    if (!function || function == &vkAcquireNextImageKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice                                    device,
    const VkCommandBufferAllocateInfo*          pAllocateInfo,
    VkCommandBuffer*                            pCommandBuffers)
{
    const auto function = reinterpret_cast<PFN_vkAllocateCommandBuffers>(ResolveDevice(device, "vkAllocateCommandBuffers"));
    if (!function || function == &vkAllocateCommandBuffers)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = function(device, pAllocateInfo, pCommandBuffers);
    if (result == VK_SUCCESS && pAllocateInfo && pCommandBuffers)
        for (uint32_t index = 0; index < pAllocateInfo->commandBufferCount; ++index)
            AsteriaRegisterVulkanCommandBuffer(device, pCommandBuffers[index], pAllocateInfo->commandPool);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(
    VkDevice                                    device,
    const VkDescriptorSetAllocateInfo*          pAllocateInfo,
    VkDescriptorSet*                            pDescriptorSets)
{
    const auto function = reinterpret_cast<PFN_vkAllocateDescriptorSets>(ResolveDevice(device, "vkAllocateDescriptorSets"));
    if (!function || function == &vkAllocateDescriptorSets)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pAllocateInfo, pDescriptorSets);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
    VkDevice                                    device,
    const VkMemoryAllocateInfo*                 pAllocateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDeviceMemory*                             pMemory)
{
    const auto function = reinterpret_cast<PFN_vkAllocateMemory>(ResolveDevice(device, "vkAllocateMemory"));
    if (!function || function == &vkAllocateMemory)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pAllocateInfo, pAllocator, pMemory);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer                             commandBuffer,
    const VkCommandBufferBeginInfo*             pBeginInfo)
{
    const auto function = reinterpret_cast<PFN_vkBeginCommandBuffer>(ResolveCommand(commandBuffer, "vkBeginCommandBuffer"));
    if (!function || function == &vkBeginCommandBuffer)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(commandBuffer, pBeginInfo);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(
    VkDevice                                    device,
    VkBuffer                                    buffer,
    VkDeviceMemory                              memory,
    VkDeviceSize                                memoryOffset)
{
    const auto function = reinterpret_cast<PFN_vkBindBufferMemory>(ResolveDevice(device, "vkBindBufferMemory"));
    if (!function || function == &vkBindBufferMemory)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, buffer, memory, memoryOffset);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(
    VkDevice                                    device,
    VkImage                                     image,
    VkDeviceMemory                              memory,
    VkDeviceSize                                memoryOffset)
{
    const auto function = reinterpret_cast<PFN_vkBindImageMemory>(ResolveDevice(device, "vkBindImageMemory"));
    if (!function || function == &vkBindImageMemory)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, image, memory, memoryOffset);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    query,
    VkQueryControlFlags                         flags)
{
    const auto function = reinterpret_cast<PFN_vkCmdBeginQuery>(ResolveCommand(commandBuffer, "vkCmdBeginQuery"));
    if (!function || function == &vkCmdBeginQuery)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdBeginQuery");
    function(commandBuffer, queryPool, query, flags);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(
    VkCommandBuffer                             commandBuffer,
    const VkRenderPassBeginInfo*                pRenderPassBegin,
    VkSubpassContents                           contents)
{
    const auto function = reinterpret_cast<PFN_vkCmdBeginRenderPass>(ResolveCommand(commandBuffer, "vkCmdBeginRenderPass"));
    if (!function || function == &vkCmdBeginRenderPass)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdBeginRenderPass");
    function(commandBuffer, pRenderPassBegin, contents);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(
    VkCommandBuffer                             commandBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipelineLayout                            layout,
    uint32_t                                    firstSet,
    uint32_t                                    descriptorSetCount,
    const VkDescriptorSet*                      pDescriptorSets,
    uint32_t                                    dynamicOffsetCount,
    const uint32_t*                             pDynamicOffsets)
{
    const auto function = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(ResolveCommand(commandBuffer, "vkCmdBindDescriptorSets"));
    if (!function || function == &vkCmdBindDescriptorSets)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdBindDescriptorSets");
    function(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    buffer,
    VkDeviceSize                                offset,
    VkIndexType                                 indexType)
{
    const auto function = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(ResolveCommand(commandBuffer, "vkCmdBindIndexBuffer"));
    if (!function || function == &vkCmdBindIndexBuffer)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdBindIndexBuffer");
    function(commandBuffer, buffer, offset, indexType);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(
    VkCommandBuffer                             commandBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipeline                                  pipeline)
{
    const auto function = reinterpret_cast<PFN_vkCmdBindPipeline>(ResolveCommand(commandBuffer, "vkCmdBindPipeline"));
    if (!function || function == &vkCmdBindPipeline)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdBindPipeline");
    function(commandBuffer, pipelineBindPoint, pipeline);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    firstBinding,
    uint32_t                                    bindingCount,
    const VkBuffer*                             pBuffers,
    const VkDeviceSize*                         pOffsets)
{
    const auto function = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(ResolveCommand(commandBuffer, "vkCmdBindVertexBuffers"));
    if (!function || function == &vkCmdBindVertexBuffers)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdBindVertexBuffers");
    function(commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageBlit*                          pRegions,
    VkFilter                                    filter)
{
    const auto function = reinterpret_cast<PFN_vkCmdBlitImage>(ResolveCommand(commandBuffer, "vkCmdBlitImage"));
    if (!function || function == &vkCmdBlitImage)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdBlitImage");
    function(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    attachmentCount,
    const VkClearAttachment*                    pAttachments,
    uint32_t                                    rectCount,
    const VkClearRect*                          pRects)
{
    const auto function = reinterpret_cast<PFN_vkCmdClearAttachments>(ResolveCommand(commandBuffer, "vkCmdClearAttachments"));
    if (!function || function == &vkCmdClearAttachments)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdClearAttachments");
    function(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     image,
    VkImageLayout                               imageLayout,
    const VkClearColorValue*                    pColor,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
    const auto function = reinterpret_cast<PFN_vkCmdClearColorImage>(ResolveCommand(commandBuffer, "vkCmdClearColorImage"));
    if (!function || function == &vkCmdClearColorImage)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdClearColorImage");
    function(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     image,
    VkImageLayout                               imageLayout,
    const VkClearDepthStencilValue*             pDepthStencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
    const auto function = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(ResolveCommand(commandBuffer, "vkCmdClearDepthStencilImage"));
    if (!function || function == &vkCmdClearDepthStencilImage)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdClearDepthStencilImage");
    function(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    dstBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
    const auto function = reinterpret_cast<PFN_vkCmdCopyBuffer>(ResolveCommand(commandBuffer, "vkCmdCopyBuffer"));
    if (!function || function == &vkCmdCopyBuffer)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdCopyBuffer");
    function(commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
    const auto function = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(ResolveCommand(commandBuffer, "vkCmdCopyBufferToImage"));
    if (!function || function == &vkCmdCopyBufferToImage)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdCopyBufferToImage");
    function(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{
    const auto function = reinterpret_cast<PFN_vkCmdCopyImage>(ResolveCommand(commandBuffer, "vkCmdCopyImage"));
    if (!function || function == &vkCmdCopyImage)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdCopyImage");
    function(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    dstBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
    const auto function = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(ResolveCommand(commandBuffer, "vkCmdCopyImageToBuffer"));
    if (!function || function == &vkCmdCopyImageToBuffer)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdCopyImageToBuffer");
    function(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    firstQuery,
    uint32_t                                    queryCount,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                stride,
    VkQueryResultFlags                          flags)
{
    const auto function = reinterpret_cast<PFN_vkCmdCopyQueryPoolResults>(ResolveCommand(commandBuffer, "vkCmdCopyQueryPoolResults"));
    if (!function || function == &vkCmdCopyQueryPoolResults)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdCopyQueryPoolResults");
    function(commandBuffer, queryPool, firstQuery, queryCount, dstBuffer, dstOffset, stride, flags);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    groupCountX,
    uint32_t                                    groupCountY,
    uint32_t                                    groupCountZ)
{
    const auto function = reinterpret_cast<PFN_vkCmdDispatch>(ResolveCommand(commandBuffer, "vkCmdDispatch"));
    if (!function || function == &vkCmdDispatch)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdDispatch");
    function(commandBuffer, groupCountX, groupCountY, groupCountZ);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    buffer,
    VkDeviceSize                                offset)
{
    const auto function = reinterpret_cast<PFN_vkCmdDispatchIndirect>(ResolveCommand(commandBuffer, "vkCmdDispatchIndirect"));
    if (!function || function == &vkCmdDispatchIndirect)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdDispatchIndirect");
    function(commandBuffer, buffer, offset);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDraw(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    vertexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstVertex,
    uint32_t                                    firstInstance)
{
    const auto function = reinterpret_cast<PFN_vkCmdDraw>(ResolveCommand(commandBuffer, "vkCmdDraw"));
    if (!function || function == &vkCmdDraw)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdDraw");
    function(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    indexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstIndex,
    int32_t                                     vertexOffset,
    uint32_t                                    firstInstance)
{
    const auto function = reinterpret_cast<PFN_vkCmdDrawIndexed>(ResolveCommand(commandBuffer, "vkCmdDrawIndexed"));
    if (!function || function == &vkCmdDrawIndexed)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdDrawIndexed");
    function(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    buffer,
    VkDeviceSize                                offset,
    uint32_t                                    drawCount,
    uint32_t                                    stride)
{
    const auto function = reinterpret_cast<PFN_vkCmdDrawIndexedIndirect>(ResolveCommand(commandBuffer, "vkCmdDrawIndexedIndirect"));
    if (!function || function == &vkCmdDrawIndexedIndirect)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdDrawIndexedIndirect");
    function(commandBuffer, buffer, offset, drawCount, stride);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    buffer,
    VkDeviceSize                                offset,
    uint32_t                                    drawCount,
    uint32_t                                    stride)
{
    const auto function = reinterpret_cast<PFN_vkCmdDrawIndirect>(ResolveCommand(commandBuffer, "vkCmdDrawIndirect"));
    if (!function || function == &vkCmdDrawIndirect)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdDrawIndirect");
    function(commandBuffer, buffer, offset, drawCount, stride);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    query)
{
    const auto function = reinterpret_cast<PFN_vkCmdEndQuery>(ResolveCommand(commandBuffer, "vkCmdEndQuery"));
    if (!function || function == &vkCmdEndQuery)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdEndQuery");
    function(commandBuffer, queryPool, query);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(
    VkCommandBuffer                             commandBuffer)
{
    const auto function = reinterpret_cast<PFN_vkCmdEndRenderPass>(ResolveCommand(commandBuffer, "vkCmdEndRenderPass"));
    if (!function || function == &vkCmdEndRenderPass)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdEndRenderPass");
    function(commandBuffer);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    commandBufferCount,
    const VkCommandBuffer*                      pCommandBuffers)
{
    const auto function = reinterpret_cast<PFN_vkCmdExecuteCommands>(ResolveCommand(commandBuffer, "vkCmdExecuteCommands"));
    if (!function || function == &vkCmdExecuteCommands)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdExecuteCommands");
    function(commandBuffer, commandBufferCount, pCommandBuffers);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                size,
    uint32_t                                    data)
{
    const auto function = reinterpret_cast<PFN_vkCmdFillBuffer>(ResolveCommand(commandBuffer, "vkCmdFillBuffer"));
    if (!function || function == &vkCmdFillBuffer)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdFillBuffer");
    function(commandBuffer, dstBuffer, dstOffset, size, data);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(
    VkCommandBuffer                             commandBuffer,
    VkSubpassContents                           contents)
{
    const auto function = reinterpret_cast<PFN_vkCmdNextSubpass>(ResolveCommand(commandBuffer, "vkCmdNextSubpass"));
    if (!function || function == &vkCmdNextSubpass)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdNextSubpass");
    function(commandBuffer, contents);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer                             commandBuffer,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        dstStageMask,
    VkDependencyFlags                           dependencyFlags,
    uint32_t                                    memoryBarrierCount,
    const VkMemoryBarrier*                      pMemoryBarriers,
    uint32_t                                    bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
    uint32_t                                    imageMemoryBarrierCount,
    const VkImageMemoryBarrier*                 pImageMemoryBarriers)
{
    const auto function = reinterpret_cast<PFN_vkCmdPipelineBarrier>(ResolveCommand(commandBuffer, "vkCmdPipelineBarrier"));
    if (!function || function == &vkCmdPipelineBarrier)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdPipelineBarrier");
    function(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(
    VkCommandBuffer                             commandBuffer,
    VkPipelineLayout                            layout,
    VkShaderStageFlags                          stageFlags,
    uint32_t                                    offset,
    uint32_t                                    size,
    const void*                                 pValues)
{
    const auto function = reinterpret_cast<PFN_vkCmdPushConstants>(ResolveCommand(commandBuffer, "vkCmdPushConstants"));
    if (!function || function == &vkCmdPushConstants)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdPushConstants");
    function(commandBuffer, layout, stageFlags, offset, size, pValues);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(
    VkCommandBuffer                             commandBuffer,
    VkEvent                                     event,
    VkPipelineStageFlags                        stageMask)
{
    const auto function = reinterpret_cast<PFN_vkCmdResetEvent>(ResolveCommand(commandBuffer, "vkCmdResetEvent"));
    if (!function || function == &vkCmdResetEvent)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdResetEvent");
    function(commandBuffer, event, stageMask);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(
    VkCommandBuffer                             commandBuffer,
    VkQueryPool                                 queryPool,
    uint32_t                                    firstQuery,
    uint32_t                                    queryCount)
{
    const auto function = reinterpret_cast<PFN_vkCmdResetQueryPool>(ResolveCommand(commandBuffer, "vkCmdResetQueryPool"));
    if (!function || function == &vkCmdResetQueryPool)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdResetQueryPool");
    function(commandBuffer, queryPool, firstQuery, queryCount);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(
    VkCommandBuffer                             commandBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     dstImage,
    VkImageLayout                               dstImageLayout,
    uint32_t                                    regionCount,
    const VkImageResolve*                       pRegions)
{
    const auto function = reinterpret_cast<PFN_vkCmdResolveImage>(ResolveCommand(commandBuffer, "vkCmdResolveImage"));
    if (!function || function == &vkCmdResolveImage)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdResolveImage");
    function(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(
    VkCommandBuffer                             commandBuffer,
    const float                                 blendConstants[4])
{
    const auto function = reinterpret_cast<PFN_vkCmdSetBlendConstants>(ResolveCommand(commandBuffer, "vkCmdSetBlendConstants"));
    if (!function || function == &vkCmdSetBlendConstants)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetBlendConstants");
    function(commandBuffer, blendConstants);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(
    VkCommandBuffer                             commandBuffer,
    float                                       depthBiasConstantFactor,
    float                                       depthBiasClamp,
    float                                       depthBiasSlopeFactor)
{
    const auto function = reinterpret_cast<PFN_vkCmdSetDepthBias>(ResolveCommand(commandBuffer, "vkCmdSetDepthBias"));
    if (!function || function == &vkCmdSetDepthBias)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetDepthBias");
    function(commandBuffer, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(
    VkCommandBuffer                             commandBuffer,
    float                                       minDepthBounds,
    float                                       maxDepthBounds)
{
    const auto function = reinterpret_cast<PFN_vkCmdSetDepthBounds>(ResolveCommand(commandBuffer, "vkCmdSetDepthBounds"));
    if (!function || function == &vkCmdSetDepthBounds)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetDepthBounds");
    function(commandBuffer, minDepthBounds, maxDepthBounds);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(
    VkCommandBuffer                             commandBuffer,
    VkEvent                                     event,
    VkPipelineStageFlags                        stageMask)
{
    const auto function = reinterpret_cast<PFN_vkCmdSetEvent>(ResolveCommand(commandBuffer, "vkCmdSetEvent"));
    if (!function || function == &vkCmdSetEvent)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetEvent");
    function(commandBuffer, event, stageMask);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(
    VkCommandBuffer                             commandBuffer,
    float                                       lineWidth)
{
    const auto function = reinterpret_cast<PFN_vkCmdSetLineWidth>(ResolveCommand(commandBuffer, "vkCmdSetLineWidth"));
    if (!function || function == &vkCmdSetLineWidth)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetLineWidth");
    function(commandBuffer, lineWidth);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    firstScissor,
    uint32_t                                    scissorCount,
    const VkRect2D*                             pScissors)
{
    const auto function = reinterpret_cast<PFN_vkCmdSetScissor>(ResolveCommand(commandBuffer, "vkCmdSetScissor"));
    if (!function || function == &vkCmdSetScissor)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetScissor");
    function(commandBuffer, firstScissor, scissorCount, pScissors);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(
    VkCommandBuffer                             commandBuffer,
    VkStencilFaceFlags                          faceMask,
    uint32_t                                    compareMask)
{
    const auto function = reinterpret_cast<PFN_vkCmdSetStencilCompareMask>(ResolveCommand(commandBuffer, "vkCmdSetStencilCompareMask"));
    if (!function || function == &vkCmdSetStencilCompareMask)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetStencilCompareMask");
    function(commandBuffer, faceMask, compareMask);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(
    VkCommandBuffer                             commandBuffer,
    VkStencilFaceFlags                          faceMask,
    uint32_t                                    reference)
{
    const auto function = reinterpret_cast<PFN_vkCmdSetStencilReference>(ResolveCommand(commandBuffer, "vkCmdSetStencilReference"));
    if (!function || function == &vkCmdSetStencilReference)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetStencilReference");
    function(commandBuffer, faceMask, reference);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(
    VkCommandBuffer                             commandBuffer,
    VkStencilFaceFlags                          faceMask,
    uint32_t                                    writeMask)
{
    const auto function = reinterpret_cast<PFN_vkCmdSetStencilWriteMask>(ResolveCommand(commandBuffer, "vkCmdSetStencilWriteMask"));
    if (!function || function == &vkCmdSetStencilWriteMask)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetStencilWriteMask");
    function(commandBuffer, faceMask, writeMask);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    firstViewport,
    uint32_t                                    viewportCount,
    const VkViewport*                           pViewports)
{
    const auto function = reinterpret_cast<PFN_vkCmdSetViewport>(ResolveCommand(commandBuffer, "vkCmdSetViewport"));
    if (!function || function == &vkCmdSetViewport)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdSetViewport");
    function(commandBuffer, firstViewport, viewportCount, pViewports);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    VkDeviceSize                                dataSize,
    const void*                                 pData)
{
    const auto function = reinterpret_cast<PFN_vkCmdUpdateBuffer>(ResolveCommand(commandBuffer, "vkCmdUpdateBuffer"));
    if (!function || function == &vkCmdUpdateBuffer)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdUpdateBuffer");
    function(commandBuffer, dstBuffer, dstOffset, dataSize, pData);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    eventCount,
    const VkEvent*                              pEvents,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        dstStageMask,
    uint32_t                                    memoryBarrierCount,
    const VkMemoryBarrier*                      pMemoryBarriers,
    uint32_t                                    bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
    uint32_t                                    imageMemoryBarrierCount,
    const VkImageMemoryBarrier*                 pImageMemoryBarriers)
{
    const auto function = reinterpret_cast<PFN_vkCmdWaitEvents>(ResolveCommand(commandBuffer, "vkCmdWaitEvents"));
    if (!function || function == &vkCmdWaitEvents)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdWaitEvents");
    function(commandBuffer, eventCount, pEvents, srcStageMask, dstStageMask, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdWriteBufferMarker2AMD(
    VkCommandBuffer                             commandBuffer,
    VkPipelineStageFlags2                       stage,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    uint32_t                                    marker)
{
    const auto function = reinterpret_cast<PFN_vkCmdWriteBufferMarker2AMD>(ResolveCommand(commandBuffer, "vkCmdWriteBufferMarker2AMD"));
    if (!function || function == &vkCmdWriteBufferMarker2AMD)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdWriteBufferMarker2AMD");
    function(commandBuffer, stage, dstBuffer, dstOffset, marker);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdWriteBufferMarkerAMD(
    VkCommandBuffer                             commandBuffer,
    VkPipelineStageFlagBits                     pipelineStage,
    VkBuffer                                    dstBuffer,
    VkDeviceSize                                dstOffset,
    uint32_t                                    marker)
{
    const auto function = reinterpret_cast<PFN_vkCmdWriteBufferMarkerAMD>(ResolveCommand(commandBuffer, "vkCmdWriteBufferMarkerAMD"));
    if (!function || function == &vkCmdWriteBufferMarkerAMD)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdWriteBufferMarkerAMD");
    function(commandBuffer, pipelineStage, dstBuffer, dstOffset, marker);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(
    VkCommandBuffer                             commandBuffer,
    VkPipelineStageFlagBits                     pipelineStage,
    VkQueryPool                                 queryPool,
    uint32_t                                    query)
{
    const auto function = reinterpret_cast<PFN_vkCmdWriteTimestamp>(ResolveCommand(commandBuffer, "vkCmdWriteTimestamp"));
    if (!function || function == &vkCmdWriteTimestamp)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkCmdWriteTimestamp");
    function(commandBuffer, pipelineStage, queryPool, query);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(
    VkDevice                                    device,
    const VkBufferCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkBuffer*                                   pBuffer)
{
    const auto function = reinterpret_cast<PFN_vkCreateBuffer>(ResolveDevice(device, "vkCreateBuffer"));
    if (!function || function == &vkCreateBuffer)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pBuffer);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView(
    VkDevice                                    device,
    const VkBufferViewCreateInfo*               pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkBufferView*                               pView)
{
    const auto function = reinterpret_cast<PFN_vkCreateBufferView>(ResolveDevice(device, "vkCreateBufferView"));
    if (!function || function == &vkCreateBufferView)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pView);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
    VkDevice                                    device,
    const VkCommandPoolCreateInfo*              pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkCommandPool*                              pCommandPool)
{
    const auto function = reinterpret_cast<PFN_vkCreateCommandPool>(ResolveDevice(device, "vkCreateCommandPool"));
    if (!function || function == &vkCreateCommandPool)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pCommandPool);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkComputePipelineCreateInfo*          pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines)
{
    const auto function = reinterpret_cast<PFN_vkCreateComputePipelines>(ResolveDevice(device, "vkCreateComputePipelines"));
    if (!function || function == &vkCreateComputePipelines)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(
    VkDevice                                    device,
    const VkDescriptorPoolCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorPool*                           pDescriptorPool)
{
    const auto function = reinterpret_cast<PFN_vkCreateDescriptorPool>(ResolveDevice(device, "vkCreateDescriptorPool"));
    if (!function || function == &vkCreateDescriptorPool)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pDescriptorPool);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(
    VkDevice                                    device,
    const VkDescriptorSetLayoutCreateInfo*      pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDescriptorSetLayout*                      pSetLayout)
{
    const auto function = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(ResolveDevice(device, "vkCreateDescriptorSetLayout"));
    if (!function || function == &vkCreateDescriptorSetLayout)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pSetLayout);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{
    const auto function = reinterpret_cast<PFN_vkCreateDevice>(ResolvePhysical(physicalDevice, "vkCreateDevice"));
    if (!function || function == &vkCreateDevice)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = function(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result == VK_SUCCESS)
        RecordCreatedDevice(physicalDevice, *pDevice);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(
    VkDevice                                    device,
    const VkEventCreateInfo*                    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkEvent*                                    pEvent)
{
    const auto function = reinterpret_cast<PFN_vkCreateEvent>(ResolveDevice(device, "vkCreateEvent"));
    if (!function || function == &vkCreateEvent)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pEvent);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(
    VkDevice                                    device,
    const VkFenceCreateInfo*                    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkFence*                                    pFence)
{
    const auto function = reinterpret_cast<PFN_vkCreateFence>(ResolveDevice(device, "vkCreateFence"));
    if (!function || function == &vkCreateFence)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pFence);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(
    VkDevice                                    device,
    const VkFramebufferCreateInfo*              pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkFramebuffer*                              pFramebuffer)
{
    const auto function = reinterpret_cast<PFN_vkCreateFramebuffer>(ResolveDevice(device, "vkCreateFramebuffer"));
    if (!function || function == &vkCreateFramebuffer)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pFramebuffer);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkGraphicsPipelineCreateInfo*         pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines)
{
    const auto function = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(ResolveDevice(device, "vkCreateGraphicsPipelines"));
    if (!function || function == &vkCreateGraphicsPipelines)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice                                    device,
    const VkImageCreateInfo*                    pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkImage*                                    pImage)
{
    const auto function = reinterpret_cast<PFN_vkCreateImage>(ResolveDevice(device, "vkCreateImage"));
    if (!function || function == &vkCreateImage)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pImage);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice                                    device,
    const VkImageViewCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkImageView*                                pView)
{
    const auto function = reinterpret_cast<PFN_vkCreateImageView>(ResolveDevice(device, "vkCreateImageView"));
    if (!function || function == &vkCreateImageView)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pView);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
    const auto function = reinterpret_cast<PFN_vkCreateInstance>(ResolveGlobal("vkCreateInstance"));
    if (!function || function == &vkCreateInstance)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = function(pCreateInfo, pAllocator, pInstance);
    if (result == VK_SUCCESS)
        RecordCreatedInstance(*pInstance);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(
    VkDevice                                    device,
    const VkPipelineCacheCreateInfo*            pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipelineCache*                            pPipelineCache)
{
    const auto function = reinterpret_cast<PFN_vkCreatePipelineCache>(ResolveDevice(device, "vkCreatePipelineCache"));
    if (!function || function == &vkCreatePipelineCache)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pPipelineCache);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(
    VkDevice                                    device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipelineLayout*                           pPipelineLayout)
{
    const auto function = reinterpret_cast<PFN_vkCreatePipelineLayout>(ResolveDevice(device, "vkCreatePipelineLayout"));
    if (!function || function == &vkCreatePipelineLayout)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pPipelineLayout);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(
    VkDevice                                    device,
    const VkQueryPoolCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkQueryPool*                                pQueryPool)
{
    const auto function = reinterpret_cast<PFN_vkCreateQueryPool>(ResolveDevice(device, "vkCreateQueryPool"));
    if (!function || function == &vkCreateQueryPool)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pQueryPool);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(
    VkDevice                                    device,
    const VkRenderPassCreateInfo*               pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkRenderPass*                               pRenderPass)
{
    const auto function = reinterpret_cast<PFN_vkCreateRenderPass>(ResolveDevice(device, "vkCreateRenderPass"));
    if (!function || function == &vkCreateRenderPass)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pRenderPass);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(
    VkDevice                                    device,
    const VkSamplerCreateInfo*                  pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSampler*                                  pSampler)
{
    const auto function = reinterpret_cast<PFN_vkCreateSampler>(ResolveDevice(device, "vkCreateSampler"));
    if (!function || function == &vkCreateSampler)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pSampler);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(
    VkDevice                                    device,
    const VkSemaphoreCreateInfo*                pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSemaphore*                                pSemaphore)
{
    const auto function = reinterpret_cast<PFN_vkCreateSemaphore>(ResolveDevice(device, "vkCreateSemaphore"));
    if (!function || function == &vkCreateSemaphore)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pSemaphore);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(
    VkDevice                                    device,
    const VkShaderModuleCreateInfo*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkShaderModule*                             pShaderModule)
{
    const auto function = reinterpret_cast<PFN_vkCreateShaderModule>(ResolveDevice(device, "vkCreateShaderModule"));
    if (!function || function == &vkCreateShaderModule)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pShaderModule);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice                                    device,
    const VkSwapchainCreateInfoKHR*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSwapchainKHR*                             pSwapchain)
{
    const auto function = reinterpret_cast<PFN_vkCreateSwapchainKHR>(ResolveDevice(device, "vkCreateSwapchainKHR"));
    if (!function || function == &vkCreateSwapchainKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pCreateInfo, pAllocator, pSwapchain);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(
    VkInstance                                  instance,
    const VkWin32SurfaceCreateInfoKHR*          pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSurfaceKHR*                               pSurface)
{
    const auto function = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(ResolveInstance(instance, "vkCreateWin32SurfaceKHR"));
    if (!function || function == &vkCreateWin32SurfaceKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(instance, pCreateInfo, pAllocator, pSurface);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(
    VkDevice                                    device,
    VkBuffer                                    buffer,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyBuffer>(ResolveDevice(device, "vkDestroyBuffer"));
    if (!function || function == &vkDestroyBuffer)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyBuffer");
    function(device, buffer, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyBufferView(
    VkDevice                                    device,
    VkBufferView                                bufferView,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyBufferView>(ResolveDevice(device, "vkDestroyBufferView"));
    if (!function || function == &vkDestroyBufferView)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyBufferView");
    function(device, bufferView, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyCommandPool>(ResolveDevice(device, "vkDestroyCommandPool"));
    if (!function || function == &vkDestroyCommandPool)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyCommandPool");
    ForgetCommandPool(device, commandPool);
    function(device, commandPool, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyDescriptorPool>(ResolveDevice(device, "vkDestroyDescriptorPool"));
    if (!function || function == &vkDestroyDescriptorPool)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyDescriptorPool");
    function(device, descriptorPool, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(
    VkDevice                                    device,
    VkDescriptorSetLayout                       descriptorSetLayout,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(ResolveDevice(device, "vkDestroyDescriptorSetLayout"));
    if (!function || function == &vkDestroyDescriptorSetLayout)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyDescriptorSetLayout");
    function(device, descriptorSetLayout, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
    VkDevice                                    device,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyDevice>(ResolveDevice(device, "vkDestroyDevice"));
    if (!function || function == &vkDestroyDevice)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyDevice");
    AsteriaUnregisterVulkanDevice(device);
    function(device, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(
    VkDevice                                    device,
    VkEvent                                     event,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyEvent>(ResolveDevice(device, "vkDestroyEvent"));
    if (!function || function == &vkDestroyEvent)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyEvent");
    function(device, event, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyFence(
    VkDevice                                    device,
    VkFence                                     fence,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyFence>(ResolveDevice(device, "vkDestroyFence"));
    if (!function || function == &vkDestroyFence)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyFence");
    function(device, fence, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(
    VkDevice                                    device,
    VkFramebuffer                               framebuffer,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyFramebuffer>(ResolveDevice(device, "vkDestroyFramebuffer"));
    if (!function || function == &vkDestroyFramebuffer)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyFramebuffer");
    function(device, framebuffer, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice                                    device,
    VkImage                                     image,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyImage>(ResolveDevice(device, "vkDestroyImage"));
    if (!function || function == &vkDestroyImage)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyImage");
    function(device, image, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
    VkDevice                                    device,
    VkImageView                                 imageView,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyImageView>(ResolveDevice(device, "vkDestroyImageView"));
    if (!function || function == &vkDestroyImageView)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyImageView");
    function(device, imageView, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
    VkInstance                                  instance,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyInstance>(ResolveInstance(instance, "vkDestroyInstance"));
    if (!function || function == &vkDestroyInstance)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyInstance");
    AsteriaUnregisterVulkanInstance(instance);
    function(instance, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(
    VkDevice                                    device,
    VkPipeline                                  pipeline,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyPipeline>(ResolveDevice(device, "vkDestroyPipeline"));
    if (!function || function == &vkDestroyPipeline)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyPipeline");
    function(device, pipeline, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyPipelineCache>(ResolveDevice(device, "vkDestroyPipelineCache"));
    if (!function || function == &vkDestroyPipelineCache)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyPipelineCache");
    function(device, pipelineCache, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(
    VkDevice                                    device,
    VkPipelineLayout                            pipelineLayout,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyPipelineLayout>(ResolveDevice(device, "vkDestroyPipelineLayout"));
    if (!function || function == &vkDestroyPipelineLayout)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyPipelineLayout");
    function(device, pipelineLayout, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(
    VkDevice                                    device,
    VkQueryPool                                 queryPool,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyQueryPool>(ResolveDevice(device, "vkDestroyQueryPool"));
    if (!function || function == &vkDestroyQueryPool)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyQueryPool");
    function(device, queryPool, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(
    VkDevice                                    device,
    VkRenderPass                                renderPass,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyRenderPass>(ResolveDevice(device, "vkDestroyRenderPass"));
    if (!function || function == &vkDestroyRenderPass)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyRenderPass");
    function(device, renderPass, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroySampler(
    VkDevice                                    device,
    VkSampler                                   sampler,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroySampler>(ResolveDevice(device, "vkDestroySampler"));
    if (!function || function == &vkDestroySampler)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroySampler");
    function(device, sampler, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(
    VkDevice                                    device,
    VkSemaphore                                 semaphore,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroySemaphore>(ResolveDevice(device, "vkDestroySemaphore"));
    if (!function || function == &vkDestroySemaphore)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroySemaphore");
    function(device, semaphore, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
    VkDevice                                    device,
    VkShaderModule                              shaderModule,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroyShaderModule>(ResolveDevice(device, "vkDestroyShaderModule"));
    if (!function || function == &vkDestroyShaderModule)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroyShaderModule");
    function(device, shaderModule, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(
    VkInstance                                  instance,
    VkSurfaceKHR                                surface,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroySurfaceKHR>(ResolveInstance(instance, "vkDestroySurfaceKHR"));
    if (!function || function == &vkDestroySurfaceKHR)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroySurfaceKHR");
    function(instance, surface, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkDestroySwapchainKHR>(ResolveDevice(device, "vkDestroySwapchainKHR"));
    if (!function || function == &vkDestroySwapchainKHR)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkDestroySwapchainKHR");
    function(device, swapchain, pAllocator);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(
    VkDevice                                    device)
{
    const auto function = reinterpret_cast<PFN_vkDeviceWaitIdle>(ResolveDevice(device, "vkDeviceWaitIdle"));
    if (!function || function == &vkDeviceWaitIdle)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(
    VkCommandBuffer                             commandBuffer)
{
    const auto function = reinterpret_cast<PFN_vkEndCommandBuffer>(ResolveCommand(commandBuffer, "vkEndCommandBuffer"));
    if (!function || function == &vkEndCommandBuffer)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(commandBuffer);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice                            physicalDevice,
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
    const auto function = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(ResolvePhysical(physicalDevice, "vkEnumerateDeviceExtensionProperties"));
    if (!function || function == &vkEnumerateDeviceExtensionProperties)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(physicalDevice, pLayerName, pPropertyCount, pProperties);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice                            physicalDevice,
    uint32_t*                                   pPropertyCount,
    VkLayerProperties*                          pProperties)
{
    const auto function = reinterpret_cast<PFN_vkEnumerateDeviceLayerProperties>(ResolvePhysical(physicalDevice, "vkEnumerateDeviceLayerProperties"));
    if (!function || function == &vkEnumerateDeviceLayerProperties)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(physicalDevice, pPropertyCount, pProperties);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
    const auto function = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(ResolveGlobal("vkEnumerateInstanceExtensionProperties"));
    if (!function || function == &vkEnumerateInstanceExtensionProperties)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(pLayerName, pPropertyCount, pProperties);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t*                                   pPropertyCount,
    VkLayerProperties*                          pProperties)
{
    const auto function = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(ResolveGlobal("vkEnumerateInstanceLayerProperties"));
    if (!function || function == &vkEnumerateInstanceLayerProperties)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(pPropertyCount, pProperties);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(
    uint32_t*                                   pApiVersion)
{
    const auto function = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(ResolveGlobal("vkEnumerateInstanceVersion"));
    if (!function || function == &vkEnumerateInstanceVersion)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(pApiVersion);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(
    VkInstance                                  instance,
    uint32_t*                                   pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties*            pPhysicalDeviceGroupProperties)
{
    const auto function = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroups>(ResolveInstance(instance, "vkEnumeratePhysicalDeviceGroups"));
    if (!function || function == &vkEnumeratePhysicalDeviceGroups)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = function(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && pPhysicalDeviceGroupProperties && pPhysicalDeviceGroupCount)
        for (uint32_t group = 0; group < *pPhysicalDeviceGroupCount; ++group)
            for (uint32_t index = 0; index < pPhysicalDeviceGroupProperties[group].physicalDeviceCount; ++index)
                AsteriaRegisterVulkanPhysicalDevice(instance, pPhysicalDeviceGroupProperties[group].physicalDevices[index]);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroupsKHR(
    VkInstance                                  instance,
    uint32_t*                                   pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties*            pPhysicalDeviceGroupProperties)
{
    const auto function = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceGroupsKHR>(ResolveInstance(instance, "vkEnumeratePhysicalDeviceGroupsKHR"));
    if (!function || function == &vkEnumeratePhysicalDeviceGroupsKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = function(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && pPhysicalDeviceGroupProperties && pPhysicalDeviceGroupCount)
        for (uint32_t group = 0; group < *pPhysicalDeviceGroupCount; ++group)
            for (uint32_t index = 0; index < pPhysicalDeviceGroupProperties[group].physicalDeviceCount; ++index)
                AsteriaRegisterVulkanPhysicalDevice(instance, pPhysicalDeviceGroupProperties[group].physicalDevices[index]);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance                                  instance,
    uint32_t*                                   pPhysicalDeviceCount,
    VkPhysicalDevice*                           pPhysicalDevices)
{
    const auto function = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(ResolveInstance(instance, "vkEnumeratePhysicalDevices"));
    if (!function || function == &vkEnumeratePhysicalDevices)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkResult result = function(instance, pPhysicalDeviceCount, pPhysicalDevices);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && pPhysicalDevices && pPhysicalDeviceCount)
        for (uint32_t index = 0; index < *pPhysicalDeviceCount; ++index)
            AsteriaRegisterVulkanPhysicalDevice(instance, pPhysicalDevices[index]);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(
    VkDevice                                    device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges)
{
    const auto function = reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(ResolveDevice(device, "vkFlushMappedMemoryRanges"));
    if (!function || function == &vkFlushMappedMemoryRanges)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, memoryRangeCount, pMemoryRanges);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    uint32_t                                    commandBufferCount,
    const VkCommandBuffer*                      pCommandBuffers)
{
    const auto function = reinterpret_cast<PFN_vkFreeCommandBuffers>(ResolveDevice(device, "vkFreeCommandBuffers"));
    if (!function || function == &vkFreeCommandBuffers)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkFreeCommandBuffers");
    if (pCommandBuffers) for (uint32_t index = 0; index < commandBufferCount; ++index)
        AsteriaUnregisterVulkanCommandBuffer(pCommandBuffers[index]);
    function(device, commandPool, commandBufferCount, pCommandBuffers);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    uint32_t                                    descriptorSetCount,
    const VkDescriptorSet*                      pDescriptorSets)
{
    const auto function = reinterpret_cast<PFN_vkFreeDescriptorSets>(ResolveDevice(device, "vkFreeDescriptorSets"));
    if (!function || function == &vkFreeDescriptorSets)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, descriptorPool, descriptorSetCount, pDescriptorSets);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    const VkAllocationCallbacks*                pAllocator)
{
    const auto function = reinterpret_cast<PFN_vkFreeMemory>(ResolveDevice(device, "vkFreeMemory"));
    if (!function || function == &vkFreeMemory)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkFreeMemory");
    function(device, memory, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(
    VkDevice                                    device,
    VkBuffer                                    buffer,
    VkMemoryRequirements*                       pMemoryRequirements)
{
    const auto function = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(ResolveDevice(device, "vkGetBufferMemoryRequirements"));
    if (!function || function == &vkGetBufferMemoryRequirements)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetBufferMemoryRequirements");
    function(device, buffer, pMemoryRequirements);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceMemoryCommitment(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    VkDeviceSize*                               pCommittedMemoryInBytes)
{
    const auto function = reinterpret_cast<PFN_vkGetDeviceMemoryCommitment>(ResolveDevice(device, "vkGetDeviceMemoryCommitment"));
    if (!function || function == &vkGetDeviceMemoryCommitment)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetDeviceMemoryCommitment");
    function(device, memory, pCommittedMemoryInBytes);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice                                    device,
    const char*                                 pName)
{
    return pName ? ResolveDevice(device, pName) : nullptr;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
    VkDevice                                    device,
    uint32_t                                    queueFamilyIndex,
    uint32_t                                    queueIndex,
    VkQueue*                                    pQueue)
{
    const auto function = reinterpret_cast<PFN_vkGetDeviceQueue>(ResolveDevice(device, "vkGetDeviceQueue"));
    if (!function || function == &vkGetDeviceQueue)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetDeviceQueue");
    function(device, queueFamilyIndex, queueIndex, pQueue);
    if (pQueue && *pQueue) AsteriaRegisterVulkanQueue(device, *pQueue);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice                                    device,
    const VkDeviceQueueInfo2*                   pQueueInfo,
    VkQueue*                                    pQueue)
{
    const auto function = reinterpret_cast<PFN_vkGetDeviceQueue2>(ResolveDevice(device, "vkGetDeviceQueue2"));
    if (!function || function == &vkGetDeviceQueue2)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetDeviceQueue2");
    function(device, pQueueInfo, pQueue);
    if (pQueue && *pQueue) AsteriaRegisterVulkanQueue(device, *pQueue);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(
    VkDevice                                    device,
    VkEvent                                     event)
{
    const auto function = reinterpret_cast<PFN_vkGetEventStatus>(ResolveDevice(device, "vkGetEventStatus"));
    if (!function || function == &vkGetEventStatus)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, event);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(
    VkDevice                                    device,
    VkFence                                     fence)
{
    const auto function = reinterpret_cast<PFN_vkGetFenceStatus>(ResolveDevice(device, "vkGetFenceStatus"));
    if (!function || function == &vkGetFenceStatus)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, fence);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(
    VkDevice                                    device,
    VkImage                                     image,
    VkMemoryRequirements*                       pMemoryRequirements)
{
    const auto function = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(ResolveDevice(device, "vkGetImageMemoryRequirements"));
    if (!function || function == &vkGetImageMemoryRequirements)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetImageMemoryRequirements");
    function(device, image, pMemoryRequirements);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetImageSparseMemoryRequirements(
    VkDevice                                    device,
    VkImage                                     image,
    uint32_t*                                   pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements*            pSparseMemoryRequirements)
{
    const auto function = reinterpret_cast<PFN_vkGetImageSparseMemoryRequirements>(ResolveDevice(device, "vkGetImageSparseMemoryRequirements"));
    if (!function || function == &vkGetImageSparseMemoryRequirements)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetImageSparseMemoryRequirements");
    function(device, image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(
    VkDevice                                    device,
    VkImage                                     image,
    const VkImageSubresource*                   pSubresource,
    VkSubresourceLayout*                        pLayout)
{
    const auto function = reinterpret_cast<PFN_vkGetImageSubresourceLayout>(ResolveDevice(device, "vkGetImageSubresourceLayout"));
    if (!function || function == &vkGetImageSubresourceLayout)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetImageSubresourceLayout");
    function(device, image, pSubresource, pLayout);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance                                  instance,
    const char*                                 pName)
{
    return pName ? ResolveInstance(instance, pName) : nullptr;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceFeatures*                   pFeatures)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceFeatures"));
    if (!function || function == &vkGetPhysicalDeviceFeatures)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetPhysicalDeviceFeatures");
    function(physicalDevice, pFeatures);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceFeatures2*                  pFeatures)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceFeatures2"));
    if (!function || function == &vkGetPhysicalDeviceFeatures2)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetPhysicalDeviceFeatures2");
    function(physicalDevice, pFeatures);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkFormatProperties*                         pFormatProperties)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceFormatProperties"));
    if (!function || function == &vkGetPhysicalDeviceFormatProperties)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetPhysicalDeviceFormatProperties");
    function(physicalDevice, format, pFormatProperties);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkImageType                                 type,
    VkImageTiling                               tiling,
    VkImageUsageFlags                           usage,
    VkImageCreateFlags                          flags,
    VkImageFormatProperties*                    pImageFormatProperties)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceImageFormatProperties"));
    if (!function || function == &vkGetPhysicalDeviceImageFormatProperties)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(physicalDevice, format, type, tiling, usage, flags, pImageFormatProperties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceMemoryProperties*           pMemoryProperties)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceMemoryProperties"));
    if (!function || function == &vkGetPhysicalDeviceMemoryProperties)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetPhysicalDeviceMemoryProperties");
    function(physicalDevice, pMemoryProperties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceProperties*                 pProperties)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceProperties"));
    if (!function || function == &vkGetPhysicalDeviceProperties)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetPhysicalDeviceProperties");
    function(physicalDevice, pProperties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice                            physicalDevice,
    VkPhysicalDeviceProperties2*                pProperties)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceProperties2"));
    if (!function || function == &vkGetPhysicalDeviceProperties2)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetPhysicalDeviceProperties2");
    function(physicalDevice, pProperties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice                            physicalDevice,
    uint32_t*                                   pQueueFamilyPropertyCount,
    VkQueueFamilyProperties*                    pQueueFamilyProperties)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!function || function == &vkGetPhysicalDeviceQueueFamilyProperties)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetPhysicalDeviceQueueFamilyProperties");
    function(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice                            physicalDevice,
    VkFormat                                    format,
    VkImageType                                 type,
    VkSampleCountFlagBits                       samples,
    VkImageUsageFlags                           usage,
    VkImageTiling                               tiling,
    uint32_t*                                   pPropertyCount,
    VkSparseImageFormatProperties*              pProperties)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceSparseImageFormatProperties>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceSparseImageFormatProperties"));
    if (!function || function == &vkGetPhysicalDeviceSparseImageFormatProperties)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetPhysicalDeviceSparseImageFormatProperties");
    function(physicalDevice, format, type, samples, usage, tiling, pPropertyCount, pProperties);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                surface,
    VkSurfaceCapabilitiesKHR*                   pSurfaceCapabilities)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    if (!function || function == &vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(physicalDevice, surface, pSurfaceCapabilities);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                surface,
    uint32_t*                                   pSurfaceFormatCount,
    VkSurfaceFormatKHR*                         pSurfaceFormats)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    if (!function || function == &vkGetPhysicalDeviceSurfaceFormatsKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice                            physicalDevice,
    VkSurfaceKHR                                surface,
    uint32_t*                                   pPresentModeCount,
    VkPresentModeKHR*                           pPresentModes)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
    if (!function || function == &vkGetPhysicalDeviceSurfacePresentModesKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(physicalDevice, surface, pPresentModeCount, pPresentModes);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t                                    queueFamilyIndex,
    VkSurfaceKHR                                surface,
    VkBool32*                                   pSupported)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    if (!function || function == &vkGetPhysicalDeviceSurfaceSupportKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(physicalDevice, queueFamilyIndex, surface, pSupported);
}

extern "C" VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceWin32PresentationSupportKHR(
    VkPhysicalDevice                            physicalDevice,
    uint32_t                                    queueFamilyIndex)
{
    const auto function = reinterpret_cast<PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR>(ResolvePhysical(physicalDevice, "vkGetPhysicalDeviceWin32PresentationSupportKHR"));
    if (!function || function == &vkGetPhysicalDeviceWin32PresentationSupportKHR)
        return {};
    return function(physicalDevice, queueFamilyIndex);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    size_t*                                     pDataSize,
    void*                                       pData)
{
    const auto function = reinterpret_cast<PFN_vkGetPipelineCacheData>(ResolveDevice(device, "vkGetPipelineCacheData"));
    if (!function || function == &vkGetPipelineCacheData)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pipelineCache, pDataSize, pData);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(
    VkDevice                                    device,
    VkQueryPool                                 queryPool,
    uint32_t                                    firstQuery,
    uint32_t                                    queryCount,
    size_t                                      dataSize,
    void*                                       pData,
    VkDeviceSize                                stride,
    VkQueryResultFlags                          flags)
{
    const auto function = reinterpret_cast<PFN_vkGetQueryPoolResults>(ResolveDevice(device, "vkGetQueryPoolResults"));
    if (!function || function == &vkGetQueryPoolResults)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetRenderAreaGranularity(
    VkDevice                                    device,
    VkRenderPass                                renderPass,
    VkExtent2D*                                 pGranularity)
{
    const auto function = reinterpret_cast<PFN_vkGetRenderAreaGranularity>(ResolveDevice(device, "vkGetRenderAreaGranularity"));
    if (!function || function == &vkGetRenderAreaGranularity)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkGetRenderAreaGranularity");
    function(device, renderPass, pGranularity);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(
    VkDevice                                    device,
    VkSemaphore                                 semaphore,
    uint64_t*                                   pValue)
{
    const auto function = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(ResolveDevice(device, "vkGetSemaphoreCounterValue"));
    if (!function || function == &vkGetSemaphoreCounterValue)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, semaphore, pValue);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice                                    device,
    VkSwapchainKHR                              swapchain,
    uint32_t*                                   pSwapchainImageCount,
    VkImage*                                    pSwapchainImages)
{
    const auto predicate = swapchain_token_predicate.load(std::memory_order_acquire);
    const auto replacement = swapchain_images_replacement.load(std::memory_order_acquire);
    if (predicate && replacement && predicate(swapchain))
        return replacement(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    const auto function = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(ResolveDevice(device, "vkGetSwapchainImagesKHR"));
    if (!function || function == &vkGetSwapchainImagesKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(
    VkDevice                                    device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges)
{
    const auto function = reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(ResolveDevice(device, "vkInvalidateMappedMemoryRanges"));
    if (!function || function == &vkInvalidateMappedMemoryRanges)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, memoryRangeCount, pMemoryRanges);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(
    VkDevice                                    device,
    VkDeviceMemory                              memory,
    VkDeviceSize                                offset,
    VkDeviceSize                                size,
    VkMemoryMapFlags                            flags,
    void**                                      ppData)
{
    const auto function = reinterpret_cast<PFN_vkMapMemory>(ResolveDevice(device, "vkMapMemory"));
    if (!function || function == &vkMapMemory)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, memory, offset, size, flags, ppData);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(
    VkDevice                                    device,
    VkPipelineCache                             dstCache,
    uint32_t                                    srcCacheCount,
    const VkPipelineCache*                      pSrcCaches)
{
    const auto function = reinterpret_cast<PFN_vkMergePipelineCaches>(ResolveDevice(device, "vkMergePipelineCaches"));
    if (!function || function == &vkMergePipelineCaches)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, dstCache, srcCacheCount, pSrcCaches);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(
    VkQueue                                     queue,
    uint32_t                                    bindInfoCount,
    const VkBindSparseInfo*                     pBindInfo,
    VkFence                                     fence)
{
    const auto function = reinterpret_cast<PFN_vkQueueBindSparse>(ResolveQueue(queue, "vkQueueBindSparse"));
    if (!function || function == &vkQueueBindSparse)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(queue, bindInfoCount, pBindInfo, fence);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue                                     queue,
    const VkPresentInfoKHR*                     pPresentInfo)
{
    const auto function = reinterpret_cast<PFN_vkQueuePresentKHR>(ResolveQueue(queue, "vkQueuePresentKHR"));
    if (!function || function == &vkQueuePresentKHR)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    native_present_attempts.fetch_add(1, std::memory_order_relaxed);
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
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue                                     queue,
    uint32_t                                    submitCount,
    const VkSubmitInfo*                         pSubmits,
    VkFence                                     fence)
{
    const auto function = reinterpret_cast<PFN_vkQueueSubmit>(ResolveQueue(queue, "vkQueueSubmit"));
    if (!function || function == &vkQueueSubmit)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(queue, submitCount, pSubmits, fence);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(
    VkQueue                                     queue)
{
    const auto function = reinterpret_cast<PFN_vkQueueWaitIdle>(ResolveQueue(queue, "vkQueueWaitIdle"));
    if (!function || function == &vkQueueWaitIdle)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(queue);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(
    VkCommandBuffer                             commandBuffer,
    VkCommandBufferResetFlags                   flags)
{
    const auto function = reinterpret_cast<PFN_vkResetCommandBuffer>(ResolveCommand(commandBuffer, "vkResetCommandBuffer"));
    if (!function || function == &vkResetCommandBuffer)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(commandBuffer, flags);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
    VkDevice                                    device,
    VkCommandPool                               commandPool,
    VkCommandPoolResetFlags                     flags)
{
    const auto function = reinterpret_cast<PFN_vkResetCommandPool>(ResolveDevice(device, "vkResetCommandPool"));
    if (!function || function == &vkResetCommandPool)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, commandPool, flags);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(
    VkDevice                                    device,
    VkDescriptorPool                            descriptorPool,
    VkDescriptorPoolResetFlags                  flags)
{
    const auto function = reinterpret_cast<PFN_vkResetDescriptorPool>(ResolveDevice(device, "vkResetDescriptorPool"));
    if (!function || function == &vkResetDescriptorPool)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, descriptorPool, flags);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(
    VkDevice                                    device,
    VkEvent                                     event)
{
    const auto function = reinterpret_cast<PFN_vkResetEvent>(ResolveDevice(device, "vkResetEvent"));
    if (!function || function == &vkResetEvent)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, event);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(
    VkDevice                                    device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences)
{
    const auto function = reinterpret_cast<PFN_vkResetFences>(ResolveDevice(device, "vkResetFences"));
    if (!function || function == &vkResetFences)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, fenceCount, pFences);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(
    VkDevice                                    device,
    VkEvent                                     event)
{
    const auto function = reinterpret_cast<PFN_vkSetEvent>(ResolveDevice(device, "vkSetEvent"));
    if (!function || function == &vkSetEvent)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, event);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(
    VkDevice                                    device,
    VkDeviceMemory                              memory)
{
    const auto function = reinterpret_cast<PFN_vkUnmapMemory>(ResolveDevice(device, "vkUnmapMemory"));
    if (!function || function == &vkUnmapMemory)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkUnmapMemory");
    function(device, memory);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(
    VkDevice                                    device,
    uint32_t                                    descriptorWriteCount,
    const VkWriteDescriptorSet*                 pDescriptorWrites,
    uint32_t                                    descriptorCopyCount,
    const VkCopyDescriptorSet*                  pDescriptorCopies)
{
    const auto function = reinterpret_cast<PFN_vkUpdateDescriptorSets>(ResolveDevice(device, "vkUpdateDescriptorSets"));
    if (!function || function == &vkUpdateDescriptorSets)
        throw std::runtime_error("Unavailable or recursive Vulkan function: vkUpdateDescriptorSets");
    function(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
    VkDevice                                    device,
    uint32_t                                    fenceCount,
    const VkFence*                              pFences,
    VkBool32                                    waitAll,
    uint64_t                                    timeout)
{
    const auto function = reinterpret_cast<PFN_vkWaitForFences>(ResolveDevice(device, "vkWaitForFences"));
    if (!function || function == &vkWaitForFences)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, fenceCount, pFences, waitAll, timeout);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(
    VkDevice                                    device,
    const VkSemaphoreWaitInfo*                  pWaitInfo,
    uint64_t                                    timeout)
{
    const auto function = reinterpret_cast<PFN_vkWaitSemaphores>(ResolveDevice(device, "vkWaitSemaphores"));
    if (!function || function == &vkWaitSemaphores)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return function(device, pWaitInfo, timeout);
}

