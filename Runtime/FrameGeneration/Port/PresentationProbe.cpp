#define ASTERIA_INTERPOLATION_PROBE_LIBRARY
#include "InterpolationProbe.cpp"
#include "sources/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK_UiComposition.h"
#include "sources/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK_DebugPacing.h"

struct WindowSurface
{
    Gpu& gpu;
    HWND window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    WindowSurface(Gpu& gpu) : gpu(gpu)
    {
        WNDCLASSW klass{};
        klass.lpfnWndProc = DefWindowProcW;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"AsteriaPresentationProbe";
        RegisterClassW(&klass);
        window = CreateWindowExW(0, klass.lpszClassName, L"Asteria SDK Presentation Probe", WS_POPUP,
            0, 0, Width, Height, nullptr, nullptr, klass.hInstance, nullptr);
        if (!window) throw std::runtime_error("CreateWindowExW failed");
        VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        info.hinstance = klass.hInstance;
        info.hwnd = window;
        Check(vkCreateWin32SurfaceKHR(gpu.instance, &info, nullptr, &surface), "vkCreateWin32SurfaceKHR");
    }
    ~WindowSurface()
    {
        if (surface) vkDestroySurfaceKHR(gpu.instance, surface, nullptr);
        if (window) DestroyWindow(window);
    }
    void Resize(uint32_t width, uint32_t height)
    {
        if (!SetWindowPos(window, nullptr, 0, 0, int(width), int(height), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
            throw std::runtime_error("SetWindowPos failed");
        MSG message{};
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
};

template<class T> T NativeInstanceFunction(Gpu& gpu, const char* name)
{
    auto result = reinterpret_cast<T>(vkGetInstanceProcAddr(gpu.instance, name));
    if (!result) throw std::runtime_error(std::string("Unavailable instance function: ") + name);
    return result;
}

void ImageTransition(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

std::vector<uint8_t> SurfaceScene(uint32_t width, uint32_t height, uint32_t frame, std::vector<float>& depth, std::vector<uint16_t>& motion)
{
    std::vector<uint8_t> color(size_t(width) * height * 4);
    depth.assign(size_t(width) * height, 0.8f);
    motion.assign(size_t(width) * height * 2, 0);
    const int left = 64 + MotionPixels * int(frame);
    const uint32_t top = height / 4;
    const uint32_t bottom = height * 3 / 4;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t index = size_t(y) * width + x;
            const bool object = int(x) >= left && int(x) < left + 72 && y >= top && y < bottom;
            const uint8_t checker = uint8_t(((x / 24 + y / 24) % 2) * 20);
            color[index * 4] = object ? 240 : 20 + checker;
            color[index * 4 + 1] = object ? uint8_t(100 + ((x - left) / 8 % 2) * 35) : 28 + checker;
            color[index * 4 + 2] = object ? 32 : 52 + checker;
            color[index * 4 + 3] = 255;
            if (object)
            {
                depth[index] = 0.45f;
                motion[index * 2] = Half(frame ? -float(MotionPixels) / width : 0.0f);
            }
        }
    return color;
}

double SurfaceCentroid(uint32_t width, uint32_t height, const std::vector<uint8_t>& bytes)
{
    double sum = 0;
    size_t count = 0;
    for (uint32_t y = height / 4; y < height * 3 / 4; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t index = (size_t(y) * width + x) * 4;
            if (bytes[index] > 180 && bytes[index + 1] > 60 && bytes[index + 1] < 180 && bytes[index + 2] < 100)
            {
                sum += x;
                ++count;
            }
        }
    return count ? sum / count : -1;
}

void RgbaToBgra(std::vector<uint8_t>& bytes)
{
    for (size_t index = 0; index < bytes.size(); index += 4) std::swap(bytes[index], bytes[index + 2]);
}

void SaveSurfaceImage(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& image)
{
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot write surface image");
    stream << "P6\n" << width << ' ' << height << "\n255\n";
    for (size_t index = 0; index < image.size(); index += 4) stream.write(reinterpret_cast<const char*>(image.data() + index), 3);
    stream.flush();
    if (!stream) throw std::runtime_error("Cannot finish writing surface image");
}

class Phase
{
public:
    Gpu& gpu;
    WindowSurface& window;
    uint32_t width;
    uint32_t height;
    uint32_t sceneFrame = 0;
    uint64_t frameID = 0;
    uint32_t callbackCount = 0;
    bool callbackReset = false;
    bool bgra = false;
    FfxErrorCode callbackError = FFX_OK;
    std::string callbackErrorMessage;
    std::vector<uint8_t> scratch;
    std::unique_ptr<Effects> effects;
    Image* depth = nullptr;
    Image* motion = nullptr;
    Image* reconstructed = nullptr;
    Image* dilatedDepth = nullptr;
    Image* dilatedMotion = nullptr;
    Image* vectors = nullptr;
    Image* scd = nullptr;
    HostBuffer& upload;
    HostBuffer& readback;
    VkSwapchainKHR token = VK_NULL_HANDLE;
    FfxSwapchain box = nullptr;
    FfxSwapchainReplacementFunctions replacement{};
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    std::vector<VkImage> gameImages;
    std::vector<bool> gameImagesInitialized;
    std::filesystem::path outputDirectory;
    bool cleaned = false;

    Phase(Gpu& gpu, WindowSurface& window, uint32_t width, uint32_t height, HostBuffer& upload, HostBuffer& readback, const std::filesystem::path& outputDirectory)
        : gpu(gpu), window(window), width(width), height(height), upload(upload), readback(readback), outputDirectory(outputDirectory) {}

    ~Phase()
    {
        try { Cleanup(); }
        catch (const std::exception& error) { std::cerr << "Phase cleanup: " << error.what() << std::endl; }
    }

    void Cleanup()
    {
        if (cleaned) return;
        if (token)
        {
            FfxFrameGenerationConfig config{};
            config.swapChain = box;
            CheckFfx(ffxSetFrameGenerationConfigToSwapchainVK(&config), "ffxSetFrameGenerationConfigToSwapchainVK(cleanup)");
            CheckFfx(ffxWaitForPresents(box), "ffxWaitForPresents(cleanup)");
            Check(vkDeviceWaitIdle(gpu.device), "vkDeviceWaitIdle(cleanup)");
            replacement.destroySwapchainKHR(gpu.device, token, nullptr);
            const auto retired = token;
            token = VK_NULL_HANDLE;
            box = nullptr;
            if (asteria_vk::ProxyBox(gpu.device, retired) || asteria_vk::ProxyObject(gpu.device, retired))
                throw std::runtime_error("Destroyed SDK proxy still resolves");
        }
        if (!AsteriaReleaseUiCompositionResourcesVK(gpu.device) || !AsteriaReleaseDebugPacingResourcesVK(gpu.device))
            throw std::runtime_error("Presenter caches did not match the probe device");
        effects.reset();
        for (auto& image : gpu.images)
        {
            asteria_vk::Retire(asteria_vk::Image(gpu.device, image.handle));
            vkDestroyImage(gpu.device, image.handle, nullptr);
            vkFreeMemory(gpu.device, image.memory, nullptr);
        }
        gpu.images.clear();
        if (acquired) vkDestroySemaphore(gpu.device, acquired, nullptr);
        if (rendered) vkDestroySemaphore(gpu.device, rendered, nullptr);
        acquired = rendered = VK_NULL_HANDLE;
        cleaned = true;
    }

    void Initialize()
    {
        window.Resize(width, height);
        VkSurfaceCapabilitiesKHR capabilities{};
        auto getCaps = NativeInstanceFunction<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(gpu, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        Check(getCaps(gpu.physical, window.surface, &capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        if (capabilities.currentExtent.width != UINT32_MAX &&
            (capabilities.currentExtent.width != width || capabilities.currentExtent.height != height))
            throw std::runtime_error("Native surface size differs from requested phase size");
        uint32_t count = 0;
        auto getFormats = NativeInstanceFunction<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(gpu, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        Check(getFormats(gpu.physical, window.surface, &count, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        std::vector<VkSurfaceFormatKHR> formats(count);
        Check(getFormats(gpu.physical, window.surface, &count, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        auto format = std::find_if(formats.begin(), formats.end(), [](const auto& value) {
            return value.format == VK_FORMAT_R8G8B8A8_UNORM && value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (format == formats.end()) format = std::find_if(formats.begin(), formats.end(), [](const auto& value) {
            return value.format == VK_FORMAT_B8G8R8A8_UNORM && value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (format == formats.end()) throw std::runtime_error("No supported UNORM SDR surface format");
        bgra = format->format == VK_FORMAT_B8G8R8A8_UNORM;
        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = window.surface;
        create.minImageCount = std::max(2u, capabilities.minImageCount);
        if (capabilities.maxImageCount) create.minImageCount = std::min(create.minImageCount, capabilities.maxImageCount);
        create.imageFormat = format->format;
        create.imageColorSpace = format->colorSpace;
        create.imageExtent = {width, height};
        create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = capabilities.currentTransform;
        create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        create.clipped = VK_TRUE;
        VkFrameInterpolationInfoFFX info{};
        info.device = gpu.device;
        info.physicalDevice = gpu.physical;
        info.gameQueue = {gpu.queue, gpu.family, nullptr};
        for (auto entry : {std::pair<VkQueueInfoFFX*, uint32_t>{&info.asyncComputeQueue, 1}, {&info.presentQueue, 2}, {&info.imageAcquireQueue, 3}})
        {
            vkGetDeviceQueue(gpu.device, gpu.family, entry.second, &entry.first->queue);
            entry.first->familyIndex = gpu.family;
        }
        info.compositionMode = VK_COMPOSITION_MODE_PRESENT_QUEUE_FFX;
        VkDeviceContext deviceContext{gpu.device, gpu.physical, vkGetDeviceProcAddr};
        auto device = ffxGetDeviceVK(&deviceContext);
        CheckFfx(ffxGetSwapchainReplacementFunctionsVK(device, &replacement), "ffxGetSwapchainReplacementFunctionsVK");
        Check(replacement.createSwapchainFFX(gpu.device, &create, nullptr, &token, &info), "createSwapchainFFX");
        box = asteria_vk::ProxyBox(gpu.device, token);
        if (!box) throw std::runtime_error("Proxy box not found");
        count = 0;
        Check(replacement.getSwapchainImagesKHR(gpu.device, token, &count, nullptr), "getSwapchainImagesKHR");
        gameImages.resize(count);
        gameImagesInitialized.resize(count);
        Check(replacement.getSwapchainImagesKHR(gpu.device, token, &count, gameImages.data()), "getSwapchainImagesKHR");
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        Check(vkCreateSemaphore(gpu.device, &sem, nullptr, &acquired), "vkCreateSemaphore(acquired)");
        Check(vkCreateSemaphore(gpu.device, &sem, nullptr, &rendered), "vkCreateSemaphore(rendered)");
        scratch.resize(ffxGetScratchMemorySizeVK(gpu.physical, 2));
        FfxInterface backend{};
        CheckFfx(ffxGetInterfaceVK(&backend, device, scratch.data(), scratch.size(), 2), "ffxGetInterfaceVK");
        effects = std::make_unique<Effects>();
        effects->device = gpu.device;
        FfxFrameInterpolationContextDescription fi{};
        fi.maxRenderSize = fi.displaySize = {width, height};
        fi.backBufferFormat = fi.previousInterpolationSourceFormat = ffxGetSurfaceFormatVK(format->format);
        fi.backendInterface = backend;
        CheckFfx(ffxFrameInterpolationContextCreate(effects->interpolation.get(), &fi), "ffxFrameInterpolationContextCreate");
        effects->interpolationCreated = true;
        FfxOpticalflowContextDescription of{};
        of.resolution = {width, height};
        of.backendInterface = backend;
        CheckFfx(ffxOpticalflowContextCreate(effects->flow.get(), &of), "ffxOpticalflowContextCreate");
        effects->flowCreated = true;
        FfxFrameInterpolationSharedResourceDescriptions fiShared{};
        FfxOpticalflowSharedResourceDescriptions ofShared{};
        CheckFfx(ffxFrameInterpolationGetSharedResourceDescriptions(effects->interpolation.get(), &fiShared), "ffxFrameInterpolationGetSharedResourceDescriptions");
        CheckFfx(ffxOpticalflowGetSharedResourceDescriptions(effects->flow.get(), &ofShared), "ffxOpticalflowGetSharedResourceDescriptions");
        depth = &gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R32_FLOAT, width, height));
        motion = &gpu.CreateImage(Description(FFX_SURFACE_FORMAT_R16G16_FLOAT, width, height));
        reconstructed = &gpu.CreateImage(fiShared.reconstructedPrevNearestDepth.resourceDescription);
        dilatedDepth = &gpu.CreateImage(fiShared.dilatedDepth.resourceDescription);
        dilatedMotion = &gpu.CreateImage(fiShared.dilatedMotionVectors.resourceDescription);
        vectors = &gpu.CreateImage(ofShared.opticalFlowVector.resourceDescription);
        scd = &gpu.CreateImage(ofShared.opticalFlowSCD.resourceDescription);
        gpu.Begin();
        for (auto image : {reconstructed, dilatedDepth, dilatedMotion, vectors, scd}) gpu.Barrier(*image, VK_IMAGE_LAYOUT_GENERAL);
        gpu.Submit();
    }

    static FfxErrorCode Callback(const FfxFrameGenerationDispatchDescription* desc, void* data)
    {
        auto& phase = *static_cast<Phase*>(data);
        try
        {
            phase.Record(*desc);
            return FFX_OK;
        }
        catch (const std::exception& error)
        {
            phase.callbackError = FFX_ERROR_BACKEND_API_ERROR;
            phase.callbackErrorMessage = error.what();
            return phase.callbackError;
        }
    }

    void Record(const FfxFrameGenerationDispatchDescription& desc)
    {
        callbackReset = desc.reset;
        FfxOpticalflowDispatchDescription flow{};
        flow.commandList = desc.commandList;
        flow.color = desc.presentColor;
        flow.opticalFlowVector = gpu.Resource(*vectors, L"Optical flow", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        flow.opticalFlowSCD = gpu.Resource(*scd, L"Scene change", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        flow.reset = desc.reset;
        flow.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        flow.minMaxLuminance = {0.0f, 1.0f};
        CheckFfx(ffxOpticalflowContextDispatch(effects->flow.get(), &flow), "ffxOpticalflowContextDispatch");
        FfxFrameInterpolationPrepareDescription prepare{};
        prepare.commandList = desc.commandList;
        prepare.renderSize = {width, height};
        prepare.motionVectorScale = {float(width), float(height)};
        prepare.frameTimeDelta = 1000.0f / 60.0f;
        prepare.cameraNear = 0.1f;
        prepare.cameraFar = 100.0f;
        prepare.cameraFovAngleVertical = 1.0f;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.depth = gpu.Resource(*depth, L"Depth");
        prepare.motionVectors = gpu.Resource(*motion, L"Motion");
        prepare.frameID = desc.frameID;
        prepare.dilatedDepth = gpu.Resource(*dilatedDepth, L"Dilated depth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        prepare.dilatedMotionVectors = gpu.Resource(*dilatedMotion, L"Dilated motion", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        prepare.reconstructedPrevDepth = gpu.Resource(*reconstructed, L"Reconstructed depth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        prepare.cameraUp[1] = prepare.cameraRight[0] = prepare.cameraForward[2] = 1.0f;
        CheckFfx(ffxFrameInterpolationPrepare(effects->interpolation.get(), &prepare), "ffxFrameInterpolationPrepare");
        FfxFrameInterpolationDispatchDescription dispatch{};
        dispatch.commandList = desc.commandList;
        dispatch.displaySize = dispatch.renderSize = {width, height};
        dispatch.currentBackBuffer = desc.presentColor;
        dispatch.output = desc.outputs[0];
        dispatch.interpolationRect = {0, 0, int32_t(width), int32_t(height)};
        dispatch.opticalFlowVector = flow.opticalFlowVector;
        dispatch.opticalFlowSceneChangeDetection = flow.opticalFlowSCD;
        dispatch.opticalFlowBufferSize = {vectors->description.width, vectors->description.height};
        dispatch.opticalFlowScale = {1.0f / width, 1.0f / height};
        dispatch.opticalFlowBlockSize = 8;
        dispatch.cameraNear = prepare.cameraNear;
        dispatch.cameraFar = prepare.cameraFar;
        dispatch.cameraFovAngleVertical = prepare.cameraFovAngleVertical;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.frameTimeDelta = prepare.frameTimeDelta;
        dispatch.reset = desc.reset;
        dispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.minMaxLuminance[1] = 1.0f;
        dispatch.frameID = desc.frameID;
        dispatch.dilatedDepth = prepare.dilatedDepth;
        dispatch.dilatedMotionVectors = prepare.dilatedMotionVectors;
        dispatch.reconstructedPrevDepth = prepare.reconstructedPrevDepth;
        CheckFfx(ffxFrameInterpolationDispatch(effects->interpolation.get(), &dispatch), "ffxFrameInterpolationDispatch");
        VkCommandBuffer command = reinterpret_cast<VkCommandBuffer>(desc.commandList);
        VkImage outputImage = asteria_vk::Decode<VkImage>(desc.outputs[0].resource, gpu.device, asteria_vk::HandleKind::Image);
        if (!outputImage) throw std::runtime_error("Invalid SDK interpolation output box");
        ImageTransition(command, outputImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(command, outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &copy);
        ImageTransition(command, outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback.handle;
        host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        ++callbackCount;
    }

    json PresentFrame(bool enabled, uint64_t id)
    {
        frameID = id;
        std::vector<float> depthPixels;
        std::vector<uint16_t> motionPixels;
        auto current = SurfaceScene(width, height, sceneFrame, depthPixels, motionPixels);
        gpu.Upload(*depth, upload, depthPixels.data(), depthPixels.size() * sizeof(float));
        gpu.Upload(*motion, upload, motionPixels.data(), motionPixels.size() * sizeof(uint16_t));
        FfxFrameGenerationConfig config{};
        config.swapChain = box;
        config.frameGenerationEnabled = enabled;
        config.frameGenerationCallback = Callback;
        config.frameGenerationCallbackContext = this;
        config.allowAsyncWorkloads = false;
        config.frameID = id;
        config.interpolationRect = {0, 0, int32_t(width), int32_t(height)};
        CheckFfx(ffxSetFrameGenerationConfigToSwapchainVK(&config), "ffxSetFrameGenerationConfigToSwapchainVK");
        uint32_t index = 0;
        Check(replacement.acquireNextImageKHR(gpu.device, token, UINT64_MAX, acquired, VK_NULL_HANDLE, &index), "acquireNextImageKHR");
        auto nativeBytes = current;
        if (bgra) RgbaToBgra(nativeBytes);
        void* mapped = nullptr;
        Check(vkMapMemory(gpu.device, upload.memory, 0, nativeBytes.size(), 0, &mapped), "vkMapMemory(color upload)");
        std::memcpy(mapped, nativeBytes.data(), nativeBytes.size());
        vkUnmapMemory(gpu.device, upload.memory);
        gpu.Begin();
        ImageTransition(gpu.command, gameImages[index], gameImagesInitialized[index] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(gpu.command, upload.handle, gameImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ImageTransition(gpu.command, gameImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Check(vkEndCommandBuffer(gpu.command), "vkEndCommandBuffer(game upload)");
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &gpu.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered;
        Check(vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(game upload)");
        gameImagesInitialized[index] = true;
        const uint64_t before = replacement.getLastPresentCountFFX(token);
        const auto nativeBefore = AsteriaGetNativePresentStats();
        const uint32_t callbacksBefore = callbackCount;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &token;
        present.pImageIndices = &index;
        Check(replacement.queuePresentKHR(gpu.queue, &present), "queuePresentKHR(FFX)");
        CheckFfx(ffxWaitForPresents(box), "ffxWaitForPresents");
        const uint64_t expectedPresentCount = before + (enabled ? 2 : 1);
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (replacement.getLastPresentCountFFX(token) < expectedPresentCount && GetTickCount64() < deadline) Sleep(1);
        Check(vkDeviceWaitIdle(gpu.device), "vkDeviceWaitIdle(frame)");
        if (callbackError != FFX_OK) throw std::runtime_error(callbackErrorMessage);
        const uint64_t after = replacement.getLastPresentCountFFX(token);
        const auto nativeAfter = AsteriaGetNativePresentStats();
        json sample = {{"frame_id", id}, {"fg_enabled", enabled}, {"sdk_native_present_count", after - before},
            {"callback_count", callbackCount - callbacksBefore}, {"current_center_x", SurfaceCentroid(width, height, current)}, {"width", width}, {"height", height},
            {"native_successful_presents", nativeAfter.successful_calls - nativeBefore.successful_calls},
            {"native_failed_presents", nativeAfter.failed_calls - nativeBefore.failed_calls}};
        if (nativeAfter.successful_calls - nativeBefore.successful_calls != (enabled ? 2 : 1) || nativeAfter.failed_calls != nativeBefore.failed_calls)
            throw std::runtime_error("Actual native present calls did not complete successfully");
        if (enabled)
        {
            if (callbackCount != callbacksBefore + 1) throw std::runtime_error("Frame-generation callback not called exactly once");
            std::vector<uint8_t> generated(size_t(width) * height * 4);
            Check(vkMapMemory(gpu.device, readback.memory, 0, generated.size(), 0, &mapped), "vkMapMemory(generated readback)");
            std::memcpy(generated.data(), mapped, generated.size());
            vkUnmapMemory(gpu.device, readback.memory);
            if (bgra) RgbaToBgra(generated);
            const double currentCenter = SurfaceCentroid(width, height, current);
            const double generatedCenter = SurfaceCentroid(width, height, generated);
            const double expected = currentCenter - MotionPixels * 0.5;
            sample["reset"] = callbackReset;
            sample["generated_center_x"] = generatedCenter;
            sample["expected_center_x"] = expected;
            sample["pixels_different_from_current"] = DifferentPixels(current, generated);
            if (sceneFrame)
            {
                auto previous = SurfaceScene(width, height, sceneFrame - 1, depthPixels, motionPixels);
                sample["previous_center_x"] = SurfaceCentroid(width, height, previous);
                sample["pixels_different_from_previous"] = DifferentPixels(previous, generated);
                sample["intermediate_position_verified"] = !callbackReset && std::abs(generatedCenter - expected) < 1.0 &&
                    DifferentPixels(current, generated) > 500 && DifferentPixels(previous, generated) > 500;
                const std::string prefix = std::to_string(width) + "x" + std::to_string(height) + "_";
                SaveSurfaceImage(outputDirectory / (prefix + "previous.ppm"), width, height, previous);
                SaveSurfaceImage(outputDirectory / (prefix + "generated.ppm"), width, height, generated);
                SaveSurfaceImage(outputDirectory / (prefix + "current.ppm"), width, height, current);
            }
            else sample["intermediate_position_verified"] = false;
            if (after - before != 2) throw std::runtime_error("Enabled frame did not produce two native presents");
        }
        else if (after - before != 1 || callbackCount != callbacksBefore)
            throw std::runtime_error("Disabled frame did not produce one native present without interpolation");
        ++sceneFrame;
        return sample;
    }
};

int main(int argc, char** argv)
{
    const std::filesystem::path output = argc > 1 ? argv[1] : "results/presentation/presentation-probe.json";
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    json report = {{"process_bits", sizeof(void*) * 8}, {"synthetic_inputs", true}, {"game_frame_generation", false},
        {"hidden_independent_window", true}, {"success", false}, {"frames", json::array()}};
    std::string stage = "initialize_vulkan";
    int result = 1;
    try
    {
        Gpu gpu;
        gpu.Initialize(true);
        AsteriaResetNativePresentStats();
        report["gpu"] = gpu.properties.deviceName;
        WindowSurface window(gpu);
        auto& upload = gpu.CreateBuffer();
        auto& readback = gpu.CreateBuffer();
        uint64_t frameID = 0;
        uint32_t verified = 0;
        uint32_t actualNativePresents = 0;
        uint32_t generatedPresents = 0;
        uint32_t resetFrames = 0;
        uint32_t cleanedPhases = 0;
        for (uint32_t phaseIndex = 0; phaseIndex < 2; ++phaseIndex)
        {
            stage = "initialize_phase_" + std::to_string(phaseIndex);
            Phase phase(gpu, window, phaseIndex ? 512 : Width, phaseIndex ? 288 : Height, upload, readback, output.parent_path());
            phase.Initialize();
            const uint32_t frameCount = phaseIndex ? 8 : 14;
            for (uint32_t index = 0; index < frameCount; ++index)
            {
                const bool enabled = phaseIndex || index < 6 || index >= 8;
                stage = "present_frame_" + std::to_string(frameID);
                auto sample = phase.PresentFrame(enabled, frameID++);
                actualNativePresents += sample["sdk_native_present_count"].get<uint32_t>();
                generatedPresents += enabled ? 1 : 0;
                if (sample.value("intermediate_position_verified", false)) ++verified;
                if (sample.value("reset", false)) ++resetFrames;
                std::cout << "Frame " << sample["frame_id"] << ": " << sample.dump() << std::endl;
                report["frames"].push_back(sample);
            }
            phase.Cleanup();
            if (phase.cleaned) ++cleanedPhases;
        }
        report["game_present_calls"] = frameID;
        report["sdk_native_present_calls"] = actualNativePresents;
        report["scheduled_generated_presents"] = generatedPresents;
        report["verified_intermediate_frames"] = verified;
        report["reset_frames"] = resetFrames;
        report["fg_disable_enable_verified"] = true;
        report["resize_verified"] = true;
        report["complete_cleanup"] = cleanedPhases == 2 && gpu.images.empty();
        report["retired_proxy_rejection_verified"] = cleanedPhases == 2;
        report["presenter_cache_cleanup_verified"] = cleanedPhases == 2;
        auto nativeStats = AsteriaGetNativePresentStats();
        report["native_present_attempts"] = nativeStats.attempts;
        report["native_present_successful_calls"] = nativeStats.successful_calls;
        report["native_present_failed_calls"] = nativeStats.failed_calls;
        report["native_present_successful_swapchains"] = nativeStats.successful_swapchains;
        report["success"] = verified == generatedPresents - resetFrames && verified == 17 && resetFrames == 3 &&
            actualNativePresents == frameID + generatedPresents && nativeStats.successful_calls == actualNativePresents &&
            nativeStats.failed_calls == 0 && cleanedPhases == 2;
        if (!report["success"].get<bool>()) throw std::runtime_error("Presentation proof did not pass image and present-count validation");
        stage = "complete";
        result = 0;
    }
    catch (const std::exception& error)
    {
        report["error"] = error.what();
        report["success"] = false;
        std::cerr << stage << ": " << error.what() << std::endl;
    }
    report["stage"] = stage;
    std::ofstream stream(output);
    stream << report.dump(2) << '\n';
    stream.flush();
    if (!stream)
    {
        std::cerr << "Cannot finish writing presentation report: " << output.string() << std::endl;
        return 1;
    }
    std::cout << "Report: " << output.string() << std::endl;
    return result;
}
