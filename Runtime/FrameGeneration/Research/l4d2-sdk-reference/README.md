# L4D2 SDK Reference and Native Input Contract

Author: Anokianx  
Date: 2026-10-14

## Provenance

These are selected, unmodified headers from [AlliedModders' L4D2 SDK branch](https://github.com/alliedmodders/hl2sdk/tree/2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6), pinned to commit `2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6`. They are an L4D2-specific SDK reference, rather than the Source SDK 2013 branch. AlliedModders maintains this distribution; the headers retain their original Valve attribution. They are not a new Valve runtime compatibility guarantee.

`source-manifest.json` records the immutable URL, Git blob SHA-1, SHA-256 and byte count of every downloaded file. All 12 raw headers were checked against the repository tree's Git blob identities. Total raw content: 287,231 bytes. No SDK libraries or game binaries were downloaded. The pinned repository tree has no standalone license/copying file; the source files' existing copyright notices remain intact and have not been replaced with an Asteria license.

## Matching the installed interface names

Read-only printable-string inspection of `H:/SteamLibrary/steamapps/common/Left 4 Dead 2/bin/engine.dll` and `left4dead2/bin/client.dll` finds the following exact names:

| Interface | L4D2 branch header | Installed strings | Result |
| --- | --- | --- | --- |
| Engine client | `VEngineClient013` | `VEngineClient013` | Exact name match |
| Render view | `VEngineRenderView013` | `VEngineRenderView013` | Exact name match |
| Model rendering | `VEngineModel016` | `VEngineModel016` | Exact name match |
| Client entity list | `VClientEntityList003` | `VClientEntityList003` | Exact name match |
| Client DLL | `VClient015` | `VClient016` | Mismatch; do not bind this header's client methods to version 016 |

An embedded name is not an executed `CreateInterface` query. Neither the presence of the name nor a successful factory query alone verifies every current implementation detail. The initial header/name inspection was read-only. A subsequent isolated game diagnostic attempted the getters, but did not establish a valid or safely sampled camera; see the runtime boundary below.

## Camera probe contract

The selected [engine header](https://github.com/alliedmodders/hl2sdk/blob/2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6/public/cdll_int.h) defines `IVEngineClient` directly, without a virtual destructor or inherited interface preceding its methods. The required read-only methods have these zero-based declaration positions:

| Method | Source position | Result ABI |
| --- | ---: | --- |
| `GetScreenSize` | 5 | `void`, two `int&` outputs |
| `GetViewAngles` | 19 | `void`, one `QAngle&` output |
| `IsInGame` | 26 | `bool` |
| `IsConnected` | 27 | `bool` |
| `WorldToScreenMatrix` | 36 | `const VMatrix&` |
| `WorldToViewMatrix` | 37 | `const VMatrix&` |

These positions were derived by enumerating the pinned header's virtual declarations. `abi-contract.json` records their exact declaration strings. They are not guessed process-memory offsets.

A minimal camera adapter can retain the first 38 exact virtual declarations and opaque forward declarations for unused types, or generate an equivalent typed table using this explicit contract. Windows x86 MSVC calls use the native member calling convention; the factory typedef is the published `void* (*)(const char*, int*)`. The retained `vector.h` and `vmatrix.h` provide the actual `QAngle` and matrix value definitions.

Any future enabled adapter must run at a verified Source engine/main-view callback, not merely a Vulkan Present callback that happens to execute inside the game. It should query the existing `engine.dll` factory for exactly `VEngineClient013` and fail closed on a missing interface. It must not load another engine DLL into a standalone probe, create a remote thread, scan for a hidden camera offset or substitute a Source SDK 2013 interface.

Read methods only when the engine callback/thread contract has been verified, copy their outputs immediately, and publish a timestamped snapshot. Reject non-finite/singular matrices, non-positive dimensions, a disconnected/loading game and a changing module/interface lifetime. A menu-state snapshot is not a valid scene camera. Returned references must not escape as stored pointers into the engine. The examined headers do not promise lock-free or cross-thread-safe getters and do not publish their renderer implementation. They cannot establish whether a particular L4D2 getter takes a material-system lock.

The matrix pair is native engine data. `GetViewAngles` is an orientation query and can differ from the final rendered camera; it does not by itself establish recoil, interpolation, off-center projection or camera-cut handling. A sample taken during postprocessing/present is not automatically aligned with the world-color/depth render. A game-state/render-stage comparison is required before declaring the snapshot frame-aligned.

The [render-view header](https://github.com/alliedmodders/hl2sdk/blob/2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6/public/ivrenderview.h) also matches version 013. Its source-declared `GetFieldOfView` position is 28 and `GetMatricesForView` is 50. The latter consumes a caller-supplied `CViewSetup`; it does not return the active camera. Do not manufacture a view setup and describe it as native capture. The header's client `GetPlayerView` is behind the mismatching `VClient015` interface and remains excluded.

## Native model and bone evidence

The [model-render header](https://github.com/alliedmodders/hl2sdk/blob/2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6/public/engine/ivmodelrender.h) matches `VEngineModel016`. Its source-declared `DrawModelSetup` and `DrawModelExecute` positions are 18 and 19. `ModelRenderInfo_t` includes native origin/angles, `pRenderable`, `pModel`, optional `pModelToWorld`, `entity_index` and a model instance handle. `DrawModelExecute` accepts an optional bone-to-world array; `DrawModelState_t` provides the studio header, renderable, model transform and LOD.

This supplies a concrete source-level contract for observing current model rendering state. Entity index is not a stable cross-frame identity by itself: entity slots can be reused, one model can draw in multiple views, and body/skin/LOD or model instances can change. Bone-array pointers are callback-lifetime data and cannot simply be retained for the next frame. A null optional array is permitted.

The [client-renderable header](https://github.com/alliedmodders/hl2sdk/blob/2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6/public/iclientrenderable.h) exposes `SetupBones` at source position 16 and `RenderableToWorldTransform` at 34. `SetupBones` outputs current bone-to-world matrices for a supplied time/mask and can evaluate/update animation state. It is not a read-only previous-frame-bone query; calling it from an arbitrary present callback can change or mismatch animation timing. Prefer renderer-provided bones in an actual draw-stage adapter, when available.

The selected `studio.h` describes native bone/mesh data. `bone_setup.h` contains bone-velocity calculation helpers and cache interfaces, but no exported dense screen-motion texture. Calculating a skeletal derivative is not evidence of a ready per-pixel velocity buffer.

## Dense native motion-vector availability

No dense motion-vector/velocity-target query was identified in these selected L4D2 interfaces. This limited header search does not prove that all private engine paths lack such a target. It does establish that the current camera and model contracts do not export one.

Current native matrices and current native bones alone do not satisfy a dense velocity input. A future engine-native velocity pass must retain the corresponding previous rendered camera/model/bone states, render visible geometry with compatible depth/alpha tests, and account for world/static geometry, skinned models, dynamic objects, viewmodels and relevant transparent geometry. Merely obtaining `entity_index` or a `SetupBones` result is not that pass.

That pass needs a supported main-view draw-stage adapter with geometry and resource ownership access, plus a verified way to publish its velocity texture and matching depth/color to Asteria. The contracts above are sufficient to start a strictly read-only camera/model diagnostic, but they do not establish a working extra render pass or game FG. No optical-flow estimate, camera-only motion reconstruction or dummy-vector substitute was implemented here.

## Verification status

- Raw header download and Git/SHA-256 identity verification: complete.
- Header-declared interface names compared with installed binary strings: complete.
- Read-only ABI declaration contract and independent native-call guards: recorded and tested.
- Current game camera capture: not connected; the isolated attempt failed matrix guards.
- Engine callback/thread safety: unverified; a later busy Present thread was observed without a proven root cause.
- Current/previous model and bone capture at the main view: untested.
- Complete engine-native dense velocity texture: unavailable in the examined public contracts.
- Game frame generation: not enabled by this reference.

## Implemented isolated camera diagnostic

`../../InputProbe/EngineCameraProbe.h` implements the declaration contract as the header-only `asteria_native_input::SampleEngineCamera()`. It requires no additional SDK library. Camera calls are disabled by default; the function returns unavailable before querying the factory unless its explicit diagnostic option is selected. Only the matching engine interface can be queried; the mismatching client interface is excluded. It validates committed readable/executable pages with `VirtualQuery`, isolates memory/call exceptions with structured exception handling, copies returned matrices and classifies copy, finite and rank failures separately. These guards cannot stop a valid-address getter that deadlocks or spins indefinitely.

Any retained raw values are marked `diagnostics_only`. `native_camera_ready`, camera semantics, frame timing and engine thread safety remain false. The function does not estimate position, clipping planes, jitter or motion vectors. Failed matrix validation never becomes a usable camera input.

The diagnostic was called outside the observer mutex and its writer only serialized completed snapshots. In the live attempt, active/connected state was reported but matrix guards failed. A later test showed a busy Present thread; the observations do not prove which component caused it. Therefore, resource-only game captures now leave camera calls disabled. Do not set `ASTERIA_NATIVE_ENGINE_CAMERA=1` for the current game's Vulkan Present callback. A future enabled test first requires a verified engine/main-view execution boundary. A writer thread must never invoke engine methods.

`contracts/Build_HostTest.bat` builds an independent x86 host test. The test passed default-disabled behavior, explicit `ASTERIA_NATIVE_ENGINE_CAMERA=1` with no engine module, inaccessible page rejection, data/code protection distinction, the native member calling convention, matrix-reference copying and separate finite/rank/copy failure classification. This validates the guards and calling convention against a local test object; it is not a successful live L4D2 camera capture. No game was launched by this host test.
