#include "ReShade.fxh"

uniform float AsteriaDebandStrength < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_label = "Deband intensity"; > = 0.3;
uniform float AsteriaDebandRadius < ui_type = "slider"; ui_min = 1.0; ui_max = 24.0; ui_label = "Sample radius / px"; > = 8.0;
uniform int AsteriaDebandQuality < ui_type = "combo"; ui_items = "Low\0Medium\0High\0"; ui_label = "Quality"; > = 1;

float3 AsteriaDebandSample(float3 center, float2 uv, float2 offset, float threshold)
{
    float3 a = tex2D(ReShade::BackBuffer, saturate(uv + offset)).rgb;
    float3 b = tex2D(ReShade::BackBuffer, saturate(uv - offset)).rgb;
    float3 difference = max(abs(a - center), abs(b - center));
    float edge = max(max(difference.r, difference.g), difference.b);
    float confidence = 1.0 - smoothstep(threshold, threshold * 2.0, edge);
    return lerp(center, (a + b) * 0.5, confidence);
}

float4 PS_AsteriaDeband(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(ReShade::BackBuffer, uv);
    float2 pixel = floor(uv * ReShade::ScreenSize);
    float rotation = frac(dot(pixel, float2(0.754877666, 0.569840296))) * 6.283185307;
    float2 direction = float2(cos(rotation), sin(rotation));
    float2 perpendicular = float2(-direction.y, direction.x);
    float2 radius = ReShade::PixelSize * AsteriaDebandRadius;
    float threshold = 0.012 + AsteriaDebandStrength * 0.032;
    float3 filtered = AsteriaDebandSample(color.rgb, uv, direction * radius, threshold);
    filtered += AsteriaDebandSample(color.rgb, uv, perpendicular * radius, threshold);
    float sample_count = 2.0;
    if (AsteriaDebandQuality >= 1)
    {
        filtered += AsteriaDebandSample(color.rgb, uv, (direction + perpendicular) * radius * 0.35355339, threshold);
        filtered += AsteriaDebandSample(color.rgb, uv, (direction - perpendicular) * radius * 0.35355339, threshold);
        sample_count += 2.0;
    }
    if (AsteriaDebandQuality >= 2)
    {
        filtered += AsteriaDebandSample(color.rgb, uv, (direction * 0.92387953 + perpendicular * 0.38268343) * radius * 0.75, threshold);
        filtered += AsteriaDebandSample(color.rgb, uv, (direction * 0.38268343 - perpendicular * 0.92387953) * radius * 0.75, threshold);
        sample_count += 2.0;
    }
    color.rgb = lerp(color.rgb, filtered / sample_count, AsteriaDebandStrength);
    return color;
}

technique AsteriaDeband
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AsteriaDeband;
    }
}
