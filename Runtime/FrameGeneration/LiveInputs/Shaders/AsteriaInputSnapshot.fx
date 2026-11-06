texture AsteriaInputDepth : DEPTH;
texture AsteriaInputColor : COLOR;
sampler2D AsteriaInputDepthSampler { Texture=AsteriaInputDepth; SRGBTexture=false; };
sampler2D AsteriaInputColorSampler { Texture=AsteriaInputColor; SRGBTexture=false; };
uniform bool AsteriaInputDepthReady < source="bufready_depth"; > = false;
texture2D AsteriaCapturedDepth { Width=BUFFER_WIDTH; Height=BUFFER_HEIGHT; Format=R32F; };
texture2D AsteriaCapturedColor { Width=BUFFER_WIDTH; Height=BUFFER_HEIGHT; Format=RGBA8; };
void AsteriaSnapshotVertex(uint vertex : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0)
{
    uv=float2(vertex==1 ? 2 : 0,vertex==2 ? 2 : 0);
    position=float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
float4 AsteriaSnapshotDepthPixel(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return float4(AsteriaInputDepthReady ? tex2D(AsteriaInputDepthSampler,uv).r : 0,0,0,1);
}
float4 AsteriaSnapshotColorPixel(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return tex2D(AsteriaInputColorSampler,uv);
}
technique AsteriaInputSnapshot < enabled=false; >
{
    pass { VertexShader=AsteriaSnapshotVertex; PixelShader=AsteriaSnapshotDepthPixel; RenderTarget=AsteriaCapturedDepth; }
    pass { VertexShader=AsteriaSnapshotVertex; PixelShader=AsteriaSnapshotColorPixel; RenderTarget=AsteriaCapturedColor; }
}
