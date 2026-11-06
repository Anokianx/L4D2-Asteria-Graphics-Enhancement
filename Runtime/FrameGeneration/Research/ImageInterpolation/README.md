# Image interpolation input research

Author: Anokianx

Research date: 2026-10-16. This directory is excluded from the production build. It contains source inspection evidence and an implementation plan, not an enabled frame-generation implementation.

## Decision

Estimated motion is now permitted by the project requirement. Constant depth and unconditional zero motion remain invalid substitutes for missing game inputs. A stationary scene can legitimately have zero estimated motion, so qualification must check the provider, frame identity and resource validity rather than reject every zero vector.

The practical route is a qualified estimated-input provider for the existing Vulkan interpolation backend. It must acquire real scene depth and consecutive color frames, use the verified camera data where applicable, estimate object/image motion, measure confidence, and retain the original frame when the inputs cannot support a useful result. Pure image interpolation is a separate candidate that does not require manufactured depth.

## AMD implementation facts

The selected SDK is [FidelityFX SDK 1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55), containing Frame Interpolation 1.1.3 and Optical Flow 1.1.2.

The [Frame Interpolation interface](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/host/ffx_frameinterpolation.h) accepts depth and motion vectors in its prepare stage. Its dispatch consumes prepared depth, prepared game motion, optical flow and camera parameters. The algorithm has separate game-motion and optical-flow fields and uses depth for disocclusion decisions. The optical-flow branch does not remove those inputs from the selected FI algorithm. Originating motion in an estimator is technically possible if its current-to-previous semantics and scale are correct; it is not an AMD-certified substitute for game motion, and quality needs independent validation. See the [official FI manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/).

The [Optical Flow 1.1.2 manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/) documents one color input with retained history, an `R16G16_SINT` vector per 8 by 8 block, and scene-change output. It uses a luminance pyramid and block matching. The documented maximum tracking movement is 512 pixels. These are block estimates rather than per-object, per-pixel engine motion. The selected source is already locally compiled for x86, so it has lower integration cost than adding a neural inference runtime.

The previous frame's stored flow is not the reverse flow for the current frame pair. A genuine forward/backward consistency test requires independent reverse estimation for that same pair. Reversing input order through a separate reset context would add work and must be benchmarked before adoption.

## RIFE and NCNN findings

The [RIFE NCNN Vulkan implementation](https://github.com/nihui/rife-ncnn-vulkan/tree/a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7) is pinned at `a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7`. Its own NCNN submodule points to `b4ba207c18d3103d6df890c0e3a97b469b196b26`. RIFE implements intermediate image synthesis from two color images without requiring scene depth. The public `RIFE::process` takes CPU `ncnn::Mat` inputs/output. Its Vulkan path uploads the inputs, downloads the result, and calls `submit_and_wait`. The checked-in [Windows CI](https://github.com/nihui/rife-ncnn-vulkan/blob/a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7/.github/workflows/CI.yml) builds x64. This is not evidence of a tested x86 Vulkan build or low-latency game integration.

[Current NCNN source](https://github.com/Tencent/ncnn/tree/c15904bb8918187ec100f1ebe708942af83b84be) was inspected independently at `c15904bb8918187ec100f1ebe708942af83b84be`; it is not a tested pairing with that RIFE revision. Its public `VulkanDevice` constructor accepts a device index and its implementation creates a Vulkan device. It does not offer a public constructor adopting the existing DXVK device. The [build guide](https://github.com/Tencent/ncnn/blob/c15904bb8918187ec100f1ebe708942af83b84be/docs/how-to-build/how-to-build.md) includes x86 CPU targets; those do not establish x86 Vulkan support for the complete RIFE application.

No model, inference binary, driver or SDK was installed. There is no local RIFE model or NCNN runtime elsewhere in the workspace search. Only 13 small source/license/document files were retrieved for inspection. Third-party notices are preserved verbatim under `Upstream`.

## Implementation sequence

1. Remove placeholder inputs from the game path. Keep original presentation active whenever a qualified input provider is unavailable. Keep synthetic resources confined to named standalone fixtures.
2. Add a provider contract carrying input provenance, color/depth dimensions, sample count, frame IDs, motion direction/scale, camera state, reset state and HUD handling. A pointer to a resource is insufficient evidence of validity.
3. Obtain main-scene depth before it is cleared or reused. Resolve actual multisampled depth with an explicitly tested method. Do not reinterpret an unrelated shadow/viewmodel buffer as main depth. Retain the matched current color and camera state.
4. Produce static-scene camera motion from actual depth and consecutive verified cameras. Use AMD Optical Flow estimates for image/object motion only where confidence supports them. Depth-aware upsampling can protect discontinuities but cannot recover hidden geometry or turn an 8 by 8 block estimate into exact object motion.
5. Before presentation integration, benchmark translation, rotation, moving objects, occlusion boundaries, disocclusion, particles, muzzle flashes, transparency, HUD and scene cuts. Evaluate midpoint position, image error, rejected coverage and GPU duration. Fall back on scene cuts and invalid history. Do not count repeated original frames as generated frames.
6. Qualify HUD separation and pacing independently. UI masks or HUD-less composition are preferable to interpolating text. Interpolation adds processing and temporal delay; it does not increase the game's simulation or input sampling rate.
7. Evaluate RIFE in an independent process only if the lightweight route cannot meet image quality. Start with a bounded lower-resolution GPU benchmark and licensed model. A game path would need a real GPU-resource interop design, ownership and synchronization rules, x86 memory accounting or an x64 helper, and latency measurements. CPU round trips and synchronous inference inside `Present` are not an acceptable default design.

No implementation can promise perfect inference for newly revealed surfaces, textureless or repeating regions, independently animated transparency, or large discontinuous motion. A trustworthy runtime must report estimated provenance and rejected frames, retain original rendering when necessary, and avoid advertising unverified quality or performance.

## Evidence check

Run `py -3 Verify_Evidence.py` from this directory or by absolute path. It verifies all retrieved source hashes and the inspected API/transfer/build facts, then writes `verification.json`. Passing this check confirms the recorded source evidence; it does not compile NCNN/RIFE, benchmark inference, capture a game or qualify production FG.
