# Retail L4D2 camera ABI evidence

Author: Anokianx  
Date: 2026-10-15

`Inspect_Retail_Camera_ABI.py` reads the installed `bin/engine.dll` as a file. It does not load the DLL, start the game, execute an engine function, inspect another process, or modify game files. Capstone 5.0.9 is installed only under this directory's `tools` folder, with its original license. The semantic report refuses any file whose SHA256 differs from the audited Windows x86 binary.

Run `py -3 Asteria/Runtime/FrameGeneration/Research/NativeBinary/Inspect_Retail_Camera_ABI.py` from the project directory. The generated `retail-camera-abi.json` contains the source hash, PE header, RTTI complete-object locators, vtables, interface registration constructors, instruction bytes and disassembly. The binary's hash is checked again after analysis.

## Confirmed mismatch

The installed engine has SHA256 `8a6d468059394bf4cfce32f11dcba12ef45de88da1373084d5a2916d0bf7ba86`, PE timestamp `0x6a440ed6`, and image size `0x73f000`. Its `VEngineClient013` registration returns the `CEngineClient` object at RVA `0x425504`, whose vtable is at RVA `0x359dec`.

The [pinned L4D2 SDK declaration](https://github.com/alliedmodders/hl2sdk/blob/2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6/public/cdll_int.h) has no `HideLoadingPlaque` entry after `IsDrawingLoadingImage`. The retail vtable includes this operation at slot 29. Its machine code references two explicit `HideLoadingPlaque` warning strings. The following methods therefore occur one slot later in this binary:

| Method | SDK slot | Retail slot | Retail function RVA |
| --- | ---: | ---: | --- |
| Sound_ExtraUpdate | 34 | 35 | `0x5d550` |
| GetGameDirectory | 35 | 36 | `0x5d610` |
| WorldToScreenMatrix | 36 | 37 | `0x5d620` |
| WorldToViewMatrix | 37 | 38 | `0x5d630` |

The old slot 36 returns the address of a game-directory character buffer. Treating its return value as 16 floats cannot validate camera input. The old slot 37 would return the world-to-screen matrix while the caller labeled it world-to-view. The unchanged SDK interface string did not guarantee compatibility with this retail layout.

## Complete matrix getter paths

The retail matrix methods load the `CRender` object through global pointer RVA `0x427bc0`. Its initial object RVA is `0x601f00`; RTTI associates its vtable at RVA `0x3725b4` with `CRender`.

World-to-screen dispatches to renderer slot 13 at RVA `0xc3160`. World-to-view dispatches to renderer slot 12 at RVA `0xc3130`. Both complete bodies consist only of loads, a comparison, a conditional branch, multiplication, address calculation and returns. There are no calls, stores, loops or lock instructions in these getter bodies.

The engine's matrix-copy routine at RVA `0x27ce80` individually copies 16 scalar float32 values at offsets `0x00` through `0x3c`. Its multiplication routine at RVA `0x27d4e0` writes those same 16 output elements. The multiplication's input strides and first-element formula establish row-major 4×4 storage. The view generator composes `world_to_screen = view_to_projection * world_to_view` with that routine. This is binary evidence for the copied memory layout, rather than a guess based solely on the SDK type name.

When the renderer's view-stack count at offset `0xe8` exceeds one, the getters select the most recent entry of a stack with stride `0x194`, then return its matrix at offset `0x150` or `0xd0`. Otherwise, they return retained matrices at renderer offsets `0x9c` or `0x1c`. These paths establish the calling convention and matrix source; they do not establish whether a callback observes a main view, another view, concurrently changing data, or a retained matrix from a prior frame.

`IsInGame` and `IsConnected` still occupy slots 26 and 27. Their shared accessor only loads a client-state pointer, adds eight, and returns; each query then reads the state field and compares it with its expected threshold.

## Screen-size call is a different path

`GetScreenSize` still occupies slot 5, but its code invokes material-system slot 96 to obtain a rendering context, then calls that context's slots 2, 103, 3 and 1. Those dynamic implementations are outside this engine-only analysis. Their thread safety at the Vulkan `Present` callback is unverified. This report does not claim that the earlier stall was caused by a particular getter, but it establishes that sampling matrices need not invoke this rendering-context path. Diagnostic image dimensions can instead come from the observed backbuffer.

## Additional view-interface boundary

The same retail engine also differs from the [pinned IVRenderView declaration](https://github.com/alliedmodders/hl2sdk/blob/2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6/public/ivrenderview.h). Its `GetFieldOfView` occupies slot 30, while SDK slot 28 would call an operation returning the view entity integer. Its `GetMatricesForView` occupies slot 53, while SDK slot 50 would dispatch elsewhere.

The matrix generator consumes an existing native view structure. Observed offsets include width `0x10`, height `0x18`, FOV `0x34`, origin `0x3c`, angles `0x48`, near plane `0x54`, far plane `0x58` and aspect ratio `0x64`. They differ from the pinned `CViewSetup`. Calling this method with the SDK structure would be invalid. Computing matrices from an invented structure would also fail to confirm the actual final camera.

The perspective constructor at RVA `0x27ea10` sets `P[3][2]=-1`, `P[3][3]=0`, `P[2][2]=far/(near-far)` and `P[2][3]=far*near/(near-far)`. With validated, stable native matrices, their projection can be recovered as `P = world_to_screen * inverse(world_to_view)`. For this verified perspective structure, near is `P[2][3]/P[2][2]`, far is `P[2][3]/(P[2][2]+1)`, and horizontal/vertical FOV are `2*atan(1/P[0][0])` and `2*atan(1/P[1][1])`. This recovers engine-provided projection data rather than assuming a camera FOV. Invalid, orthographic, unstable or mismatched matrices must be rejected before applying these formulas.

The next diagnostic can use the guarded retail matrix slots without `GetScreenSize` or `GetViewAngles` calls. It must retain `native_camera_ready=false` until matrix values and their rendering-frame association are verified. A native main-view producer and frame boundary are still required for final-camera confirmation. This analysis supplies no native motion-vector source.
