# DXVK constant layout and native shader identity

Author: Anokianx  
Date: 2026-10-15

## Observed version and source contract

The installed-game test log reports DXVK v2.3.1. The annotated upstream tag resolves to commit `037d0fa1ad093065b18dad968d5898cbcd143e0e`. `Reference/DXVK-v2.3.1` retains 21 selected unmodified upstream files and the MIT license, with a per-file URL, size and SHA-256 manifest. A reported version is not proof that the installed DLL is byte-for-byte identical to the upstream release.

The source [constant layout](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/d3d9/d3d9_constant_layout.h), [constant structures](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/d3d9/d3d9_constant_set.h) and [DXSO compiler](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/dxso/dxso_compiler.cpp) establish this hardware vertex profile:

| Block member | Type | Offset | Array stride |
| --- | --- | ---: | ---: |
| 0, `i` | int4[16] | 0 | 16 |
| 1, `f` | float4[256] | 256 | 16 |

The float register array follows 256 bytes of integer constants. Thus c8–c11 begin at **384, 400, 416 and 432 bytes relative to the bound descriptor range**, and together occupy 64 bytes. The software vertex-processing profile separates float constants into a different block; this hardware offset must not be applied to that layout.

The compiler's initial resource slot/set decorations are logical. [Shader code remapping](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/dxvk/dxvk_shader.cpp) rewrites set/binding according to the [actual pipeline binding layout](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/dxvk/dxvk_pipelayout.cpp). The normal vertex descriptor set is 2. The physical binding number must come from the captured module, especially when vertex texture resources are present.

## Actual game modules and exact native origins

The first reflection snapshot contains 255 actual game-session SPIR-V modules. All parse structurally. Eighty match the hardware float block layout, and 49 load c8, c9, c10 and c11 directly into `OpDot` operations. These are actual final module decorations and instructions, not deductions from the upstream layout alone.

DXVK [GetShaderModule](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/d3d9/d3d9_shader.cpp) constructs its source key from SHA-1 of the full D3D9 bytecode length. [The analyzer](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/dxso/dxso_module.cpp) includes the version/header and END tokens. The matcher hashes **the untouched full raw bytecode from VCS**, including CTAB/comments/END. It does not guess comment stripping, alter tokens or substitute shader code.

Across 37,721 validated installed vertex combinations, 78 of 80 requested module source keys match exact raw VCS bytecode. All 49 of the c8–c11 dot-use candidates match. The source `VS_3fd98757025273333db790f6481a590dc0c96113`, carried by SPIR-V SHA-256 `0507793291c82ee7f10d05ba8b0f4c74059d4654ffed736f21c5bf8c400d2911`, maps to `depthwrite_vs30.vcs`, static combo 0, dynamic combo 3. It is therefore excluded from the main-camera sampling whitelist despite its correct float block layout.

`native-uniform-profiles.json` keeps 26 modules from `infected_vs30`, `phong_vs30`, `vertexlit_and_unlit_generic_vs30` and its bump family. Each profile requires:

- The actual captured SPIR-V SHA-256 and exact file bytes.
- Its actual vertex entry point, DescriptorSet and Binding.
- Actual Block member Offset=256, ArrayStride=16 and c8–c11 direct dot usage.
- An exact original VCS source-key match and that same compiled combination's reflected `cViewProj` float4x4 declaration at c8–c11.

The manifest is small and intended for explicit initialization-time loading. It provides `shader_sha256`, `spirv_file`, `stage`, `descriptor_set`, `binding`, `relative_byte_offset` and `byte_length`. An observer can validate file hashes during initialization and compare the raw pipeline shader payload with the prepared bytes without render-callback file access or hashing. Matching these profiles establishes shader identity and layout; it does not establish that a particular draw belongs to the final main view.

## Descriptor offsets and the public ReShade limit

The pinned [D3D9 constant-buffer allocator](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/d3d9/d3d9_constant_buffer.cpp) allocates aligned ring slices. [The buffer descriptor](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/dxvk/dxvk_buffer.h) includes the physical slice plus ring-range offset. In [updateResourceBindings](https://github.com/doitsujin/dxvk/blob/037d0fa1ad093065b18dad968d5898cbcd143e0e/src/dxvk/dxvk_context.cpp), the standard constant-buffer descriptor is ordinary `UNIFORM_BUFFER`, and descriptor binding supplies dynamicOffsetCount=0.

For that source path, the address calculation is:

```text
descriptor.buffer.offset + reflected float-member Offset + register * ArrayStride
```

It is not `c8 * 16` from the whole Vulkan buffer. ReShade `buffer_range.offset` and `size` expose the ordinary descriptor range. Its public `bind_descriptor_tables` event does **not** include Vulkan bind-time dynamic offsets. If an actual descriptor is dynamic, the additional bind-time offset cannot be recovered from this event alone.

SPIR-V Uniform/Block does not distinguish ordinary and dynamic Vulkan descriptor types. The ordinary descriptor/count-zero result is supported by the matching upstream implementation; an actual-game raw `VkDescriptorSetLayout` descriptor-type and `vkCmdBindDescriptorSets` dynamicOffsetCount capture has not been recorded here. This boundary must remain explicit in sampling diagnostics. The missing public argument is real, but it is not evidence that standard upstream DXVK 2.3.1 uses dynamic offsets.

## What remains unconfirmed

Main-view classification, frame/submission timing, the current descriptor association, ring-buffer lifetime and write ordering still need verification. Geometry shaders can use the same camera constant contract for weapons, shadows, reflections and other subviews. A native current camera can be valid while its association with one specific rendered color/depth frame remains unproven.

No buffer data is read by these Python tools. No game files are modified, no shader is installed, and no game is launched by this investigation. `native_camera_ready`, camera semantics and native motion-vector readiness remain false. These native current transform declarations do not supply a verified dense motion-vector attachment or prior bone/deformation history.

## Reproduction and validation

```powershell
py -3 Asteria/Runtime/FrameGeneration/Research/NativeShaders/Prepare_DXVK_Reference.py
py -3 Asteria/Runtime/FrameGeneration/Research/NativeShaders/Reflect_SPIRV_Inputs.py
py -3 Asteria/Runtime/FrameGeneration/Research/NativeShaders/Build_Native_Uniform_Profiles.py
py -3 Asteria/Runtime/FrameGeneration/Research/NativeShaders/Build_DXVK_Layout_Report.py
py -3 Asteria/Runtime/FrameGeneration/Research/NativeShaders/Validate_SPIRV_Reflection.py
```

The reflection reader uses a pinned unmodified Khronos grammar with its license. It reports entry points, descriptor blocks, offsets/strides, locations and access-chain/load/dot hints. It checks headers, instruction bounds, known opcodes, result IDs and reflected metadata consistency. It is not a full SPIR-V semantic validator. Its validation passes 92 official FidelityFX modules and the 26 selected actual-game modules, and rejects seven malformed input cases. Missing capture files produce an explicit source-unavailable report, not a guessed shader profile.
