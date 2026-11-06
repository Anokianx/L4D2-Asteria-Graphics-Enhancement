# Vulkan backend x86 overlay

`sources/vk/ffx_vk.cpp` and `include/FidelityFX/host/backends/vk/ffx_vk.h` overlay the pinned FidelityFX SDK v1.1.4 files. Their original MIT notices remain intact. The research source tree is unchanged.

`Build_Backend.bat` compiles the backend and shared handle bridge, using the installed MSVC x86 compiler and normal Khronos v1.3.250 headers. Outputs are `build/x86/ffx_vk.obj` and `build/x86/AsteriaFGBackend.lib`; compiler diagnostics are saved in `build/x86/backend-build.log`. The static library must be linked with the official FI/Optical Flow shader-blob accessors, SDK shared sources, the overlay's Vulkan dispatch shim (or a Vulkan loader import library) and frame-interpolation swapchain sources. Link the handle registry exactly once.

The overlay preserves `FfxResource.resource` and other CPU ABI pointer fields. Full 64-bit non-dispatchable handles are interned by the shared `asteria_vk` registry and decoded with the active backend's device and expected handle kind. No Vulkan handle-width macro is forced.

- `ffxGetResourceVK` accepts an already boxed pointer. `ffxGetImageResourceVK` and `ffxGetBufferResourceVK` accept an explicit device plus a typed native image/buffer and produce the correctly boxed resource.
- Dynamic external registrations borrow their existing boxes, validate device/type and preserve the caller's ownership of the GPU object. They do not retire external boxes at frame unregistration.
- Internal resource exports keep a stable box in their resource slot and retire it when the owning SDK resource is destroyed. The uniform buffer keeps its own box until the backend's final context is released.
- Compute pipeline creation, binding and destruction use typed pipeline boxes. Root-signature fields remain pointers to actual backend host objects, and indirect-command signature fields retain their existing stride sentinel.
- Breadcrumb GPU buffer and device-memory fields are boxed as separate handle kinds. Their own free operation retires those boxes.
- `ffxGetSwapchainVK` recognizes registered proxy tokens, including retired tokens. An inactive proxy returns null instead of being re-registered as a native swapchain. Native swapchains are boxed using the previously registered `VkDeviceContext` device; without that device, registration returns null. `ffxGetVKSwapchain` returns the native/proxy token through the shared bridge, never through pointer casts.
- Vulkan custom replacement-function typedefs use `VKAPI_CALL` under x86. Integer Vulkan nulls and 64-bit diagnostic format arguments are corrected.

The caller must finish submitted GPU use before destroying SDK contexts/resources or retiring the device. This overlay does not acquire game resources, create generated frames on its own or make a second presentation call. An independent GPU-compute probe must validate shader execution and image results before game integration.

The external-resource state contract is unchanged: `FFX_RESOURCE_STATE_COMPUTE_READ` corresponds to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`, and `FFX_RESOURCE_STATE_UNORDERED_ACCESS` corresponds to `VK_IMAGE_LAYOUT_GENERAL`. Unregistration restores each resource's state supplied at registration. External image descriptions must provide the actual mip/layer dimensions and `FFX_RESOURCE_USAGE_UAV` when storage views are required.

The pinned FI Prepare implementation divides `motionVectorScale` by the selected motion-vector target dimensions. For normalized UV motion vectors, pass the target width/height; for pixel motion vectors, pass 1/1. The official Optical Flow output contains pixel displacements, so FI uses `opticalFlowScale = {1 / displayWidth, 1 / displayHeight}`, `opticalFlowBlockSize = 8`, and the shared flow resource dimensions returned by the SDK. Physical-device capability queries in this upstream backend can select FP16 or KHR operations based on supported extensions, so the caller must actually enable the corresponding features/extensions on its logical device.

The final overlay, including the retired-proxy guard, was rebuilt successfully for x86 on 2026-10-14. This establishes compilation; GPU execution is validated separately by the interpolation probe.
