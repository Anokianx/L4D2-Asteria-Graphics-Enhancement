
# Asteria Shader workspace

`Effects/Asteria.fx` is the visible pipeline validation effect. `AsteriaCAS.fx` and `AsteriaSMAA.fx` wrap the SweetFX CAS/SMAA sources in `Includes`, retaining their upstream notices. SMAA lookup textures are in `Textures`. Upstream commit identifiers are recorded in `sources.json`.

The Asteria effects catalog is in `Asteria/effects.py`. SMAA and CAS start enabled; the new visual effects and validation marker start disabled. Each module can be adjusted independently.

| Effect file | Technique | Controls |
| --- | --- | --- |
| `AsteriaBloom.fx` | `AsteriaBloom` | Intensity, highlight threshold, diffusion radius |
| `AsteriaColor.fx` | `AsteriaColor` | Exposure, contrast, saturation, cool/warm balance, vibrance |
| `AsteriaTonemap.fx` | `AsteriaTonemap` | Curve mix, filmic/ACES/Reinhard SDR curve |
| `AsteriaVignette.fx` | `AsteriaVignette` | Intensity, clear radius, edge softness |
| `AsteriaDeband.fx` | `AsteriaDeband` | Intensity, sample radius, quality |

Bloom extracts highlights and runs two separable blur passes at one-quarter width and height before compositing at the output resolution. Color grading, tone mapping and vignette each use one full-screen pass. Deband uses 4, 8 or 12 neighbouring samples depending on quality and rejects strong edges; the sampling pattern is static so it does not introduce time-based noise. These modules require no depth buffer. Tone mapping adjusts an SDR image and does not change the display to HDR.

The Runtime add-on controls these effects through ReShade uniforms and technique states. The active configuration is written to the Asteria runtime state directory at game launch; packaged configuration templates are in `Runtime/ReShadeHost`. `Presets/validation.json` describes the same `.fx` resources and control keys for reference; the launcher uses the ReShade `.ini` preset instead. The former window-capture `.glsl` shaders have been removed.

Sources: [SweetFX](https://github.com/CeeJayDK/SweetFX), [ReShade shader includes](https://github.com/crosire/reshade-shaders).
