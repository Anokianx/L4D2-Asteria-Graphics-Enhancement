# Asteria

**Asteria** is an explicit 32-bit (`x86`) Vulkan graphics enhancement, real-time telemetry, and **AMD FidelityFX™ FSR 3 Frame Generation** runtime engineered for *Left 4 Dead 2* (`shaderapivk.dll` / DXVK).

Source Engine games on 32-bit Windows do not natively output per-pixel motion vectors, expose modern frame-generation hooks, or support 64-bit-only upscaling/interpolation SDKs. Asteria bridges that gap entirely at the Vulkan layer and shader boundary—without patching game executables, replacing `d3d9.dll`, or writing to game process memory.

---

## Key Capabilities

### 1. FSR 3 Frame Generation for 32-Bit Vulkan (`VK_LAYER_Asteria_FrameGeneration`)
- **x86 Port of AMD FidelityFX SDK v1.1.4**: Ports AMD's Frame Interpolation (`v1.1.3`), Optical Flow (`v1.1.2`), Vulkan backend, and replacement swapchain presenter to 32-bit `x86`.
- **64-Bit Non-Dispatchable Handle Preservation**: Vulkan non-dispatchable handles (`VkImage`, `VkImageView`, `VkPipeline`, `VkSwapchainKHR`, `VkSemaphore`, etc.) are always 64-bit, even on 32-bit platforms where `void*` is 32-bit. Asteria wraps 64-bit Vulkan handles in typed handle boxes (`AsteriaVkHandles`) and proxy swapchain tokens across the FidelityFX SDK boundary.
- **True Depth Capture (`1x` / `2x` / `4x` / `8x` MSAA → `R32_SFLOAT`)**: Intercepts *Left 4 Dead 2*'s main-scene `VK_FORMAT_D24_UNORM_S8_UINT` depth attachment before clear and dispatches a compute pass (`ResolveDepth.comp` / `ResolveDepth1x.comp`) to resolve closest-surface depth into `VK_FORMAT_R32_SFLOAT`. Dynamically adapts on the fly when the in-game Anti-Aliasing setting changes.
- **Native Camera Recovery (`cViewProj` Decomposition)**: Samples the 64-byte row-major `cViewProj` matrix from mapped vertex shader constant buffers at descriptor set `2`, binding `0`, byte offset `384`. Mathematically decomposes the rigid camera basis (`origin`, `right`, `up`, `backward`), `nearPlane`, `farPlane`, and `verticalFovRadians`, while automatically rejecting 3D skybox (`sky32`) passes and orthographic VGUI/HUD draws.
- **GPU Camera Motion Reconstruction**: Reprojects resolved `R32_SFLOAT` depth through `inverseCurrentViewProjection` and `previousViewProjection` (`ReconstructMotion.comp`) using a zero-allocation 8-slot persistent mapped uniform and descriptor ring to synthesize per-pixel `VK_FORMAT_R32G32_SFLOAT` normalized UV motion vectors and `VK_FORMAT_R32_SFLOAT` confidence masks.
- **Optical Flow Assistance & Paced Presentation**: Combines reconstructed geometric camera motion and True Depth with AMD Optical Flow block vectors and scene-change detection (`SCD`), dispatching `ffxFrameInterpolationPrepare` and `ffxFrameInterpolationDispatch` into a QPC-paced replacement swapchain presenter.

### 2. Post-Processing Shader Suite (`VK_LAYER_Asteria_ReShade` & `Asteria.addon`)
Asteria hosts an isolated, explicit Vulkan ReShade runtime (`VK_LAYER_Asteria_ReShade`) controlled live through `Asteria.addon` without touching the game's installation directory:
- **SMAA (Subpixel Morphological Anti-Aliasing)**: Configurable edge-detection threshold and search quality (`LOW` / `MEDIUM` / `HIGH`).
- **CAS (Contrast Adaptive Sharpening)**: Tunable sharpening intensity with contrast adaptation.
- **Quarter-Resolution 4-Pass Bloom**: Highlight extraction followed by two separable Gaussian blur passes at quarter resolution.
- **Color Grading**: Live exposure, contrast, saturation, color temperature (cool/warm balance), and vibrance controls.
- **SDR Tone Mapping**: Selectable Filmic, ACES, and Reinhard tonal curves with adjustable curve mix.
- **Vignette & Edge-Preserving Deband**: Configurable vignette falloff and 4/8/12-sample static-pattern debanding that eliminates banding without introducing temporal noise.

### 3. Hardware Telemetry & Desktop Control Panel
- **Live Control Panel (`Asteria/panel.py`)**: Four dedicated control pages (`QUALITY`, `COLOR / LIGHT`, `PERFORMANCE`, `RUNTIME`) with 100 ms coalesced live updates to the running game.
- **Draggable In-Game HUD (`Asteria/telemetry.py`)**: Displays real-time presented FPS, mean frame interval, P95 frame time, 1% Low FPS, process CPU utilization, working-set RAM, and live FSR 3 pipeline status (`LIVE · FSR 3 FG`).
- **Adapter-Matched VRAM Accounting**: Matches the active Vulkan device LUID to the native system DXGI adapter and queries 64-bit `D3DKMT` segment capacities and per-process `LOCAL` / `NON_LOCAL` video memory usage and WDDM budgets.
- **Dynamic In-Game Video Synchronization**: Reads `left4dead2/cfg/video.txt` offline and queries `VMaterialSystem080` (`MaterialSystem_Config_t`) live at runtime to display and synchronize resolution, window mode (`Fullscreen`, `Borderless`, `Windowed`), MSAA sample count, anisotropic filtering, and VSync state.

---

## Architecture Overview

```text
Left 4 Dead 2 (left4dead2.exe -vulkan -> shaderapivk.dll / DXVK 2.3.1)
  │
  ▼
VK_LAYER_Asteria_FrameGeneration (AsteriaFrameGenerationLayer.dll)
  ├── Descriptor & Mapped Buffer Tracker
  │     └── Reads VS cViewProj (Set 2, Binding 0, Offset 384) -> RecoverNativePerspective()
  ├── Dynamic Rendering & Depth Interceptor
  │     └── Snapshots 1x/2x/4x/8x MSAA D24S8 depth before clear -> ResolveDepth.spv (R32F)
  ├── GPU Camera Motion Reconstruction
  │     └── ReconstructMotion.spv (Current/Prev R32F Depth + ViewProj -> RG32F UV Motion)
  ├── AMD FidelityFX Optical Flow (v1.1.2) + Frame Interpolation (v1.1.3)
  │     └── Optical Flow 8x8 Vectors + SCD + FI Prepare & Dispatch -> Interpolated Frame
  └── FFX Replacement Swapchain Presenter (FrameInterpolationSwapchainVK)
        └── QPC Frame Pacer (Presents Interpolated + Real frames at 2x engine rate)
  │
  ▼
VK_LAYER_Asteria_ReShade (ReShade32.dll + Asteria.addon)
  ├── Executes enabled post-processing passes (SMAA, CAS, Bloom, Color, Tonemap, Vignette, Deband)
  └── Collects Present timestamps, DXGI/D3DKMT VRAM budgets, and publishes runtime telemetry
  │
  ▼
Vulkan ICD / GPU Driver
```

---

## Repository Layout

```text
Asteria/
├── __init__.py
├── effects.py                        # Post-processing shader catalog & parameter definitions
├── panel.py                          # PySide6 desktop control panel (Quality, Color, Performance, Runtime)
├── runtime.py                        # Runtime state manager, IPC config writer, & telemetry reader
├── telemetry.py                      # Draggable transparent telemetry HUD overlay
├── Shader/
│   ├── Effects/                      # HLSL/ReShade effect files (SMAA, CAS, Bloom, Color, Tonemap, Vignette, Deband)
│   ├── Includes/                     # Upstream shader headers
│   └── Textures/                     # SMAA area and search lookup textures
└── Runtime/
    ├── AsteriaLoader.cpp             # Standalone CLI launcher & explicit Vulkan layer orchestrator (Asteria.exe)
    ├── AsteriaReShadeAddon.cpp       # ReShade add-on driving shader uniforms & hardware telemetry (Asteria.addon)
    ├── Host/                         # Pinned 32-bit ReShade Vulkan layer binary & manifest
    ├── ReShadeHost/                  # Runtime INI templates
    └── FrameGeneration/              # x86 Vulkan FSR 3 Frame Generation pipeline
        ├── Port/                     # 32-bit port of AMD FidelityFX SDK v1.1.4 backend, presenter, & 408 SPIR-V shaders
        ├── Layer/                    # VK_LAYER_Asteria_FrameGeneration, LiveSceneMotionRuntime, & contract tests
        ├── DepthCapture/             # 1x/2x/4x/8x MSAA D24S8 snapshot & R32F closest-depth compute resolve
        ├── FrameAssociation/         # cViewProj perspective decomposition, sky32 rejection, & frame association
        ├── MotionReconstruction/     # GPU depth + camera motion vector reconstruction compute pipeline
        ├── EstimatedMotion/          # Bidirectional optical flow filtering & confidence estimation
        ├── SceneInputs/              # Cross-DLL GPU scene interpolation probes
        ├── LiveInputs/               # Producer-consumer frame coordination & snapshot contracts
        ├── InputProbe/               # Read-only engine camera, pipeline provenance, & uniform discovery probes
        └── Research/                 # Pinned SDK sources, shader profiles, & architectural notes
```

---

## Building from Source

### Prerequisites
- Windows 10 / 11 (x64 host with 32-bit `x86` target toolchain)
- Visual Studio 2022 (MSVC `v143` x86 C++ build tools & Windows 10/11 SDK)
- Vulkan SDK (with `glslangValidator` for SPIR-V compilation)
- Python 3.10+ (`PySide6` for the desktop control panel)

### 1. Build the Asteria Launcher & ReShade Add-on
From `Asteria/Runtime`:
```powershell
py -3 Prepare_ReShadeHost.py
.\Build_RuntimeStub.bat
.\Build_AsteriaLoader.bat
.\Build_AsteriaReShadeAddon.bat
```

### 2. Build the FSR 3 Frame Generation Layer & Test Suite
From `Asteria/Runtime/FrameGeneration`:
```powershell
.\Build_Prototype.bat
```
This compiles all 38 targets across `Port`, `Layer`, `DepthCapture`, `FrameAssociation`, `MotionReconstruction`, `EstimatedMotion`, `SceneInputs`, `LiveInputs`, and `InputProbe`, producing `Layer\bin\AsteriaFrameGenerationLayer.dll`.

### 3. Run Automated Validation & GPU Contract Tests
From `Asteria/Runtime/FrameGeneration`:
```powershell
.\Run_Prototype_Tests.bat
```
This executes the full unit, Vulkan layer contract, and GPU compute validation suite (including 64-bit handle boxing, lower-dispatch isolation, mapped buffer readback, swapchain lifecycle, MSAA depth capture/resolve, perspective camera recovery, GPU motion reconstruction, bidirectional optical flow filtering, and end-to-end FFX frame interpolation), writing verified artifact hashes to `validation.json` and `estimated-input-validation.json`.

---

## Usage

### Launching via the Control Panel
1. Run `START.bat` (or `py -3 Danyria.pyw`) and open the **Asteria** panel.
2. Enable **ASTERIA** in the top bar and configure desired post-processing effects under **QUALITY** and **COLOR / LIGHT**.
3. Open the **PERFORMANCE** tab to configure **FSR 3 FRAME GENERATION**:
   - **ENABLE**: Activates `VK_LAYER_Asteria_FrameGeneration` and the FFX replacement swapchain presenter.
   - **TRUE DEPTH (`1X` / `2X` / `4X` / `8X` MSAA)**: Captures and resolves the main-scene depth buffer before clear.
   - **MOTION RECONSTRUCTION**: Reconstructs per-pixel camera motion vectors on the GPU from resolved depth and `cViewProj` matrices.
   - **OPTICAL FLOW ASSIST**: Enables AMD Optical Flow block motion estimation and scene-change detection.
4. Click **START GAME WITH ASTERIA**.

### Launching via the Command Line
```powershell
.\Asteria\Runtime\bin\Asteria.exe --launch --game "C:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2\left4dead2.exe"
```
`Asteria.exe` automatically passes `-vulkan -novid -console -multirun`, configures `VK_LAYER_Asteria_FrameGeneration;VK_LAYER_Asteria_ReShade` in the child process environment, and cleans up any temporary explicit layer registration when the game exits.

---

## License & Third-Party Acknowledgments

- **AMD FidelityFX™ SDK v1.1.4** (`Frame Interpolation v1.1.3`, `Optical Flow v1.1.2`): Copyright © Advanced Micro Devices, Inc. Licensed under the MIT License (`Asteria/Runtime/FrameGeneration/Research/sdk-v1.1.4/LICENSE.txt`).
- **ReShade v6.7.2**: Copyright © Patrick Mours. Licensed under the BSD 3-Clause License (`Asteria/Runtime/Host/LICENSE.md`, `Asteria/Runtime/sdk/reshade/LICENSE.md`).
- **SweetFX / SMAA & CAS Shaders**: Copyright © CeeJayDK and contributors (`Asteria/Shader/Includes`).
- **Khronos Vulkan Headers**: Licensed under the Apache License 2.0 (`Asteria/Runtime/FrameGeneration/Layer/ThirdParty/Vulkan-Headers/LICENSE.md`).
