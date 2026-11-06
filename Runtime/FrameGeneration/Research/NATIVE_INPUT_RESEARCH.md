# L4D2 Native Frame Generation Input Research

Author: Anokianx  
Date: 2026-10-16  
Scope: read-only installed-game inspection, pinned public references and isolated opt-in graphics diagnostics.

## Current integration policy, 2026-10-16

The project now accepts explicitly identified Engine or Estimated motion. The native-only requirement applied to the earlier investigation below; it no longer prevents an estimated-motion implementation. That change does not make the selected AMD FI implementation color-only, validate diagnostic resources, or justify placeholder depth/motion.

The experimental layer now requires a per-swapchain input session with qualified depth/motion resources, device/frame/dimension matches, finite camera and actual frame interval, resource state/lifetime, synchronization and HUD handling. No real game producer is implemented. Missing providers/sessions return `VK_ERROR_FEATURE_NOT_PRESENT` and preserve native rendering. Individual unavailable or invalid frames disable interpolation and reset history. The game presentation path no longer invokes engine getters or changes `freeaim.dll` lifetime; the guarded InputProbe camera path remains an explicit diagnostic.

[Image interpolation research](ImageInterpolation/README.md) records the practical estimated-input route and pinned RIFE/NCNN alternatives. Real main-scene depth and matching color/camera timing remain necessary for the selected FI path. Camera motion alone misses independently moving surfaces; block optical flow needs confidence, scene-cut and boundary handling. Pure image interpolation is a separate algorithm option whose inference cost, GPU sharing and latency have not been validated here. Neither a native velocity producer nor a qualified estimated producer is complete, so real game FG remains unavailable.

## Result

The inspected L4D2 resources do not establish an existing dense, per-pixel native motion-vector render target that Asteria can consume. The actual installed motion-blur shader has only one color sampler across all 15 compiled combinations. Its material supplies `_rt_FullFrameFB` and a four-component blur parameter, rather than a velocity texture. That particular effect cannot supply the engine motion vectors required by the requested integration.

This is a bounded negative finding, not proof that every internal L4D2 renderer path lacks motion vectors. Shader filenames, compiled reflection and printable strings do not enumerate unnamed runtime attachments or undocumented passes. The initial offline scan launched no game and read no process memory. The later, explicitly enabled graphics diagnostics below read cached native matrices and observe game rendering; game files remain unchanged.

Native depth has an identifiable attachment candidate, and the corrected retail diagnostic reads stable raw camera matrices. Their main-scene identity, render timing, resource format and supported execution boundary still require validation. They do not replace a complete engine velocity pass for moving or animated geometry.

## Installed-game evidence

The inspected installation is `H:/SteamLibrary/steamapps/common/Left 4 Dead 2`. Its `left4dead2/steam.inf` reports app ID 550, patch version 2.2.4.3 and network version 2.1.0.0. This installation also contains third-party graphics files; those are not evidence of a Valve-native motion-vector pass. The scan excluded workshop/addon packs, screenshots, crash dumps and third-party ReShade shaders.

### Official VPK directory indexes

Only directory trees and selected small material records were read. Large archive payloads were not extracted or scanned wholesale.

| Index | Entries | Shader entries | SHA-256 |
| --- | ---: | ---: | --- |
| `left4dead2/pak01_dir.vpk` | 22,273 | 0 | `51cc2957bbc5fd545357fd6b8da0fd5c387243756cd6f82e067d5e256e48fcdd` |
| `left4dead2_dlc1/pak01_dir.vpk` | 1,302 | 0 | `e24b54158e39b292bccab1b38f430f667dd1fca53846963876d45509dd7209f6` |
| `left4dead2_dlc2/pak01_dir.vpk` | 6,036 | 0 | `013b4e5331af837a39c03d1164b9e59658f4a29233d37a8488edebf4db14dbff` |
| `left4dead2_dlc3/pak01_dir.vpk` | 6,918 | 0 | `97fd476159c8cc9b12d41ea132e7a0239c6975285639eea5f1ae3b330b8d5f39` |
| `update/pak01_dir.vpk` | 3,634 | 0 | `47f31ef57a94f88463da8b2132a55cfed14bae127f35e4cfb68948e840ed8b52` |

No indexed name supplied an identifiable motion-vector/velocity buffer material. The base pack does include `materials/dev/motion_blur.vmt`, `materials/dev/depth_of_field.vmt` and `materials/dev/downsampledepth.vmt`.

The installed `motion_blur.vmt` uses the `MotionBlur` shader, a `_rt_FullFrameFB` base texture, `MotionBlurInternal`, `MotionBlurViewportInternal`, and a `MotionBlur` material proxy. Its SHA-256 is `6d13d4ae829ff0c4725164465f0e347a433f8de36f47d437b50ee08201a57a4c`.

The installed `downsampledepth.vmt` uses `_rt_FullFrameDepth` and `downsampledepth_extractnormal_ps20`. Its SHA-256 is `6d3dab7159d03159f603db6c401acd0a49cf1252e23b655c4419d776fcbe6382`. This establishes a named native depth consumer, but not that its texture is available, populated correctly and sampleable at Asteria's desired frame boundary.

### Actual compiled motion-blur shader

The installed `hl2/shaders` contains 341 compiled shader files. Related files include `motion_blur_ps20.vcs`, `motion_blur_ps20b.vcs`, `motion_blur_vs20.vcs`, `depthwrite_*`, `debugdrawdepth_*`, `depth_of_field_*` and `downsampledepth_extractnormal_*`. No velocity/motion-vector shader filename was identified in that directory.

`motion_blur_ps20b.vcs` was parsed offline, without loading it into the game or changing its bytecode:

| Check | Observed result |
| --- | --- |
| File size | 1,509 bytes |
| File SHA-256 | `6a5ddd9d5591118dfd326fcb99e38b9c15b1e6eed2114b49befe6fcb63744c24` |
| VCS version | 6 |
| Decompressed combination data | 18,432 bytes, verified against its stored size |
| Dynamic combinations | 15 |
| Sampler reflection | Every combination has exactly one sampler: `g_tTexSampler`, register `s0`, count 1 |
| Active blur constants | `g_flMaxMotionBlur`, `g_vConst5`, `g_vViewport`; combinations optimize unused constants away |

`motion_blur_ps20.vcs` has the same file size and SHA-256. `motion_blur_vs20.vcs` is 205 bytes, SHA-256 `405c2756381e240a0c60b6be02d623dd3b0360bfb0ccd60304e387f18e063ee3`.

The compiled reflection and actual material binding together exclude an additional velocity/depth input from this installed motion-blur path. They do not exclude an unrelated internal pass that is not exposed by these resources.

### Renderer interface and texture-name evidence

Printable strings in the installed `engine.dll`, `materialsystem.dll` and `left4dead2/bin/client.dll` include `_rt_FullFrameDepth` and `_rt_FullFrameFB`. The engine/client also identify `VEngineClient013`, `VClient016` and `VEngineRenderView013`. No identifiable velocity-buffer texture string was found in the narrowly inspected renderer binaries.

| File | SHA-256 |
| --- | --- |
| `bin/engine.dll` | `8a6d468059394bf4cfce32f11dcba12ef45de88da1373084d5a2916d0bf7ba86` |
| `bin/materialsystem.dll` | `09fd324cd977d5d4e2a216ee831c07ff8b882efd024137c27de4155fbdfcf0ba` |
| `bin/stdshader_dx9.dll` | `6a3915991277cec448bfdf8ed9dc7cc299662af09db220e2039e4df2d964d7ee` |
| `left4dead2/bin/client.dll` | `c0a8e1e88f7312db11bcfd4b0ec574d224a34b2d9a615d6088b28335eeb4af1f` |

An interface name found in a file is not proof of a valid live pointer or a complete ABI match. A texture-name string is not proof of live contents. Neither should enable FG automatically.

## What the Valve public SDK can and cannot establish

The public-source reference was pinned to Valve's Source SDK 2013 commit `be57e3d01ac7d7aa6a2cacbfef187c0b46616db0`. Its [README](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/README.md) identifies HL2, HL2: DM and TF2 game code. It does not identify this repository as L4D2 renderer source. Consequently, its data layouts and implementation details are reference material, not a confirmed L4D2 integration contract.

The public [motion-blur shader wrapper](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/materialsystem/stdshaders/motion_blur_dx9.cpp) binds the color input and passes a four-component material value as a pixel constant. The [pixel shader](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/materialsystem/stdshaders/motion_blur_ps2x.fxc) samples one color texture and forms a spatial blur direction from constant parameters. The [client postprocess reference](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/game/client/viewpostprocess.cpp) derives those parameters from camera history. These facts explain why a motion-blur effect need not contain a native per-object velocity buffer, and are consistent with the installed shader reflection above.

The public [VCS format header](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/public/materialsystem/shader_vcs_version.h) defines version 6 and static-combination records. It supplied the format reference for the offline shader inspection. No SDK shader was compiled, installed or substituted.

## Native camera and depth candidates

| Input | Native candidate | Remaining verification |
| --- | --- | --- |
| Current world-view and world-projection matrices | Guarded retail `VEngineClient013` slots 37/38 now yield stable raw matrices | Confirm the main scene and matching draw/frame; discard mirror, shadow, sky and viewmodel subviews |
| Camera origin, orientation, FOV and near/far planes | Guarded native matrices allow standard perspective recovery | Confirm final main-camera identity/frame association and unit scale; do not cast an SDK `CViewSetup` into the retail layout |
| Main-scene depth | Native `_rt_FullFrameDepth`, or the actual main depth attachment tracked through the existing graphics path | Identify the matching color/depth view, clear/copy timing, sampleable format, MSAA resolve, depth convention and GPU synchronization |
| Per-pixel geometry motion | An existing verified native velocity target, or a new renderer velocity pass | Currently unproven; motion-blur constants and camera matrices alone are insufficient |

The public [engine/client interfaces](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/public/cdll_int.h) describe `WorldToScreenMatrix`, `WorldToViewMatrix` and `GetPlayerView`. The installed client interface is version 016, so the SDK client method order must not be assumed. The public [render-view interface](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/public/ivrenderview.h) has matrix generation helpers, but the installed version 013 differs from that header's version 014.

The public [view setup](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/public/view_shared.h) describes camera/FOV/near/far fields. The [native render-texture reference](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/game/client/rendertexture.cpp) resolves the named full-frame depth texture through the material system. These public designs are reference material. The retail matrix path is now separately guarded and sampled, while the SDK structure layout and complete native input adapter remain unverified.

## Retail ABI and native draw follow-up, 2026-10-15

The [retail engine report](NativeBinary/retail-camera-abi.json) pins engine SHA-256 `8a6d468059394bf4cfce32f11dcba12ef45de88da1373084d5a2916d0bf7ba86`. `HideLoadingPlaque` at slot 29 shifts the real `VEngineClient013` world-to-screen/world-to-view getters to 37/38; SDK slot 36 returns `GetGameDirectory`. The same interface version name therefore did not establish layout compatibility. The guarded diagnostic avoids the screen-size path that obtains a material-system context and does not call SDK render-view methods with mismatched structures.

A completed `c1m1_hotel` diagnostic capture retained 6,089 observed frames and 2,350,661 recorded draw events. Two consecutive native matrix-pair copies matched bitwise and passed finite/rank checks. The verified standard projection recovers near approximately 7, far approximately 5,000 and a 16:9 aspect matching the 3840×2160 render surface. These are raw native observations, not a verified final main camera. A retained matrix, viewmodel, reflection or other subview must still be separated by actual draw/frame evidence; thread safety is not proven by a successful run.

The follow-up scan parses all 341 inspected `hl2/shaders` files: 339 VCS6 and two VCS4, 41,402 compressed blocks and 188,679 unique compiled combinations. The 188,676 constant tables expose 468 names. No recognizable previous-frame/history/velocity name establishes an existing native velocity pass. Original source hashes are unchanged; third-party shader directories remain excluded. This inventory does not enumerate every private or unnamed runtime path.

Native vertex programs do supply a positive lead: original CTAB and disassembly confirm current `cViewProj` c8–c11 and current model/bone constants. Exact full-bytecode SHA-1 keys match captured DXVK shaders to original native VCS combinations. The 26 whitelisted scene vertex shaders have an actual reflected float-member offset 256 and stride 16, placing their 64-byte `cViewProj` at descriptor-relative byte 384. Applying c8×16 alone would read the wrong bytes. The shader identity/layout establishes where to inspect; it does not establish a main camera or previous-frame motion.

The optional pipeline recorder associates captured shader bytes with observed target/depth bindings. Its controlled two-shader host passes exact bytecode/target checks and excludes Asteria effects. In the real-game report, 1,039 missing payloads and 4,047 unknown draw pipelines leave coverage incomplete despite no capacity-limit hits. No pixel-write or motion semantics are inferred from those association counts.

A separate Vulkan layer bridge now copies at most 4,096 bytes from buffers already mapped by the application, with ownership, range, lifetime and stable-copy checks. It does not introduce GPU mappings or change shaders. The 255-check bridge contract and 60-present passthrough test pass. The first completed game-uniform test now reads actual stable buffer bytes. Its resource snapshot retained 1,569 frames and 830,871 recorded draws; a later pre-exit sampler snapshot had accumulated 163,033 copies. External process finalization recorded exit code 100; this is not by itself evidence of a clean shutdown. A second capture adds direct row-major register/getter comparisons across neighboring observer intervals. Both matches and differences are retained, without transposition, fitting, view selection or a claim of same GPU frame. This is positive native-source/readback evidence, not a completed main-camera or motion input.

The second uniform collection contains 97,299 copies: its comparison totals are 10,465 matching, 82,428 differing and 4,391 unavailable, while the resource writer's separate snapshot contains 8,661 frames and 3,183,866 draw records. The independent validator verifies 1,040 retained samples and recomputes 1,009 comparisons (78 matching and 931 differing). It passes only evidence integrity. Selection is capped at 16 attempts per device/observer frame, not uniformly sampled across the scene. The collection has 1,653,657 budget skips, 455,026 unverified-profile draw observations, 380 absent pipeline payloads and 517 unknown pipeline draws. Its match/mismatch counts cannot describe all views or rule out an unobserved motion input. The session displayed a game exception, stopped frame progress despite continuing writer activity, and generated a crash dump. Both game-stability and clean-shutdown flags remain false; external exit code 100 must not conceal that failure. The retained diagnostic evidence has now been reviewed, with the root cause and shutdown repair still unresolved.

The [final session outcome](../InputProbe/results/GameNativeUniform/test_session_outcome.json) retains two actual exception dumps at `vstdlib.dll+0x75f0`, exception code `0xc0000005`, on `mov edx, dword ptr [eax+0x24]`. Matching retail disassembly and public Valve `ConVar_Unregister` source support interpreting the path as command unregistration traversing an invalid virtual table; these are not private-symbol-backed names and do not identify the original object owner.

The separate shutdown control loaded neither the native-input add-on nor the FG layer, as checked against its actual module inventory. ReShade and production Asteria were still present. Quit failed and the crash dialog reproduced; the owned control process was stopped after collecting its private diagnostic evidence. The late WOW64 capture does not establish the control's precise x86 exception instruction. This shows the two experimental input components are not necessary to reproduce the observed shutdown failure; it does not identify a specific offending extension or prove identical fault addresses in every run. The outcome plus its three supporting report hashes are checked by the uniform validator. `exception_diagnostic_pending=false`, root-cause identification false and shutdown resolution false preserve the distinction between reviewed evidence and a solved defect.

## Conditions for a real engine-native velocity input

If an existing native velocity attachment cannot be identified and verified, a new engine rendering pass is required. Merely copying the depth attachment or exposing camera matrices does not produce movement for animated infected, moving props, weapons, particles or changing silhouettes.

The pass must have access to the actual current and previous rendered geometry transforms, camera projection and view transforms, and previous/current skinning data for animated models. It must write current-to-previous screen displacement for visible scene surfaces at the appropriate resolution, with compatible depth testing and consistent alpha-tested geometry. Disocclusion, viewmodel projection and any excluded transparent/HUD content need explicit treatment.

A renderer adapter must then export that native velocity texture together with the matching scene color, depth, frame ID and camera parameters. It must describe displacement units and direction, depth convention, viewport/subrect and jitter, reset history on camera cuts or scene changes, and synchronize GPU ownership with the interpolation queue. Inputs collected from simulation ticks or console output are not automatically aligned with rendered frames.

This requirement needs a verified renderer data path exposing the necessary native state. Investigation continues from the installed binary, actual shader identities and draw bindings; it does not depend on the user obtaining an undocumented interface from Valve. The public SDK alone does not establish that the installed retail renderer permits such a pass. It is not achievable through a VPK material-name change alone, and a postprocess add-on cannot relabel estimated data as a pre-existing engine-native buffer.

The native-input route must remain unqualified until those native inputs are proven. Under the updated policy, an independently qualified Estimated producer is also allowed, but none is implemented. Asteria therefore leaves game FG inactive when its provider/session is unavailable. The historical investigation below did not implement optical-flow substitution, camera-only reconstruction, dummy zero vectors, shader rewriting or anti-cheat bypasses.
