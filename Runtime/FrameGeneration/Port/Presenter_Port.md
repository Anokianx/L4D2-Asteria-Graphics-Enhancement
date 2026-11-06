# AMD Vulkan presenter x86 port

`Build_Presenter.bat` compiles the four preserved AMD swapchain sources with MSVC x86, C++17 and static CRT, then produces `build/x86/AsteriaFGPresenter.lib`. The build log is `build/x86/presenter-build.log`. This overlay is based on the immutable SDK source under `../Research/sdk-v1.1.4`; upstream license headers are preserved.

## ABI and resource lifetime

- Vulkan replacement entry points use `VKAPI_CALL`, including the custom create-swapchain and present-count callbacks. AMD host callback types keep their original calling convention.
- Every external image resource is boxed through `AsteriaVkHandles`. Swapchains use full 64-bit native handles or typed proxy tokens; x86 pointer truncation is not used. Calls reject expired tokens, foreign devices and a present queue that does not belong to the proxy.
- Construction initializes the four critical sections before any failing Vulkan operation. Destruction closes their lifetime after draining pending presents and stopping the presenter thread.
- Creation failure preserves the original swapchain until native replacement creation was attempted. An attempted `vkCreateSwapchainKHR` retires its `oldSwapchain` even when replacement creation fails; the old wrapper is consequently destroyed and the caller's FFX handle cleared.
- A copy of the allocation callbacks follows the wrapper and its UI replacement image. The wrapper's destroy function uses that copy, including when replacing an older wrapper created with different callbacks.
- The initial duplicate allocation of frame-rendered semaphores is removed. Each actual swapchain image gets one semaphore. Swapchain count queries return the number of images actually written.
- Win32 pacing events are unnamed and belong to the individual wrapper. Callback failure clears the generated-frame count, and the game-queue composition branch transfers ownership to the correct queue family. The native present count is atomic on x86.

## Link dependencies

Link the presenter library with the patched `ffx_vk.obj`, `AsteriaVkHandles.obj`, `VulkanShim.obj`, the official frame-interpolation and optical-flow CPU libraries and `Shaders/build/x86/AsteriaFGShaders.lib`. Normal Win32 `kernel32.lib` and `user32.lib` are sufficient for the presenter host calls. Vulkan functions are supplied by the isolated prototype dispatch shim rather than a Vulkan SDK import library.

The four archive members are:

1. `FrameInterpolationSwapchainVK.obj`
2. `FrameInterpolationSwapchainVK_UiComposition.obj`
3. `FrameInterpolationSwapchainVK_DebugPacing.obj`
4. `FrameInterpolationSwapchainVK_Helpers.obj`

## Independent presentation test

Use one small independent Win32 surface and a Vulkan device with timeline semaphores enabled. Request four distinct queues from a graphics/compute/present-capable family when available: indices 0, 1, 2 and 3 for game, async compute, present and image acquisition. The async queue is optional when async workloads are disabled; game, present and acquisition queues must be separate. Sharing a family avoids queue-family transfer complexity while retaining the SDK's required separation between submission queues.

1. Fill `VkFrameInterpolationInfoFFX`, preserving device, physical-device and queue ownership. Call `ffxGetSwapchainReplacementFunctionsVK`, then `createSwapchainFFX` with the ordinary surface create info and this FFX extension. Retain the returned proxy token and obtain its FFX box with `asteria_vk::ProxyBox`.
2. Obtain the replacement game images through `getSwapchainImagesKHR`. Acquire with `acquireNextImageKHR`, render/upload the deterministic input into the returned image, and transition it to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` before the SDK game present. These replacement images are sampled inputs, unlike the native presentable images. Never send the proxy token to a native driver function.
3. Set `FfxFrameGenerationConfig` for every frame with the proxy FFX box, increasing `frameID`, full-screen interpolation rectangle and `frameGenerationEnabled=true`. Start with `allowAsyncWorkloads=false`, no UI resource and a frame-generation callback.
4. The callback receives an already recording command buffer, `presentColor`, one owned output UAV, reset state and the same frame ID. Record the real OF, Prepare and FI dispatches into that command buffer using the verified external depth/motion resources. Use `desc.outputs[0]` as the FI destination and `desc.presentColor` as its color source. Return `FFX_OK` only after every dispatch succeeded. The SDK owns command-buffer end and queue submission.
5. Submit the game's rendering, then call `queuePresentKHR` through the replacement function table with the proxy token and render-completion semaphore. The SDK presents the generated image and real image using its separate queues and native swapchain.
6. Drain with `ffxWaitForPresents`, then wait for the expected CPU native-present calls to complete. The timeline semaphore can complete before the CPU presenter calls its final paced present. Compare `getLastPresentCountFFX` against the actual successful and failed native calls recorded by the shim; the SDK's count alone includes failed attempts. Independently read back generated images so a copied frame cannot qualify as frame generation. Warm-up/reset frames must be reported separately.
7. Toggle frame generation off through the config, test a resize/recreation and repeat. Drain again, destroy the proxy with the replacement destroy function, verify its old token no longer resolves and release presenter caches with `AsteriaReleaseUiCompositionResourcesVK` and `AsteriaReleaseDebugPacingResourcesVK`. Then destroy FI/OF contexts and remaining resources, retire device boxes and destroy the Vulkan device. These cache release functions require all presenter work on that device to be stopped and drained; they must not be called while another swapchain uses the same global cache.

`ffxGetFrameinterpolationCommandlistVK` is an alternative to the callback: it returns an SDK-owned recording command buffer after frame generation is enabled. Record commands and let the SDK submit it; do not end or submit it independently. The callback path is simpler because the SDK records the required output barriers before calling it.

## Verified independent presentation

`Build_PresentationProbe.bat` builds `build/x86/AsteriaPresentationProbe.exe`. Running it from the Port directory writes `results/presentation/presentation-probe.json` and raw before/generated/after PPM images for both resolutions.

The probe ran successfully on the NVIDIA GeForce RTX 4080 SUPER in a 32-bit process. It submitted 22 game presents: 20 frames with generation enabled and two disabled frames. The shim recorded 42 actual successful native presents and zero failures. Three reset frames correctly copied the current input; every one of the remaining 17 generated frames placed the moving object exactly halfway between inputs that moved by 12 pixels. Each verified output differed from both real source frames.

Turning generation off removed both the callback and extra native present. Re-enabling generation reset its temporal state. The test recreated the window, swapchain, FI/OF contexts and shared textures from 640 × 360 to 512 × 288 and verified seven further intermediate frames at the smaller size. Cleanup stopped and drained the presenter, invalidated both retired proxy tokens, released the global composition/debug caches and destroyed all owned resources before the device.

These results establish actual SDK image generation and native presentation in the isolated synthetic-input test. They do not establish pacing performance or game integration. The isolated shim loads native dispatch for one prototype device. Production integration must route the SDK's internal native Vulkan calls through the saved lower-layer dispatch so generated presents do not recurse through the Asteria/ReShade upper layers.
