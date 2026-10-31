#include "ReShade.fxh"

uniform float AsteriaVignetteStrength < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_label = "Vignette intensity"; > = 0.2;
uniform float AsteriaVignetteRadius < ui_type = "slider"; ui_min = 0.3; ui_max = 1.0; ui_label = "Clear radius"; > = 0.75;
uniform float AsteriaVignetteSoftness < ui_type = "slider"; ui_min = 0.05; ui_max = 1.0; ui_label = "Edge softness"; > = 0.35;

float4 PS_AsteriaVignette(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(ReShade::BackBuffer, uv);
    float2 centered = (uv - 0.5) * 2.0;
    float distance_from_center = length(centered) * 0.70710678;
    float falloff = smoothstep(AsteriaVignetteRadius, AsteriaVignetteRadius + AsteriaVignetteSoftness, distance_from_center);
    color.rgb *= 1.0 - falloff * AsteriaVignetteStrength;
    return color;
}

technique AsteriaVignette
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AsteriaVignette;
    }
}
