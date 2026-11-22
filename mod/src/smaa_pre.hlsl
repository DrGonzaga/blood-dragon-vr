// smaa1: SMAA 1x (Jimenez et al., MIT licence, third_party/smaa) on each eye's
// image as it goes to the headset. This file goes before SMAA.hlsl, smaa_post.hlsl after.
cbuffer C : register(b0) {
    float4 rtm;  // SMAA_RT_METRICS: 1/w, 1/h, w, h
    float4 opt;  // y 1 = write linear (sRGB target)
};
#define SMAA_RT_METRICS rtm
// the preset (SMAA_PRESET_LOW..ULTRA) is defined by smaa.cpp when compiling
#define SMAA_CUSTOM_SL 1
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
