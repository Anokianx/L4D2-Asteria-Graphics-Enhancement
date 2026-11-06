# Minimum correct x86 Vulkan portability plan

This is the original source assessment of FidelityFX SDK v1.1.4. The pinned AMD/Khronos files remain unchanged. The separate `../Port` overlay now applies the backend/presenter port and has passed actual x86 GPU interpolation tests. The goal is to preserve every 64-bit Vulkan handle and keep its lifetime distinct from x86 host pointers; the remaining production-integration contracts below still apply.

## ABI boundary

Vulkan dispatchable handles such as `VkDevice`, `VkQueue` and `VkCommandBuffer` are pointers on x86. Non-dispatchable `VkImage`, `VkBuffer`, `VkPipeline`, `VkSwapchainKHR`, etc. remain 64-bit values. `FfxResource.resource`, `FfxPipeline` and `FfxSwapchain` are `void*`, therefore 32-bit in the target game. A port must define typed, stable host boxes or a device-scoped handle registry for the non-dispatchable values. The pointer in the existing FFX CPU interface can identify that box; it must never contain truncated handle bits.

The upstream ABI definitions can remain unchanged if the new private x86 backend consistently interprets these pointers as boxes. Mixing a patched backend with an unpatched API/provider or exchanging those boxes with an official x64 DLL is invalid. Every call site must use the same representation.

## Resource and pipeline changes

| Source/API boundary | Required work |
| --- | --- |
| `ffxGetResourceVK` and API `FfxApiResource` conversion | Add typed image/buffer entry points or an explicit box parameter. Carry the complete handle, device, resource type and lifetime token. Preserve resource description/state. |
| `RegisterResourceVK`, dynamic-resource duplicate lookup and `GetResourceVK` | Decode the box rather than casting its pointer into `VkImage`/`VkBuffer`; return stable boxes for internal resources. Identity checks compare full 64-bit handle, device and type. |
| `RegisterStaticResourceVK`, bindless descriptor writers and resource-copy/barrier paths | Decode resource handles consistently. Keep descriptor views and registered external resources alive through every submitted GPU use. |
| Constant-buffer allocator and `FfxConstantAllocation` | Box the allocator's real `VkBuffer`; decode it at descriptor binding. Keep the existing 64-bit offset field intact and synchronize ring reuse. |
| `ffxGetPipelineVK`, pipeline creation, bind and destruction | Box full `VkPipeline` values and decode them at `vkCmdBindPipeline`/`vkDestroyPipeline`. `rootSignature` currently points to a real backend `PipelineLayout` host object and should stay a host pointer. `cmdSignature` is currently a small stride sentinel; handle it explicitly rather than treating it as a Vulkan object. |
| Breadcrumb buffers and GPU debug paths | Their `void*` buffer fields have the same issue. Either port the full-handle paths or explicitly exclude that optional feature from the initial backend. |
| Integer-handle nulls and diagnostic format strings | Use `VK_NULL_HANDLE` for non-dispatchable handle arguments and correct 64-bit formatting. These repairs do not resolve the larger lifetime/ABI problem by themselves. |

A box that merely borrows a game texture does not own the actual texture. Unregistering a frame resource or deleting its box must not call `vkDestroyImage` on game-owned images. Internally allocated GPU resources and external registrations need separate ownership flags and destruction paths.

Boxes copied into queued `FfxGpuJobDescription`/frame records must remain stable. They cannot live on a temporary caller stack or be erased at function return. Use device/context-owned pools and retire them only after the owning GPU fence/timeline and all interpolation/composition/presentation tasks complete. Guard against a reused Vulkan numeric handle belonging to a new resource after device reset or swapchain recreation.

## Swapchain representation and ownership

The official `FfxSwapchain` representation uses a pointer-shaped value for two different objects: an original real `VkSwapchainKHR`, then a `FrameInterpolationSwapChainVK*` wrapper. Those cases require an explicit typed state on x86.

1. Preserve the original real `VkSwapchainKHR` as a complete 64-bit member. The SDK wrapper remains an actual x86 host pointer.
2. `ffxGetSwapchainVK`/`ffxGetVKSwapchain`, the frame-generation API VK provider and `ffxReplaceSwapchainForFrameinterpolationVK` must distinguish original and wrapped swapchains. A device-scoped synthetic 64-bit token can identify a wrapper at the application's Vulkan boundary, with a validated token-to-wrapper map. Never pass that synthetic token to the next Vulkan implementation as a real native handle.
3. The public replacement functions for create/destroy/get-images/acquire/present/HDR metadata must decode wrapped tokens and delegate original real handles appropriately. `VkSwapchainCreateInfoKHR.oldSwapchain` needs the same translation.
4. During replacement, upstream retires/destroys the old real swapchain even if new initialization fails. A port needs explicit success/failure ownership rules before enabling that operation in a game; retain the original allocator callbacks and ensure every wrapper/native object is destroyed exactly once.
5. Interpolation output, real swapchain images and UI composition resources must use the new resource boxes. All image barrier/copy/presentation/composition code currently casts `FfxResource.resource` to `VkImage` and needs the corresponding decoding.
6. Wait for pacing/presenter threads and GPU submissions before destroying wrappers, their synchronization objects, command pools or boxes. Apply the same teardown order on resize, out-of-date swapchains, device lost and master-switch shutdown.

## Function calling conventions

All functions assigned to standard `PFN_vk*` function pointers need `VKAPI_CALL` on both declaration and definition under x86 Windows. This covers at least the destroy, acquire, get-images, queue-present and HDR-metadata replacement functions and their provider wrappers. Keep the custom FFX callback convention aligned with its own typedefs. Fixing only the assignment with a cast would hide a stack-cleanup mismatch.

## Build and validation sequence

1. Keep the pinned upstream source tree/hash manifests immutable. Apply any future port in a separate source overlay and record the exact diff.
2. Compile the x86 CPU core, corrected VK backend and provider with normal Vulkan headers; do not force the x64 non-dispatchable-handle macro.
3. Test high-bit fake handles such as `0x12345678ABCDEF01` through every box encode/decode route and test stale/wrong-device/wrong-type rejection. This is a host ABI test, not a GPU frame-generation claim.
4. Generate and validate the official FI/Optical Flow SPIR-V permutations using the official compiler pipeline. Link only the required effects for the first backend.
5. Create an independent x86 Vulkan device/swapchain test with known depth, motion, camera and separate UI. Confirm GPU writes, interpolation image correctness, frame-ID/reset behavior and resource-state transitions before integrating with DXVK.
6. Validate acquire/present semaphore consumption, wrapper/native image indexing, queue-family transfers, real/generated cadence and pacing. A count of two `Present` calls or doubled status text is insufficient evidence.
7. Validate resize/alt-tab/device-lost and shutdown without leaks, deadlocks or destroying externally owned textures. Only then evaluate obtaining the game's real motion/depth/UI/camera data and enabling a game-facing FG option.

The successful official x86 CPU probe establishes that source portability can begin. The original x86 Vulkan compiler failures establish that the existing official backend is not directly usable in this game. Neither result establishes GPU interpolation or in-game generated frames.
