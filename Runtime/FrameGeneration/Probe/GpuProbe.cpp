#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <json.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <dbghelp.h>
#include "../Layer/AsteriaFrameInputApi.h"

#pragma comment(lib, "user32.lib")

namespace fs = std::filesystem;
static fs::path CrashDumpPath;
static PFN_vkGetDeviceProcAddr NativeProbeDevice = nullptr;
static bool FailNativeAcquire = false, FailNativeRecreation = false;
static uint32_t NativeCreateAttempts = 0, NativeAcquireFailures = 0, NativeCreateFailures = 0;

static VkResult VKAPI_CALL ProbeNativeAcquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
    if (FailNativeAcquire) { FailNativeAcquire = false; ++NativeAcquireFailures; return VK_ERROR_OUT_OF_DATE_KHR; }
    const auto next = reinterpret_cast<PFN_vkAcquireNextImageKHR>(NativeProbeDevice(device, "vkAcquireNextImageKHR"));
    return next(device, swapchain, timeout, semaphore, fence, index);
}

static VkResult VKAPI_CALL ProbeNativeCreate(VkDevice device, const VkSwapchainCreateInfoKHR *info,
    const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain)
{
    ++NativeCreateAttempts;
    if (FailNativeRecreation && NativeCreateAttempts == 2)
    {
        ++NativeCreateFailures; *swapchain = UINT64_C(0xBAD0C0FFEE123456);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    const auto next = reinterpret_cast<PFN_vkCreateSwapchainKHR>(NativeProbeDevice(device, "vkCreateSwapchainKHR"));
    return next(device, info, allocator, swapchain);
}

static PFN_vkVoidFunction VKAPI_CALL ProbeNextDevice(VkDevice device, const char *name)
{
    if (std::strcmp(name, "vkAcquireNextImageKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ProbeNativeAcquire);
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&ProbeNativeCreate);
    return NativeProbeDevice(device, name);
}

static LONG WINAPI RecordNativeCrash(EXCEPTION_POINTERS *exception)
{
    const HMODULE module = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto write = module ? reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(module, "MiniDumpWriteDump")) : nullptr;
    const HANDLE file = CreateFileW(CrashDumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (write && file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION information{GetCurrentThreadId(), exception, FALSE};
        write(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithThreadInfo, &information, nullptr, nullptr);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (module) FreeLibrary(module);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void Require(VkResult result, const char *operation)
{
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}

template <typename T> static T Function(PFN_vkVoidFunction function, const char *name)
{
    if (!function) throw std::runtime_error(std::string("Missing Vulkan entry: ") + name);
    return reinterpret_cast<T>(function);
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t **argv)
{
    fs::path layer_path;
    bool validate_dispatch = false;
    bool system_layers = false;
    bool registered_provider = false;
    bool register_provider_after_device = false;
    bool test_acquire_failure = false, test_recreation_failure = false;
    fs::path output = fs::absolute(fs::path(argv[0])).parent_path() / L"gpu-probe.json";
    for (int index = 1; index < argc; ++index)
    {
        const std::wstring option = argv[index];
        if (option == L"--layer" && index + 1 < argc) layer_path = fs::absolute(argv[++index]);
        if (option == L"--output" && index + 1 < argc) output = fs::absolute(argv[++index]);
        if (option == L"--validate-lower-dispatch") validate_dispatch = true;
        if (option == L"--system-layers") system_layers = true;
        if (option == L"--registered-input-provider") registered_provider = true;
        if (option == L"--register-input-provider-after-device") { registered_provider = true; register_provider_after_device = true; }
        if (option == L"--fail-native-present-acquire") test_acquire_failure = true;
        if (option == L"--fail-native-recreation") test_recreation_failure = true;
    }
    nlohmann::json report = {{"mode", "gpu-layer-probe"}, {"process_bits", sizeof(void *) * 8},
        {"frame_generation_active", false}, {"generated_frames", 0}, {"success", false},
        {"system_loader_layer_chain", system_layers}};
    HMODULE vulkan = nullptr, layer = nullptr;
    HWND window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<VkSemaphore> render_semaphores;
    PFN_vkGetInstanceProcAddr get_instance = nullptr;
    PFN_vkGetDeviceProcAddr get_device = nullptr;
    uint64_t registration = 0;
    PFN_AsteriaUnregisterFrameInputProviderV1 unregister_provider = nullptr;
    PFN_AsteriaQueryFrameInputTicketV1 query_ticket = nullptr;
    try
    {
        fs::create_directories(output.parent_path());
        if ((test_acquire_failure || test_recreation_failure) && !registered_provider)
            throw std::runtime_error("Native failure fixture requires a registered provider");
        FailNativeRecreation = test_recreation_failure;
        CrashDumpPath = output.parent_path() / L"native-crash.mdmp";
        SetUnhandledExceptionFilter(&RecordNativeCrash);
        if (system_layers && !layer_path.empty()) throw std::runtime_error("Choose either system layers or a direct layer path");
        const fs::path layer_report = output.parent_path() / L"layer-probe.json";
        SetEnvironmentVariableW(L"ASTERIA_FG_PROBE_OUTPUT", layer_report.c_str());
        SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", L"1");
        if (!system_layers) SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", nullptr);
        else
        {
            wchar_t names[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", names, 32768);
            if (!length || length >= 32768) throw std::runtime_error("System layer mode requires VK_INSTANCE_LAYERS");
            report["requested_system_layers"] = fs::path(names).u8string();
        }
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        const fs::path loader_path = fs::path(system) / L"vulkan-1.dll";
        vulkan = LoadLibraryExW(loader_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!vulkan) throw std::runtime_error("System Vulkan loader unavailable");
        const auto native_instance = Function<PFN_vkGetInstanceProcAddr>(reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")), "vkGetInstanceProcAddr");
        get_instance = native_instance;
        if (!layer_path.empty())
        {
            layer = LoadLibraryW(layer_path.c_str());
            if (!layer) throw std::runtime_error("Frame-generation probe layer unavailable");
            const auto negotiate = Function<PFN_vkNegotiateLoaderLayerInterfaceVersion>(reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(layer, "vkNegotiateLoaderLayerInterfaceVersion")), "vkNegotiateLoaderLayerInterfaceVersion");
            VkNegotiateLayerInterface negotiation{};
            negotiation.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
            negotiation.loaderLayerInterfaceVersion = 2;
            Require(negotiate(&negotiation), "layer negotiation");
            get_instance = negotiation.pfnGetInstanceProcAddr;
            get_device = negotiation.pfnGetDeviceProcAddr;
            if (!get_instance || !get_device) throw std::runtime_error("Layer dispatch interface unavailable");
        }
        PFN_AsteriaRegisterFrameInputProviderV1 register_provider = nullptr;
        if (registered_provider)
        {
            if (!layer) throw std::runtime_error("Registered provider fixture requires a direct layer");
            register_provider = reinterpret_cast<PFN_AsteriaRegisterFrameInputProviderV1>(GetProcAddress(layer, "AsteriaRegisterFrameInputProviderV1"));
            unregister_provider = reinterpret_cast<PFN_AsteriaUnregisterFrameInputProviderV1>(GetProcAddress(layer, "AsteriaUnregisterFrameInputProviderV1"));
            query_ticket = reinterpret_cast<PFN_AsteriaQueryFrameInputTicketV1>(GetProcAddress(layer, "AsteriaQueryFrameInputTicketV1"));
            if (!register_provider || !unregister_provider || !query_ticket || !GetProcAddress(layer, "AsteriaSubmitFrameInputsV1"))
                throw std::runtime_error("Versioned frame input C exports unavailable");
            if (!register_provider_after_device)
            {
                AsteriaFrameInputRegistrationV1 info{sizeof(info), ASTERIA_FG_INPUT_API_V1, 3};
                if (register_provider(&info, &registration) != 0) throw std::runtime_error("Frame input registration failed");
                report["frame_input_provider_registered"] = true;
                report["frame_input_provider_registered_after_device"] = false;
                report["frame_input_tickets"] = nlohmann::json::array();
                report["input_images_submitted"] = 0;
            }
        }
        const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Asteria FG GPU Probe";
        application.apiVersion = VK_API_VERSION_1_1;
        VkLayerInstanceLink instance_link{};
        instance_link.pfnNextGetInstanceProcAddr = native_instance;
        VkLayerInstanceCreateInfo instance_chain{};
        instance_chain.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
        instance_chain.function = VK_LAYER_LINK_INFO;
        instance_chain.u.pLayerInfo = &instance_link;
        VkInstanceCreateInfo create_instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create_instance.pNext = layer ? &instance_chain : nullptr;
        create_instance.pApplicationInfo = &application;
        create_instance.enabledExtensionCount = 2;
        create_instance.ppEnabledExtensionNames = extensions;
        Require(Function<PFN_vkCreateInstance>(get_instance(nullptr, "vkCreateInstance"), "vkCreateInstance")(&create_instance, nullptr, &instance), "create instance");
        const HINSTANCE app_instance = GetModuleHandleW(nullptr);
        WNDCLASSW window_class{};
        window_class.hInstance = app_instance;
        window_class.lpfnWndProc = WindowProc;
        window_class.lpszClassName = L"AsteriaFgGpuProbeWindow";
        RegisterClassW(&window_class);
        window = CreateWindowExW(0, window_class.lpszClassName, L"Asteria FG Probe", WS_POPUP, 0, 0, 640, 360, nullptr, nullptr, app_instance, nullptr);
        if (!window) throw std::runtime_error("Probe window creation failed");
        VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface_info.hinstance = app_instance;
        surface_info.hwnd = window;
        Require(Function<PFN_vkCreateWin32SurfaceKHR>(get_instance(instance, "vkCreateWin32SurfaceKHR"), "vkCreateWin32SurfaceKHR")(instance, &surface_info, nullptr, &surface), "create surface");
        const auto enumerate = Function<PFN_vkEnumeratePhysicalDevices>(get_instance(instance, "vkEnumeratePhysicalDevices"), "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        Require(enumerate(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> physicals(count);
        Require(enumerate(instance, &count, physicals.data()), "enumerate devices");
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        uint32_t family = 0;
        const auto get_families = Function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get_instance(instance, "vkGetPhysicalDeviceQueueFamilyProperties"), "vkGetPhysicalDeviceQueueFamilyProperties");
        const auto present_support = Function<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(get_instance(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"), "vkGetPhysicalDeviceSurfaceSupportKHR");
        for (const auto candidate : physicals)
        {
            uint32_t families_count = 0;
            get_families(candidate, &families_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(families_count);
            get_families(candidate, &families_count, families.data());
            for (uint32_t index = 0; index < families_count; ++index)
            {
                VkBool32 present = VK_FALSE;
                Require(present_support(candidate, index, surface, &present), "surface support");
                if (present && (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                {
                    physical = candidate;
                    family = index;
                    report["queue_family"] = family;
                    report["queue_capacity"] = families[index].queueCount;
                    report["graphics_queue_compute"] = bool(families[index].queueFlags & VK_QUEUE_COMPUTE_BIT);
                    break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("No graphics/presentation queue available");
        VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &subgroup;
        Function<PFN_vkGetPhysicalDeviceProperties2>(get_instance(instance, "vkGetPhysicalDeviceProperties2"), "vkGetPhysicalDeviceProperties2")(physical, &properties);
        report["gpu_name"] = properties.properties.deviceName;
        report["vulkan_api"] = properties.properties.apiVersion;
        report["subgroup_size"] = subgroup.subgroupSize;
        report["subgroup_compute"] = bool(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT);
        report["subgroup_operations"] = subgroup.supportedOperations;
        report["nondispatchable_handle_bits"] = sizeof(VkImage) * 8;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        const auto native_device = Function<PFN_vkGetDeviceProcAddr>(native_instance(instance, "vkGetDeviceProcAddr"), "native vkGetDeviceProcAddr");
        NativeProbeDevice = native_device;
        VkLayerDeviceLink device_link{};
        device_link.pfnNextGetInstanceProcAddr = native_instance;
        device_link.pfnNextGetDeviceProcAddr = (test_acquire_failure || test_recreation_failure) ? &ProbeNextDevice : native_device;
        VkLayerDeviceCreateInfo device_chain{};
        device_chain.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
        device_chain.function = VK_LAYER_LINK_INFO;
        device_chain.u.pLayerInfo = &device_link;
        VkDeviceCreateInfo create_device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create_device.pNext = layer ? &device_chain : nullptr;
        create_device.queueCreateInfoCount = 1;
        create_device.pQueueCreateInfos = &queue_info;
        create_device.enabledExtensionCount = 1;
        create_device.ppEnabledExtensionNames = device_extensions;
        Require(Function<PFN_vkCreateDevice>(get_instance(instance, "vkCreateDevice"), "vkCreateDevice")(physical, &create_device, nullptr, &device), "create device");
        if (register_provider_after_device)
        {
            AsteriaFrameInputRegistrationV1 info{sizeof(info), ASTERIA_FG_INPUT_API_V1, 3};
            if (register_provider(&info, &registration) != 0) throw std::runtime_error("Post-device frame input registration failed");
            report["frame_input_provider_registered"] = true;
            report["frame_input_provider_registered_after_device"] = true;
            report["frame_input_tickets"] = nlohmann::json::array();
            report["input_images_submitted"] = 0;
        }
        if (!get_device) get_device = native_device;
        const auto device_function = [&](const char *name) { return get_device(device, name); };
        VkQueue queue = VK_NULL_HANDLE;
        Function<PFN_vkGetDeviceQueue>(device_function("vkGetDeviceQueue"), "vkGetDeviceQueue")(device, family, 0, &queue);
        if (validate_dispatch)
        {
            const HMODULE validation_layer = layer ? layer : GetModuleHandleW(L"AsteriaFrameGenerationLayer.dll");
            if (!validation_layer) throw std::runtime_error("Lower-dispatch validation requires the Asteria layer");
            using Validate = VkResult (VKAPI_CALL *)(VkDevice, VkQueue);
            const auto validate = reinterpret_cast<Validate>(GetProcAddress(validation_layer, "AsteriaValidateNextDispatch"));
            if (!validate) throw std::runtime_error("Lower-dispatch validation entry unavailable");
            Require(validate(device, queue), "saved next dispatch");
            report["saved_next_dispatch_verified"] = true;
        }
        const auto surface_caps = Function<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(get_instance(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        const auto surface_formats = Function<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(get_instance(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        uint32_t formats_count = 0;
        Require(surface_formats(physical, surface, &formats_count, nullptr), "surface formats");
        std::vector<VkSurfaceFormatKHR> formats(formats_count);
        Require(surface_formats(physical, surface, &formats_count, formats.data()), "surface formats");
        if (formats.empty()) throw std::runtime_error("No surface format available");
        auto format = formats.front();
        for (const auto candidate : formats)
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM) { format = candidate; break; }
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        Require(Function<PFN_vkCreateCommandPool>(device_function("vkCreateCommandPool"), "vkCreateCommandPool")(device, &pool_info, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        Require(Function<PFN_vkAllocateCommandBuffers>(device_function("vkAllocateCommandBuffers"), "vkAllocateCommandBuffers")(device, &allocate, &command), "command buffer");
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        const auto create_semaphore = Function<PFN_vkCreateSemaphore>(device_function("vkCreateSemaphore"), "vkCreateSemaphore");
        Require(create_semaphore(device, &semaphore_info, nullptr, &acquired), "acquire semaphore");
        const auto create_swapchain = Function<PFN_vkCreateSwapchainKHR>(device_function("vkCreateSwapchainKHR"), "vkCreateSwapchainKHR");
        const auto destroy_swapchain = Function<PFN_vkDestroySwapchainKHR>(device_function("vkDestroySwapchainKHR"), "vkDestroySwapchainKHR");
        const auto get_images = Function<PFN_vkGetSwapchainImagesKHR>(device_function("vkGetSwapchainImagesKHR"), "vkGetSwapchainImagesKHR");
        const auto acquire_image = Function<PFN_vkAcquireNextImageKHR>(device_function("vkAcquireNextImageKHR"), "vkAcquireNextImageKHR");
        const auto acquire_image_2 = Function<PFN_vkAcquireNextImage2KHR>(device_function("vkAcquireNextImage2KHR"), "vkAcquireNextImage2KHR");
        const auto begin = Function<PFN_vkBeginCommandBuffer>(device_function("vkBeginCommandBuffer"), "vkBeginCommandBuffer");
        const auto end = Function<PFN_vkEndCommandBuffer>(device_function("vkEndCommandBuffer"), "vkEndCommandBuffer");
        const auto barrier = Function<PFN_vkCmdPipelineBarrier>(device_function("vkCmdPipelineBarrier"), "vkCmdPipelineBarrier");
        const auto clear = Function<PFN_vkCmdClearColorImage>(device_function("vkCmdClearColorImage"), "vkCmdClearColorImage");
        const auto submit = Function<PFN_vkQueueSubmit>(device_function("vkQueueSubmit"), "vkQueueSubmit");
        const auto present = Function<PFN_vkQueuePresentKHR>(device_function("vkQueuePresentKHR"), "vkQueuePresentKHR");
        const auto idle = Function<PFN_vkQueueWaitIdle>(device_function("vkQueueWaitIdle"), "vkQueueWaitIdle");
        const auto reset = Function<PFN_vkResetCommandBuffer>(device_function("vkResetCommandBuffer"), "vkResetCommandBuffer");
        uint32_t present_count = 0;
        uint64_t previous_generation = 0;
        for (unsigned cycle = 0; cycle < 2; ++cycle)
        {
            if (cycle) SetWindowPos(window, nullptr, 0, 0, 512, 288, SWP_NOACTIVATE | SWP_NOZORDER);
            VkSurfaceCapabilitiesKHR caps{};
            Require(surface_caps(physical, surface, &caps), "surface capabilities");
            if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) throw std::runtime_error("Surface lacks transfer-destination usage");
            VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            info.surface = surface;
            info.minImageCount = (std::max)(caps.minImageCount, 2u);
            if (caps.maxImageCount) info.minImageCount = (std::min)(info.minImageCount, caps.maxImageCount);
            info.imageFormat = format.format;
            info.imageColorSpace = format.colorSpace;
            info.imageExtent = caps.currentExtent;
            if (info.imageExtent.width == UINT32_MAX) info.imageExtent = {cycle ? 512u : 640u, cycle ? 288u : 360u};
            info.imageArrayLayers = 1;
            info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = caps.currentTransform;
            info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            info.clipped = VK_TRUE;
            info.oldSwapchain = swapchain;
            VkSwapchainKHR next = VK_NULL_HANDLE;
            const auto create_result = create_swapchain(device, &info, nullptr, &next);
            if (test_recreation_failure && cycle == 1)
            {
                if (create_result != VK_ERROR_OUT_OF_DEVICE_MEMORY || next != VK_NULL_HANDLE || NativeCreateAttempts != 2 || NativeCreateFailures != 1)
                    throw std::runtime_error("Native recreation failure was retried, lost, or published dirty output");
                uint32_t unused = UINT32_MAX;
                if (acquire_image(device, swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &unused) != VK_ERROR_OUT_OF_DATE_KHR || unused != UINT32_MAX)
                    throw std::runtime_error("Retired old proxy still allowed image acquisition");
                report["injected_native_create_failure_propagated"] = true;
                report["dirty_failed_create_output_not_owned"] = true;
                report["old_proxy_acquire_retired"] = true;
                break;
            }
            Require(create_result, "create swapchain");
            if (swapchain) destroy_swapchain(device, swapchain, nullptr);
            swapchain = next;
            uint64_t generation = 0;
            if (registered_provider)
            {
                AsteriaFrameInputTicketV1 ticket{sizeof(ticket), ASTERIA_FG_INPUT_API_V1};
                if (query_ticket(registration, reinterpret_cast<uintptr_t>(device), uint64_t(swapchain), &ticket) != ASTERIA_FG_INPUT_UNAVAILABLE)
                    throw std::runtime_error("Input ticket must remain unavailable before a successful acquire");
            }
            uint32_t image_count = 0;
            Require(get_images(device, swapchain, &image_count, nullptr), "get swapchain images");
            std::vector<VkImage> images(image_count);
            Require(get_images(device, swapchain, &image_count, images.data()), "get swapchain images");
            std::vector<VkSemaphore> finished(image_count, VK_NULL_HANDLE);
            for (auto &semaphore : finished)
            {
                Require(create_semaphore(device, &semaphore_info, nullptr, &semaphore), "render semaphore");
                render_semaphores.push_back(semaphore);
            }
            for (unsigned frame = 0; frame < 30; ++frame)
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                uint32_t index = 0;
                if (frame % 2)
                {
                    VkAcquireNextImageInfoKHR acquire_info{VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR};
                    acquire_info.swapchain = swapchain;
                    acquire_info.timeout = UINT64_MAX;
                    acquire_info.semaphore = acquired;
                    acquire_info.deviceMask = 1;
                    Require(acquire_image_2(device, &acquire_info, &index), "acquire image 2");
                }
                else Require(acquire_image(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index), "acquire image");
                const VkSemaphore rendered = finished.at(index);
                Require(reset(command, 0), "reset command buffer");
                VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                Require(begin(command, &begin_info), "begin command buffer");
                VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                transition.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                transition.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                transition.image = images[index];
                transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);
                VkClearColorValue color{{0.03f + frame * 0.001f, 0.07f, 0.15f, 1.0f}};
                clear(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &transition.subresourceRange);
                transition.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                transition.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                transition.dstAccessMask = 0;
                barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &transition);
                Require(end(command), "end command buffer");
                const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submission.waitSemaphoreCount = 1;
                submission.pWaitSemaphores = &acquired;
                submission.pWaitDstStageMask = &wait_stage;
                submission.commandBufferCount = 1;
                submission.pCommandBuffers = &command;
                submission.signalSemaphoreCount = 1;
                submission.pSignalSemaphores = &rendered;
                Require(submit(queue, 1, &submission, VK_NULL_HANDLE), "queue submit");
                VkPresentInfoKHR presentation{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                presentation.waitSemaphoreCount = 1;
                presentation.pWaitSemaphores = &rendered;
                presentation.swapchainCount = 1;
                presentation.pSwapchains = &swapchain;
                presentation.pImageIndices = &index;
                if (registered_provider)
                {
                    AsteriaFrameInputTicketV1 ticket{sizeof(ticket), ASTERIA_FG_INPUT_API_V1};
                    if (query_ticket(registration, reinterpret_cast<uintptr_t>(device), uint64_t(swapchain), &ticket) != 0 ||
                        ticket.frame_id != frame || ticket.image_index != index || ticket.acquire_id != frame + 1 ||
                        ticket.width != info.imageExtent.width || ticket.height != info.imageExtent.height)
                        throw std::runtime_error("Actual present input ticket mismatch");
                    if (frame == 0)
                    {
                        if (!ticket.generation || ticket.generation == previous_generation)
                            throw std::runtime_error("Actual FG swapchain generation mismatch");
                        generation = ticket.generation; previous_generation = generation;
                    }
                    if (ticket.generation != generation) throw std::runtime_error("Swapchain generation changed within a live swapchain");
                    report["frame_input_tickets"].push_back({{"generation", generation}, {"frame_id", ticket.frame_id},
                        {"swapchain", uint64_t(swapchain)}, {"width", ticket.width}, {"height", ticket.height},
                        {"image_index", ticket.image_index}, {"acquire_id", ticket.acquire_id}});
                }
                const bool injecting = test_acquire_failure && cycle == 0 && frame == 10;
                if (injecting) FailNativeAcquire = true;
                const auto present_result = present(queue, &presentation);
                if (injecting)
                {
                    if (present_result != VK_ERROR_OUT_OF_DATE_KHR || NativeAcquireFailures != 1)
                        throw std::runtime_error("SDK native acquire failure was not propagated by QueuePresent");
                    report["injected_native_acquire_failure_propagated"] = true;
                }
                else { Require(present_result, "queue present"); ++present_count; }
                Require(idle(queue), "queue idle");
                Sleep(17);
            }
        }
        report["presented_real_frames"] = present_count;
        report["swapchain_creations"] = test_recreation_failure ? 1 : 2;
        report["layer_loaded"] = layer != nullptr || GetModuleHandleW(L"AsteriaFrameGenerationLayer.dll") != nullptr;
        report["reshade_layer_loaded"] = GetModuleHandleW(L"ReShade32.dll") != nullptr;
        report["success"] = present_count == (test_recreation_failure ? 30u : test_acquire_failure ? 59u : 60u);
    }
    catch (const std::exception &error)
    {
        report["error"] = error.what();
        std::cerr << error.what() << std::endl;
        std::ofstream(output, std::ios::binary | std::ios::trunc) << report.dump(2);
    }
    if (device && get_device)
    {
        Function<PFN_vkDeviceWaitIdle>(get_device(device, "vkDeviceWaitIdle"), "vkDeviceWaitIdle")(device);
        const auto destroy_semaphore = reinterpret_cast<PFN_vkDestroySemaphore>(get_device(device, "vkDestroySemaphore"));
        if (acquired) destroy_semaphore(device, acquired, nullptr);
        for (const auto semaphore : render_semaphores) destroy_semaphore(device, semaphore, nullptr);
        if (swapchain) reinterpret_cast<PFN_vkDestroySwapchainKHR>(get_device(device, "vkDestroySwapchainKHR"))(device, swapchain, nullptr);
        if (pool) reinterpret_cast<PFN_vkDestroyCommandPool>(get_device(device, "vkDestroyCommandPool"))(device, pool, nullptr);
        reinterpret_cast<PFN_vkDestroyDevice>(get_device(device, "vkDestroyDevice"))(device, nullptr);
    }
    if (registration && unregister_provider)
    {
        const auto result = unregister_provider(registration);
        report["frame_input_provider_unregistered"] = result == ASTERIA_FG_INPUT_OK;
        if (result != 0) report["success"] = false;
    }
    if (surface) reinterpret_cast<PFN_vkDestroySurfaceKHR>(get_instance(instance, "vkDestroySurfaceKHR"))(instance, surface, nullptr);
    if (instance) reinterpret_cast<PFN_vkDestroyInstance>(get_instance(instance, "vkDestroyInstance"))(instance, nullptr);
    if (window) DestroyWindow(window);
    if (layer) FreeLibrary(layer);
    if (vulkan) FreeLibrary(vulkan);
    std::ofstream(output, std::ios::binary | std::ios::trunc) << report.dump(2);
    std::cout << report.dump(2) << std::endl;
    return report.value("success", false) ? 0 : 1;
}
