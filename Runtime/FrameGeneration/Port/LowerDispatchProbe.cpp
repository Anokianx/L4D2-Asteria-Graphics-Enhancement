#include "AsteriaVulkanShim.h"
#include <json.hpp>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

static VkInstance fake_instances[] = {reinterpret_cast<VkInstance>(0x1100), reinterpret_cast<VkInstance>(0x1200)};
static VkPhysicalDevice fake_physicals[] = {reinterpret_cast<VkPhysicalDevice>(0x2100), reinterpret_cast<VkPhysicalDevice>(0x2200)};
static VkDevice fake_devices[] = {reinterpret_cast<VkDevice>(0x3100), reinterpret_cast<VkDevice>(0x3200)};
static VkQueue fake_queues[] = {reinterpret_cast<VkQueue>(0x4100), reinterpret_cast<VkQueue>(0x4200)};
static VkCommandBuffer fake_commands[] = {reinterpret_cast<VkCommandBuffer>(0x5100), reinterpret_cast<VkCommandBuffer>(0x5200)};
static std::atomic<uint32_t> fake_present_counts[2]{};
static std::atomic<uint32_t> upper_trap_count{0};
static std::atomic<bool> self_dispatch{false};
static std::atomic<uint32_t> native_reuse_callbacks{0};
static PFN_vkGetDeviceProcAddr native_device_proc = nullptr;
static PFN_vkQueuePresentKHR native_present = nullptr;
static std::atomic<uint32_t> actual_lower_present_count{0};

static void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static void Require(VkResult value, const char* message)
{
    if (value != VK_SUCCESS && value != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(value));
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeDeviceProc(VkDevice device, const char* name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeInstanceProc(VkInstance instance, const char* name);

static VKAPI_ATTR void VKAPI_CALL ReusingFreeCommands(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* commands)
{
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    for (uint32_t index = 0; index < count; ++index)
    {
        Check(vkBeginCommandBuffer(commands[index], &begin) == VK_ERROR_EXTENSION_NOT_PRESENT, "old free command owner retired before native callback");
        Check(AsteriaRegisterVulkanCommandBuffer(device, commands[index], pool), "register reused free command address");
    }
    native_reuse_callbacks.fetch_add(1);
}

static VKAPI_ATTR void VKAPI_CALL ReusingDestroyPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks*)
{
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(fake_commands[0], &begin) == VK_ERROR_EXTENSION_NOT_PRESENT, "old pool command owner retired before native callback");
    Check(AsteriaRegisterVulkanCommandBuffer(device, fake_commands[0], pool), "register reused pool and command address");
    native_reuse_callbacks.fetch_add(1);
}

static VKAPI_ATTR void VKAPI_CALL ReusingDestroyDevice(VkDevice device, const VkAllocationCallbacks*)
{
    Check(AsteriaResolveVulkanDeviceFunction(device, "vkQueuePresentKHR") == nullptr, "old device owner retired before native callback");
    Check(AsteriaRegisterVulkanDevice(fake_instances[0], fake_physicals[0], device, &FakeDeviceProc), "register reused device address");
    Check(AsteriaRegisterVulkanQueue(device, fake_queues[0]), "register reused queue address");
    Check(AsteriaRegisterVulkanCommandBuffer(device, fake_commands[0]), "register reused device command address");
    native_reuse_callbacks.fetch_add(1);
}

static VKAPI_ATTR void VKAPI_CALL ReusingDestroyInstance(VkInstance instance, const VkAllocationCallbacks*)
{
    Check(AsteriaResolveVulkanInstanceFunction(instance, "vkGetPhysicalDeviceProperties") == nullptr, "old instance owner retired before native callback");
    Check(AsteriaRegisterVulkanInstance(instance, &FakeInstanceProc), "register reused instance address");
    Check(AsteriaRegisterVulkanPhysicalDevice(instance, fake_physicals[0]), "register reused physical address");
    Check(AsteriaRegisterVulkanDevice(instance, fake_physicals[0], fake_devices[0], &FakeDeviceProc), "register reused instance device address");
    Check(AsteriaRegisterVulkanQueue(fake_devices[0], fake_queues[0]), "register reused instance queue address");
    Check(AsteriaRegisterVulkanCommandBuffer(fake_devices[0], fake_commands[0]), "register reused instance command address");
    native_reuse_callbacks.fetch_add(1);
}

static VKAPI_ATTR VkResult VKAPI_CALL UpperPresentTrap(VkQueue queue, const VkPresentInfoKHR* info)
{
    upper_trap_count.fetch_add(1);
    return vkQueuePresentKHR(queue, info);
}

static VKAPI_ATTR VkResult VKAPI_CALL FakePresentFirst(VkQueue queue, const VkPresentInfoKHR*)
{
    if (queue == fake_queues[0]) { fake_present_counts[0].fetch_add(1); return VK_SUCCESS; }
    return VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR VkResult VKAPI_CALL FakePresentSecond(VkQueue queue, const VkPresentInfoKHR*)
{
    if (queue == fake_queues[1]) { fake_present_counts[1].fetch_add(1); return VK_SUBOPTIMAL_KHR; }
    return VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR VkResult VKAPI_CALL FakeCommandBeginFirst(VkCommandBuffer command, const VkCommandBufferBeginInfo*)
{
    return command == fake_commands[0] ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR VkResult VKAPI_CALL FakeCommandBeginSecond(VkCommandBuffer command, const VkCommandBufferBeginInfo*)
{
    return command == fake_commands[1] ? VK_NOT_READY : VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR void VKAPI_CALL FakePropertiesFirst(VkPhysicalDevice physical, VkPhysicalDeviceProperties* properties)
{
    properties->deviceID = physical == fake_physicals[0] ? 1001 : 0;
}

static VKAPI_ATTR void VKAPI_CALL FakePropertiesSecond(VkPhysicalDevice physical, VkPhysicalDeviceProperties* properties)
{
    properties->deviceID = physical == fake_physicals[1] ? 2002 : 0;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeDeviceProc(VkDevice device, const char* name)
{
    if (device != fake_devices[0] && device != fake_devices[1]) return nullptr;
    if (!std::strcmp(name, "vkQueuePresentKHR"))
        return reinterpret_cast<PFN_vkVoidFunction>(self_dispatch.load() ? &vkQueuePresentKHR : device == fake_devices[0] ? &FakePresentFirst : &FakePresentSecond);
    if (!std::strcmp(name, "vkBeginCommandBuffer"))
        return reinterpret_cast<PFN_vkVoidFunction>(device == fake_devices[0] ? &FakeCommandBeginFirst : &FakeCommandBeginSecond);
    if (!std::strcmp(name, "vkFreeCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(&ReusingFreeCommands);
    if (!std::strcmp(name, "vkDestroyCommandPool")) return reinterpret_cast<PFN_vkVoidFunction>(&ReusingDestroyPool);
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(&ReusingDestroyDevice);
    return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeInstanceProc(VkInstance instance, const char* name)
{
    if (instance != fake_instances[0] && instance != fake_instances[1]) return nullptr;
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&FakeDeviceProc);
    if (!std::strcmp(name, "vkGetPhysicalDeviceProperties"))
        return reinterpret_cast<PFN_vkVoidFunction>(instance == fake_instances[0] ? &FakePropertiesFirst : &FakePropertiesSecond);
    if (!std::strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(&ReusingDestroyInstance);
    return nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL ActualLowerPresent(VkQueue queue, const VkPresentInfoKHR* info)
{
    actual_lower_present_count.fetch_add(1);
    return native_present(queue, info);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ActualLowerDeviceProc(VkDevice device, const char* name)
{
    if (!std::strcmp(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(&ActualLowerPresent);
    return native_device_proc(device, name);
}

static void SimulatedDispatch(nlohmann::json& report)
{
    AsteriaSetVulkanPrototypeFallback(false);
    for (uint32_t index = 0; index < 2; ++index)
    {
        Check(AsteriaRegisterVulkanInstance(fake_instances[index], &FakeInstanceProc), "register simulated instance");
        Check(AsteriaRegisterVulkanPhysicalDevice(fake_instances[index], fake_physicals[index]), "register simulated physical");
        Check(AsteriaRegisterVulkanDevice(fake_instances[index], fake_physicals[index], fake_devices[index], &FakeDeviceProc), "register simulated device");
        Check(AsteriaRegisterVulkanQueue(fake_devices[index], fake_queues[index]), "register simulated queue");
        Check(AsteriaRegisterVulkanCommandBuffer(fake_devices[index], fake_commands[index]), "register simulated command buffer");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(fake_physicals[index], &properties);
        Check(properties.deviceID == (index ? 2002 : 1001), "physical instance ownership");
    }
    Check(!AsteriaRegisterVulkanQueue(fake_devices[1], fake_queues[0]), "reject foreign queue ownership");
    Check(!AsteriaRegisterVulkanCommandBuffer(fake_devices[1], fake_commands[0]), "reject foreign command ownership");
    Check(!AsteriaRegisterVulkanPhysicalDevice(fake_instances[1], fake_physicals[0]), "reject foreign physical ownership");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(fake_commands[0], &begin) == VK_SUCCESS, "route first command buffer");
    Check(vkBeginCommandBuffer(fake_commands[1], &begin) == VK_NOT_READY, "route second command buffer");
    Check(vkGetDeviceProcAddr(fake_devices[0], "vkQueuePresentKHR") == reinterpret_cast<PFN_vkVoidFunction>(&FakePresentFirst), "get lower present callback");
    Check(vkGetDeviceProcAddr(fake_devices[0], "vkAsteriaMissing") == nullptr, "missing registered function cannot fall back");
    Check(AsteriaResolveVulkanFunction("vkQueuePresentKHR") == nullptr, "ambiguous global lookup cannot fall back");
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    Check(vkQueuePresentKHR(reinterpret_cast<VkQueue>(0xdead), &info) == VK_ERROR_EXTENSION_NOT_PRESENT, "unknown queue cannot fall back");
    std::atomic<bool> threaded_success{true};
    auto submit = [&](uint32_t index)
    {
        for (uint32_t call = 0; call < 1000; ++call)
            if (vkQueuePresentKHR(fake_queues[index], &info) != (index ? VK_SUBOPTIMAL_KHR : VK_SUCCESS)) threaded_success.store(false);
    };
    std::thread first(submit, 0), second(submit, 1);
    first.join();
    second.join();
    Check(threaded_success.load() && fake_present_counts[0].load() == 1000 && fake_present_counts[1].load() == 1000, "threaded device isolation");
    self_dispatch.store(true);
    Check(vkQueuePresentKHR(fake_queues[0], &info) == VK_ERROR_EXTENSION_NOT_PRESENT, "recursive self forwarder cannot re-enter");
    self_dispatch.store(false);
    AsteriaUnregisterVulkanInstance(fake_instances[0]);
    Check(vkQueuePresentKHR(fake_queues[0], &info) == VK_ERROR_EXTENSION_NOT_PRESENT, "instance teardown retires queues");
    Check(vkBeginCommandBuffer(fake_commands[0], &begin) == VK_ERROR_EXTENSION_NOT_PRESENT, "instance teardown retires commands");
    Check(vkQueuePresentKHR(fake_queues[1], &info) == VK_SUBOPTIMAL_KHR, "teardown retains unrelated device");
    AsteriaUnregisterVulkanInstance(fake_instances[1]);
    Check(upper_trap_count.load() == 0, "upper present trap never entered");
    report["simulated"] = {{"devices", 2}, {"threaded_present_calls", 2000}, {"foreign_owner_rejected", true},
        {"unknown_owner_rejected", true}, {"recursive_self_rejected", true}, {"cascade_cleanup", true}, {"upper_trap_calls", 0}};
}

static void DestructionAddressReuse(nlohmann::json& report)
{
    const VkCommandPool pool = static_cast<VkCommandPool>(0x6100);
    Check(AsteriaRegisterVulkanInstance(fake_instances[0], &FakeInstanceProc), "register reuse fixture instance");
    Check(AsteriaRegisterVulkanPhysicalDevice(fake_instances[0], fake_physicals[0]), "register reuse fixture physical");
    Check(AsteriaRegisterVulkanDevice(fake_instances[0], fake_physicals[0], fake_devices[0], &FakeDeviceProc), "register reuse fixture device");
    Check(AsteriaRegisterVulkanQueue(fake_devices[0], fake_queues[0]), "register reuse fixture queue");
    Check(AsteriaRegisterVulkanCommandBuffer(fake_devices[0], fake_commands[0], pool), "register reuse fixture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkFreeCommandBuffers(fake_devices[0], pool, 1, &fake_commands[0]);
    Check(vkBeginCommandBuffer(fake_commands[0], &begin) == VK_SUCCESS, "free preserves immediately reused command owner");
    vkDestroyCommandPool(fake_devices[0], pool, nullptr);
    Check(vkBeginCommandBuffer(fake_commands[0], &begin) == VK_SUCCESS, "pool destroy preserves immediately reused command owner");
    vkDestroyDevice(fake_devices[0], nullptr);
    Check(AsteriaResolveVulkanDeviceFunction(fake_devices[0], "vkQueuePresentKHR") == reinterpret_cast<PFN_vkVoidFunction>(&FakePresentFirst), "device destroy preserves immediately reused device owner");
    Check(vkBeginCommandBuffer(fake_commands[0], &begin) == VK_SUCCESS, "device destroy preserves immediately reused command owner");
    vkDestroyInstance(fake_instances[0], nullptr);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(fake_physicals[0], &properties);
    Check(properties.deviceID == 1001, "instance destroy preserves immediately reused physical owner");
    Check(vkBeginCommandBuffer(fake_commands[0], &begin) == VK_SUCCESS, "instance destroy preserves immediately reused command owner");
    Check(native_reuse_callbacks.load() == 4, "all destruction reuse callbacks executed");
    AsteriaUnregisterVulkanInstance(fake_instances[0]);
    report["address_reuse"] = {{"native_callbacks", 4}, {"free_command_preserved", true}, {"destroy_pool_preserved", true},
        {"destroy_device_preserved", true}, {"destroy_instance_preserved", true}, {"old_owner_retired_before_native", true}};
}

static void RealLowerDispatch(nlohmann::json& report)
{
    AsteriaSetVulkanDispatch(VK_NULL_HANDLE, VK_NULL_HANDLE);
    SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", L"1");
    SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", nullptr);
    wchar_t system[MAX_PATH]{};
    Check(GetSystemDirectoryW(system, MAX_PATH) != 0, "system directory");
    const auto loader_path = std::filesystem::path(system) / L"vulkan-1.dll";
    HMODULE loader = LoadLibraryExW(loader_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(loader != nullptr, "native Vulkan loader");
    const auto native_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    Check(native_instance_proc != nullptr, "native get instance proc");
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence acquire_fence = VK_NULL_HANDLE;
    VkSemaphore completed = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    HWND window = nullptr;
    auto cleanup = [&]()
    {
        if (device)
        {
            vkDeviceWaitIdle(device);
            if (completed) vkDestroySemaphore(device, completed, nullptr);
            if (acquire_fence) vkDestroyFence(device, acquire_fence, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface) { vkDestroySurfaceKHR(instance, surface, nullptr); surface = VK_NULL_HANDLE; }
        if (instance) { vkDestroyInstance(instance, nullptr); instance = VK_NULL_HANDLE; }
        if (window) { DestroyWindow(window); window = nullptr; }
        if (loader) { FreeLibrary(loader); loader = nullptr; }
    };
    try
    {
        const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Asteria Lower Dispatch Probe";
        application.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &application;
        instance_info.enabledExtensionCount = 2;
        instance_info.ppEnabledExtensionNames = instance_extensions;
        Require(vkCreateInstance(&instance_info, nullptr, &instance), "create real instance");
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"AsteriaLowerDispatchProbe";
        RegisterClassW(&window_class);
        window = CreateWindowExW(0, window_class.lpszClassName, L"Asteria Lower Dispatch Probe", WS_POPUP, 0, 0, 96, 64, nullptr, nullptr, window_class.hInstance, nullptr);
        Check(window != nullptr, "hidden probe window");
        VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface_info.hinstance = window_class.hInstance;
        surface_info.hwnd = window;
        Require(vkCreateWin32SurfaceKHR(instance, &surface_info, nullptr, &surface), "create real surface");
        uint32_t count = 0;
        Require(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate physical count");
        Check(count != 0, "at least one GPU");
        std::vector<VkPhysicalDevice> physicals(count);
        Require(vkEnumeratePhysicalDevices(instance, &count, physicals.data()), "enumerate physical handles");
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        uint32_t family = 0;
        for (const auto candidate : physicals)
        {
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            for (uint32_t index = 0; index < family_count; ++index)
            {
                VkBool32 present = VK_FALSE;
                Require(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &present), "query presentation support");
                if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { physical = candidate; family = index; break; }
            }
            if (physical) break;
        }
        Check(physical != VK_NULL_HANDLE, "graphics present queue");
        const float priority = 1.f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = device_extensions;
        Require(vkCreateDevice(physical, &device_info, nullptr, &device), "create real device");
        vkGetDeviceQueue(device, family, 0, &queue);
        native_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(native_instance_proc(instance, "vkGetDeviceProcAddr"));
        native_present = reinterpret_cast<PFN_vkQueuePresentKHR>(native_device_proc(device, "vkQueuePresentKHR"));
        Check(native_present != nullptr, "native queue present");
        Check(AsteriaRegisterVulkanInstance(instance, native_instance_proc), "register actual lower instance");
        Check(AsteriaRegisterVulkanPhysicalDevice(instance, physical), "register actual physical owner");
        Check(AsteriaRegisterVulkanDevice(instance, physical, device, &ActualLowerDeviceProc), "register actual lower device");
        Check(AsteriaRegisterVulkanQueue(device, queue), "register actual lower queue");
        AsteriaSetVulkanPrototypeFallback(false);
        Check(vkGetDeviceProcAddr(device, "vkQueuePresentKHR") == reinterpret_cast<PFN_vkVoidFunction>(&ActualLowerPresent), "saved lower present pointer");
        VkSurfaceCapabilitiesKHR capabilities{};
        Require(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities), "surface capabilities");
        Check((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0, "transfer destination surface");
        uint32_t format_count = 0;
        Require(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr), "surface format count");
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        Require(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data()), "surface formats");
        VkSwapchainCreateInfoKHR swapchain_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchain_info.surface = surface;
        swapchain_info.minImageCount = capabilities.minImageCount;
        swapchain_info.imageFormat = formats[0].format;
        swapchain_info.imageColorSpace = formats[0].colorSpace;
        swapchain_info.imageExtent = capabilities.currentExtent;
        if (swapchain_info.imageExtent.width == UINT32_MAX) swapchain_info.imageExtent = {96, 64};
        swapchain_info.imageArrayLayers = 1;
        swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain_info.preTransform = capabilities.currentTransform;
        swapchain_info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(capabilities.supportedCompositeAlpha & (~capabilities.supportedCompositeAlpha + 1));
        swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchain_info.clipped = VK_TRUE;
        Require(vkCreateSwapchainKHR(device, &swapchain_info, nullptr, &swapchain), "create real swapchain");
        uint32_t image_count = 0;
        Require(vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr), "swapchain image count");
        std::vector<VkImage> images(image_count);
        Require(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images.data()), "swapchain images");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Require(vkCreateFence(device, &fence_info, nullptr, &acquire_fence), "create acquire fence");
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        Require(vkCreateSemaphore(device, &semaphore_info, nullptr, &completed), "create render semaphore");
        uint32_t acquired_index = 0;
        Require(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, acquire_fence, &acquired_index), "acquire real image");
        Require(vkWaitForFences(device, 1, &acquire_fence, VK_TRUE, UINT64_MAX), "wait acquire fence");
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = family;
        Require(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "create command pool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        Require(vkAllocateCommandBuffers(device, &allocate, &command), "allocate tracked command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Require(vkBeginCommandBuffer(command, &begin), "begin tracked command buffer");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[acquired_index];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkClearColorValue clear{{0.04f, 0.06f, 0.12f, 1.f}};
        vkCmdClearColorImage(command, images[acquired_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &barrier.subresourceRange);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        Require(vkEndCommandBuffer(command), "end tracked command buffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &completed;
        Require(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit to tracked lower queue");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &completed;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &acquired_index;
        Require(vkQueuePresentKHR(queue, &present), "present through saved lower dispatch");
        Require(vkQueueWaitIdle(queue), "wait lower queue");
        Check(actual_lower_present_count.load() == 1 && upper_trap_count.load() == 0, "exactly one lower GPU present");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        report["gpu"] = {{"device", properties.deviceName}, {"saved_lower_present_calls", 1}, {"upper_trap_calls", 0},
            {"standard_present_success", true}, {"tracked_command_buffer", true}, {"strict_mode", true}};
        const VkDevice retired_device = device;
        cleanup();
        Check(AsteriaResolveVulkanDeviceFunction(retired_device, "vkQueuePresentKHR") == nullptr, "real device dispatch cleaned");
    }
    catch (...)
    {
        cleanup();
        throw;
    }
}

int main()
{
    nlohmann::json report = {{"process_bits", sizeof(void*) * 8}, {"success", false}, {"game_frame_generation", false}};
    try
    {
        SimulatedDispatch(report);
        DestructionAddressReuse(report);
        RealLowerDispatch(report);
        report["success"] = true;
    }
    catch (const std::exception& error)
    {
        report["error"] = error.what();
    }
    std::filesystem::create_directories("results");
    std::ofstream("results/lower-dispatch.json") << report.dump(2) << '\n';
    std::cout << report.dump() << '\n';
    return report["success"].get<bool>() ? 0 : 1;
}
