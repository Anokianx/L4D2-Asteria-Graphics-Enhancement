# Installed L4D2 native shader evidence

Author: Anokianx  
Date: 2026-10-15

## Result

The complete inspected `hl2/shaders` tree parses successfully: 341 files, 41,402 bounded LZMA blocks, 188,679 unique compiled combinations and 188,676 constant tables. The two VCS4 assembly files have three combinations without reflection names. The remaining 339 VCS6 files expose 468 distinct reflected constant names. None of the names matches previous-frame/history/velocity motion-vector identifiers. This strengthens the earlier motion-blur inspection but does not prove that the retail engine has no undocumented velocity pass.

The useful positive result is the actual vertex-shader transform register mapping. These are observed installed-game CTAB entries, not guessed interface slots:

| Installed shader family | Reflected constant | D3D9 float registers | Meaning supported by Valve public reference |
| --- | --- | --- | --- |
| `infected_vs20`, `phong_vs20`, `vertexlit_and_unlit_generic_vs20` | `cViewProj` | c8–c11 | Current world-to-clip view/projection transform |
| Skinned variants in those families | `cModel` | Starts at c58; 3 or 159 float4 registers | Current model transform or 53 model/bone matrices |
| `lightmappedgeneric_vs20` and other static families | `cModelViewProj` | c4–c7 | Current model-to-clip transform |

`cViewProj` appears in 61 inspected files and `cModel` in 69. The actual 159-register `cModel` entry has rows=4, columns=3, elements=53 and D3DX matrix-columns class, matching the public Source skinning reference. The Shader Model 3 morph inputs also identify current deformation data. These inputs could help a renderer adapter obtain native current transforms. They are not themselves a dense velocity attachment, do not identify a main view automatically, and do not provide previous-frame skinning correspondence.

The recorded `infected_vs20_disassembly.json` confirms the first compiled combination computes clip coordinates with `dp4` against c8, c9, c10 and c11. Pixel-shader constants named `g_mViewProj0`, `g_mViewProj1` and `g_mViewProj3` are individual float4 rows in refraction families; the numeric suffixes are not evidence of previous/current camera pairs. The separate `g_CustomViewProj` in `portal_vs20` also requires subview classification before use.

The register numbers above are D3D9 constant register numbers. They must not be used directly as Vulkan uniform-buffer byte offsets. DXVK translation, descriptor binding, constant upload, matrix storage/transposition and actual draw/frame association must be checked first. Main-scene, weapon, shadow and reflection views must be separated using observed render state.

## False-positive candidates

| Candidate | Installed evidence | Interpretation |
| --- | --- | --- |
| `motion_blur_ps20` / `ps20b` | 15 combinations each; only `g_tTexSampler` s0, plus blur/viewport constants | Camera-based color blur, no additional velocity sampler |
| `morphaccumulate_ps30` / `SideSpeedMap` | MorphTarget s0, SideSpeedMap s1, MorphWeights s2 and morph scale | Morph weighting/deformation data; the public shader uses side/speed values to weight a vertex delta, not screen displacement |
| `water*` / flow samplers | Flow/flow-noise samplers with flow parameters | Water texture-advection inputs; not established scene geometry motion |
| `emissive_scroll_blended_pass*`, `vortwarp*` | Flow-map sampler alongside material textures | Material UV/distortion animation; not established scene geometry motion |
| `hdrselectrange_ps20` / `ps20b` | Only inspected PS family writing both oC0 and oC1 | Actual disassembly sends sampled HiSampler color to oC1 and constant black with alpha=1 to oC0; neither output is a motion vector |

Multiple render targets alone do not identify velocity. A dedicated single-target velocity pass would not require MRT, so the MRT inventory must not become a negative filter for runtime candidates. RGBA floating-point resources remain possible candidates until pixel content and producer/consumer relationships are examined.

## Reproduction

Run from the repository root:

```powershell
py -3 Asteria/Runtime/FrameGeneration/Research/NativeShaders/Scan_Native_Shaders.py --game 'H:\SteamLibrary\steamapps\common\Left 4 Dead 2'
py -3 Asteria/Runtime/FrameGeneration/Research/NativeShaders/Validate_Native_Shaders.py
py -3 Asteria/Runtime/FrameGeneration/Research/NativeShaders/Disassemble_Selected_Shader.py --shader hdrselectrange_ps20.vcs
```

The scan uses Python's standard library and writes `native_shader_inventory.json` plus the smaller `native_shader_findings.json` beside the script. The validator checks the actual inventory hash, all 341 source hashes, the known motion-blur sampler in all 15 combinations, and rejection of five malformed input cases. It never loads game libraries, starts the game, replaces bytecode or writes into the game tree. Output paths within the game directory are rejected. The disassembly helper uses the installed Microsoft system `d3dx9_43.dll`, reads one validated VCS6 file, and prints the first compiled combination. It does not install a graphics component. The recorded HDR disassembly is in `hdrselectrange_disassembly.json`.

The VCS6 reader validates directory boundaries, the sentinel, unique static IDs, every alias destination, every block size, each dynamic-combination ID, shader END tokens, comment boundaries and constant-table bounds/version. Valve raw LZMA blocks can omit an end-of-stream marker; decompression is bounded by the stored output length, and the full returned combination structure is validated. The scan does not materialize the 385,625,888 decompressed bytes on disk. Static aliases are validated and their underlying bytecode is parsed once, so the unique combination count is not the theoretical logical combo count.

The `hl2` installation path and recorded SHA-256 values provide reproducible installed-file provenance, not Steam-depot authentication. The existing non-`hl2` Neko/PBR/outline shader trees are inventoried by filename as excluded provenance, including the nested duplicate folder. No VPK payload extraction is performed.

## References and limits

The follow-up [DXVK constant-layout and actual shader-identity investigation](DXVK_CONSTANT_LAYOUT.md) establishes the translated block packing, native source-key matches and the initialized shader-profile whitelist. It preserves the distinction between confirmed shader layout and unconfirmed main-camera/frame semantics.

The [Valve VCS format header](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/public/materialsystem/shader_vcs_version.h) defines the header/static records/version. Microsoft documents the [constant table record](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxshader-constantinfo), [type record](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxshader-typeinfo) and [disassembly API](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxdisassembleshader).

The pinned Valve [common vertex shader reference](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/materialsystem/stdshaders/common_vs_fxc.h) agrees with the observed current-transform packing. The [morph accumulation reference](https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/materialsystem/stdshaders/morphaccumulate_ps30.fxc) explains the SideSpeedMap false positive. Source SDK 2013 is a reference for these declarations, not a confirmed complete L4D2 renderer API.

Reflection names can be optimized or generic. Shader semantics, runtime selection and texture pixel contents cannot be established solely from names. A future native-input adapter still needs GPU resource content, correct producer/frame association, valid camera conventions and native object/deformation history. Both motion-vector and camera verification flags stay false. This investigation adds no optical-flow substitute, reconstructed velocity pass or anti-cheat bypass.
