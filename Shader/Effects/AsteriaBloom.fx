#include "ReShade.fxh"

uniform float AsteriaBloomStrength < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_label = "Bloom intensity"; > = 0.35;
uniform float AsteriaBloomThreshold < ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_label = "Highlight threshold"; > = 0.75;
uniform float AsteriaBloomRadius < ui_type = "slider"; ui_min = 0.5; ui_max = 6.0; ui_label = "Diffusion radius"; > = 2.0;

texture AsteriaBloomHighlights { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };
texture AsteriaBloomHorizontal { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };
texture AsteriaBloomBlurred { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };

sampler AsteriaBloomHighlightSampler { Texture = AsteriaBloomHighlights; AddressU = CLAMP; AddressV = CLAMP; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler AsteriaBloomHorizontalSampler { Texture = AsteriaBloomHorizontal; AddressU = CLAMP; AddressV = CLAMP; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };
sampler AsteriaBloomBlurredSampler { Texture = AsteriaBloomBlurred; AddressU = CLAMP; AddressV = CLAMP; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = POINT; };

float3 AsteriaExtractHighlight(float2 uv)
{
    float3 color = tex2D(ReShade::BackBuffer, uv).rgb;
    float brightness = max(max(color.r, color.g), color.b);
    float knee = max(0.03, AsteriaBloomThreshold * 0.25);
    float soft = clamp(brightness - AsteriaBloomThreshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.0001);
    return color * max(soft, brightness - AsteriaBloomThreshold) / max(brightness, 0.0001);
}

float4 PS_AsteriaBloomExtract(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float2 step_uv = ReShade::PixelSize;
    float3 highlights = AsteriaExtractHighlight(uv + step_uv * float2(-1.0, -1.0));
    highlights += AsteriaExtractHighlight(uv + step_uv * float2(1.0, -1.0));
    highlights += AsteriaExtractHighlight(uv + step_uv * float2(-1.0, 1.0));
    highlights += AsteriaExtractHighlight(uv + step_uv * float2(1.0, 1.0));
    return float4(highlights * 0.25, 1.0);
}

float4 PS_AsteriaBloomHorizontal(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float2 step_uv = float2(4.0 * BUFFER_RCP_WIDTH * AsteriaBloomRadius, 0.0);
    float3 color = tex2D(AsteriaBloomHighlightSampler, uv).rgb * 0.227027;
    color += tex2D(AsteriaBloomHighlightSampler, uv + step_uv * 1.384615).rgb * 0.316216;
    color += tex2D(AsteriaBloomHighlightSampler, uv - step_uv * 1.384615).rgb * 0.316216;
    color += tex2D(AsteriaBloomHighlightSampler, uv + step_uv * 3.230769).rgb * 0.070270;
    color += tex2D(AsteriaBloomHighlightSampler, uv - step_uv * 3.230769).rgb * 0.070270;
    return float4(color, 1.0);
}

float4 PS_AsteriaBloomVertical(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float2 step_uv = float2(0.0, 4.0 * BUFFER_RCP_HEIGHT * AsteriaBloomRadius);
    float3 color = tex2D(AsteriaBloomHorizontalSampler, uv).rgb * 0.227027;
    color += tex2D(AsteriaBloomHorizontalSampler, uv + step_uv * 1.384615).rgb * 0.316216;
    color += tex2D(AsteriaBloomHorizontalSampler, uv - step_uv * 1.384615).rgb * 0.316216;
    color += tex2D(AsteriaBloomHorizontalSampler, uv + step_uv * 3.230769).rgb * 0.070270;
    color += tex2D(AsteriaBloomHorizontalSampler, uv - step_uv * 3.230769).rgb * 0.070270;
    return float4(color, 1.0);
}

float4 PS_AsteriaBloomComposite(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(ReShade::BackBuffer, uv);
    float3 glow = saturate(tex2D(AsteriaBloomBlurredSampler, uv).rgb * AsteriaBloomStrength);
    color.rgb = 1.0 - (1.0 - color.rgb) * (1.0 - glow);
    return color;
}

technique AsteriaBloom
{
    pass Extract
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AsteriaBloomExtract;
        RenderTarget = AsteriaBloomHighlights;
    }
    pass BlurHorizontal
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AsteriaBloomHorizontal;
        RenderTarget = AsteriaBloomHorizontal;
    }
    pass BlurVertical
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AsteriaBloomVertical;
        RenderTarget = AsteriaBloomBlurred;
    }
    pass Composite
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_AsteriaBloomComposite;
    }
}
