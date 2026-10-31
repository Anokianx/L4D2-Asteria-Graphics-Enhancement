#include "ReShade.fxh"

uniform float AsteriaTonemapStrength < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_label = "Curve mix"; > = 0.5;
uniform int AsteriaTonemapMode < ui_type = "combo"; ui_items = "Filmic\0ACES\0Reinhard\0"; ui_label = "SDR tonal curve"; > = 0;

float3 AsteriaFilmicCurve(float3 color)
{
    float3 value = max(0.0, color - 0.004);
    return (value * (6.2 * value + 0.5)) / (value * (6.2 * value + 1.7) + 0.06);
}

float3 AsteriaACESCurve(float3 color)
{
    return saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
}

float4 PS_AsteriaTonemap(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(ReShade::BackBuffer, uv);
    float3 mapped;
    if (AsteriaTonemapMode == 1)
        mapped = AsteriaACESCurve(color.rgb) / AsteriaACESCurve(float3(1.0, 1.0, 1.0));
    else if (AsteriaTonemapMode == 2)
        mapped = (color.rgb / (1.0 + color.rgb)) * 2.0;
    else
        mapped = AsteriaFilmicCurve(color.rgb) / AsteriaFilmicCurve(float3(1.0, 1.0, 1.0));
    color.rgb = lerp(color.rgb, saturate(mapped), AsteriaTonemapStrength);
    return color;
}

technique AsteriaTonemap
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AsteriaTonemap;
    }
}
