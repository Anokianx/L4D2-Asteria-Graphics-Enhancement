#include "ReShade.fxh"

uniform float AsteriaMarkerStrength <
    ui_category = "Asteria";
    ui_label = "Marker strength";
    ui_type = "slider";
    ui_min = 0.0;
    ui_max = 1.0;
    ui_step = 0.01;
> = 0.7;

float4 PS_Asteria(float4 pos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    float4 color = tex2D(ReShade::BackBuffer, texcoord);
    float2 edge_pixels = min(texcoord, 1.0 - texcoord) * ReShade::ScreenSize;
    float marker = 1.0 - smoothstep(3.0, 7.0, min(edge_pixels.x, edge_pixels.y));
    float3 tinted = color.rgb * float3(0.90, 0.98, 1.08);
    color.rgb = lerp(color.rgb, tinted, AsteriaMarkerStrength);
    color.rgb = lerp(color.rgb, float3(0.15, 0.8, 1.0), marker * AsteriaMarkerStrength);
    return color;
}

technique AsteriaMarker
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_Asteria;
    }
}
