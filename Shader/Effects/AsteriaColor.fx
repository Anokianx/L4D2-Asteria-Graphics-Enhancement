#include "ReShade.fxh"

uniform float AsteriaExposure < ui_type = "slider"; ui_min = -2.0; ui_max = 2.0; ui_label = "Exposure / EV"; > = 0.0;
uniform float AsteriaContrast < ui_type = "slider"; ui_min = 0.5; ui_max = 1.5; ui_label = "Contrast"; > = 1.0;
uniform float AsteriaSaturation < ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_label = "Saturation"; > = 1.0;
uniform float AsteriaTemperature < ui_type = "slider"; ui_min = -1.0; ui_max = 1.0; ui_label = "Cool / warm balance"; > = 0.0;
uniform float AsteriaVibrance < ui_type = "slider"; ui_min = -1.0; ui_max = 1.0; ui_label = "Vibrance"; > = 0.0;

float4 PS_AsteriaColor(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 source = tex2D(ReShade::BackBuffer, uv);
    float3 color = source.rgb * exp2(AsteriaExposure / 2.2);
    color *= float3(1.0 + 0.12 * AsteriaTemperature, 1.0 + 0.015 * AsteriaTemperature, 1.0 - 0.12 * AsteriaTemperature);
    color = (color - 0.5) * AsteriaContrast + 0.5;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = lerp(luminance.xxx, color, AsteriaSaturation);
    float chroma = saturate(max(max(color.r, color.g), color.b) - min(min(color.r, color.g), color.b));
    float vibrance = 1.0 + AsteriaVibrance * (1.0 - chroma);
    source.rgb = saturate(lerp(luminance.xxx, color, vibrance));
    return source;
}

technique AsteriaColor
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AsteriaColor;
    }
}
