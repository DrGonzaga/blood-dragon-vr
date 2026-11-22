// smaa1: the three passes, drawn as one full-screen triangle each.
Texture2D colorTex : register(t0);
Texture2D edgesTex : register(t1);
Texture2D areaTex : register(t2);
Texture2D searchTex : register(t3);
Texture2D blendTex : register(t4);

float4 FullScreen(uint id, out float2 tc) {
    tc = float2((id << 1) & 2, id & 2);
    return float4(tc * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 ToTarget(float3 c) {
    // The game's pixels are sRGB-encoded already; an _SRGB target encodes again on write.
    if (opt.y > 0.5) c = c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4);
    return c;
}

void vsEdge(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 tc : TEXCOORD0, out float4 off[3] : TEXCOORD1) {
    pos = FullScreen(id, tc);
    SMAAEdgeDetectionVS(tc, off);
}
float4 psEdge(float4 pos : SV_Position, float2 tc : TEXCOORD0, float4 off[3] : TEXCOORD1) : SV_Target {
    return float4(SMAALumaEdgeDetectionPS(tc, off, colorTex), 0, 0);
}

void vsBlend(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 tc : TEXCOORD0, out float2 pix : TEXCOORD1,
             out float4 off[3] : TEXCOORD2) {
    pos = FullScreen(id, tc);
    SMAABlendingWeightCalculationVS(tc, pix, off);
}
float4 psBlend(float4 pos : SV_Position, float2 tc : TEXCOORD0, float2 pix : TEXCOORD1, float4 off[3] : TEXCOORD2) : SV_Target {
    return SMAABlendingWeightCalculationPS(tc, pix, off, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0));
}

void vsNb(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 tc : TEXCOORD0, out float4 off : TEXCOORD1) {
    pos = FullScreen(id, tc);
    SMAANeighborhoodBlendingVS(tc, off);
}
float4 psNb(float4 pos : SV_Position, float2 tc : TEXCOORD0, float4 off : TEXCOORD1) : SV_Target {
    return float4(ToTarget(SMAANeighborhoodBlendingPS(tc, off, colorTex, blendTex).rgb), 1);
}
