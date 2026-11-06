texture AsteriaDepthSemantic : DEPTH;
sampler2D AsteriaDepthSampler { Texture=AsteriaDepthSemantic; };
uniform bool AsteriaDepthReady < source="bufready_depth"; > = false;
void AsteriaDepthVertex(uint vertex : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv=float2(vertex==1 ? 2 : 0,vertex==2 ? 2 : 0);
    position=float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
float4 AsteriaDepthPixel(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float value=AsteriaDepthReady ? tex2D(AsteriaDepthSampler,uv).r : 0;
    return float4(value,value,value,1);
}
technique AsteriaDepthSemanticProbe < enabled=false; >
{
    pass { VertexShader=AsteriaDepthVertex; PixelShader=AsteriaDepthPixel; }
}
