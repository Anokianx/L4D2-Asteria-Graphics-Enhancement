# Native model and bone input contract

Author: Anokianx

This research stays outside production Build and does not launch, attach to, or change the game. It extends the existing 26 native camera shader profiles with their current engine-written geometry inputs. The original `NativeShaders/native-uniform-profiles.json` remains unchanged.

`geometry-profiles.json` contains 26 exact captured SPIR-V identities and 41 original Valve VCS combination origins. Every origin is re-extracted from the installed, hash-checked VCS, matched by full raw D3D9 SHA-1 and SHA-256, checked against its CTAB, and disassembled with the installed Microsoft D3DX tool. `Disassembly/` retains executable instructions without generated comment text. The report includes the actual cModel read instructions and the following c8–c11 DP4 projection window, rather than accepting a constant name as proof of usage.

| Compiled path | Profiles | Actual native constant range | Descriptor-relative offset | Bytes |
| --- | ---: | --- | ---: | ---: |
| Single current model matrix | 17 | c58–c60 | 1184 | 48 |
| Current skinning palette range | 9 | c58–c216 | 1184 | 2544 |
| Current view projection | 26 | c8–c11 | 384 | 64 |

The UBO is the already verified DXVK 2.3.1 hardware vertex constant buffer at set 2, binding 0: float member offset 256, float4 array stride 16. Add the actual bound descriptor offset before passing any relative offset to the existing read-only mapped-buffer bridge. The report does not treat a buffer's native handle as the start of its currently bound slice.

CTAB declares `float4x3 cModel[53]` for both paths. Optimized single-matrix paths nevertheless have `register_count=3`, not 159. Use the actual register count. Three float4 vectors describe one affine transform in the operational DP4 layout; a skinning path first blends rows using native blend weights and indices. A 159-register compiled range permits 53 matrix slots; it does not prove that every slot is used by every draw or is a separate skeleton root. Do not flatten an animated palette into a single model matrix.

Twenty-three profiles contain executable cFlexScale reads together with additional vertex declarations such as position1/normal1. A cModel snapshot alone therefore does not represent their complete deformed geometry. The remaining infected profiles also require their exact original vertex inputs and any shader-specific deformation/discard state. No current profile performs a vertex texture fetch, within this bounded set of captured shader identities. This is not a claim about all game shaders.

For a coherent diagnostic snapshot, the current mapped-buffer bridge's 4096-byte limit can cover camera, flex scale and model constants together: read descriptor-relative bytes [384,1232) for a single matrix (848 bytes), or [384,3728) for the palette path (3344 bytes). The copied matrix begins at byte 800 inside either snapshot. Validate descriptor range, arithmetic overflow, buffer generation, mapping bounds, stable copy and finite values before decoding. This does not solve GPU submission/frame ownership by itself.

`Validate_Geometry_Profiles.py` uses a separate raw CTAB decoder and D3D9 operand-token reader. It checks 41 origins, 299 executable model source operands, all 26 translated shader payload hashes and assembly evidence hashes. Eight corrupted metadata/readiness fixtures must fail, including the tempting `elements*3` overread on a single-matrix path.

```powershell
py -3 Asteria/Runtime/FrameGeneration/Research/NativeGeometry/Build_Geometry_Profiles.py
py -3 Asteria/Runtime/FrameGeneration/Research/NativeGeometry/Validate_Geometry_Profiles.py
py -3 Asteria/Runtime/FrameGeneration/Research/NativeGeometry/Build_Draw_Identity_Contract.py
```

## Draw identity integration

`draw-identity-contract.json` records the installed ReShade header hashes, exact event declarations and input-layout fields. These are available graphics draw descriptors, not engine entity identifiers.

1. Extend per-command-list state with vertex buffer slots and the index buffer. `bind_vertex_buffers` supplies `first`, `count`, handles and byte offsets; its `strides` pointer is optional. Update only the specified slots. Preserve a known stride from a copied pipeline input layout when appropriate, and otherwise report unknown rather than zero as a usable stride. `bind_index_buffer` supplies the buffer, byte offset and index element size.
2. At `init_pipeline`, copy each `input_layout` element and the contents of its optional semantic string. The array and string pointers are callback-owned and cannot be retained. Record location, format, binding, offset, stride and per-instance step rate. Bind state to pipeline generations, exact verified VS identity and the current command list.
3. Record `draw` parameters (`vertex_count`, `instance_count`, `first_vertex`, `first_instance`) and `draw_indexed` parameters (`index_count`, `instance_count`, `first_index`, signed `vertex_offset`, `first_instance`). An indexed first byte is `IB_offset + first_index*index_size`; retain signed base vertex separately. Return false so the observation never suppresses the original draw.
4. Give resources, pipelines and command lists lifetime generations. Retire them on destruction; clear transient command state on reset. Track content writes separately from binding generations: one persistent VB/IB can be overwritten without destruction. Propagate secondary command-list execution correctly or mark it unsupported. Reset/map/scene/device transitions invalidate any cross-frame candidate association.
5. Emit a bounded structural signature and explicit completeness flags: profile hash, layout hash, live VB/IB generations and ranges, draw/instance parameters, view/render-target scope and captured native input identity. This can group duplicate geometry submissions for diagnostics. It is not sufficient to join actors across frames. Exclude incomplete signatures, collisions, indirect draws without safely captured arguments, and capacity loss from any history-ready result.

A static mesh can share VB/IB/pipeline with many actors. Dynamic ring slices can move each frame. Actor sorting, culling and LOD can change draw order. Palette contents are not a stable identifier. Therefore neither this contract nor current ReShade draw fields establish a persistent entity identity or a previous-frame transform.

## Motion-vector boundary

These are current native inputs, including current animation state. No previous engine palette, native previous object transform, engine-provided per-pixel motion texture, or reliable entity-lifetime join has been established. Saving these matrices and inventing a history association would be an Asteria reconstruction path; it must not be described as discovering an already existing native motion-vector output. All motion-vector, entity-identity and game-FG readiness flags remain false. Native main-view and same-frame provenance still needs its independent validation.
