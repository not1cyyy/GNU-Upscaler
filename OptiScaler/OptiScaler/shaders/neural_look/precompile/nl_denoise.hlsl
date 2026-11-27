#include "nl_common.hlsli"

#ifdef VK_MODE
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    int GiWidth;
    int GiHeight;
    int StepSize;
    int SsrEnabled;

    float SkyDepthThreshold;
    float PhiDepth;
    float PhiNormal;
    float PhiLuma;

    int PassIndex;
    int _pad0;
    int _pad1;
    int _pad2;
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> HalfColorDepthTex : register(t0);

#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float4> HalfNormalTex : register(t1);

#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
Texture2D<float4> InGiTex : register(t2);

#ifdef VK_MODE
[[vk::binding(4, 0)]]
#endif
Texture2D<float4> InSsrTex : register(t3);

#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
Texture2D<float4> InGiMetaTex : register(t4);

#ifdef VK_MODE
[[vk::binding(6, 0)]]
#endif
Texture2D<float4> InGiDirTex : register(t5);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
#endif
Texture2D<float4> Material1Tex : register(t6);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
SamplerState LinearClampMat : register(s0);
#else
SamplerState LinearClampMat : register(s0);
#endif

#ifdef VK_MODE
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutDenoisedGiTex : register(u0);

#ifdef VK_MODE
[[vk::binding(9, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutDenoisedSsrTex : register(u1);

#ifdef VK_MODE
[[vk::binding(10, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutDenoisedMetaTex : register(u2);

#ifdef VK_MODE
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutDenoisedGiDirTex : register(u3);

int2 ClampGi(int2 p)
{
    return clamp(p, int2(0, 0), int2(GiWidth - 1, GiHeight - 1));
}

float3 UnpackNormal(float2 xy)
{
    float2 nxy = clamp(xy, -1.0f, 1.0f);
    return float3(nxy, -sqrt(max(0.0f, 1.0f - dot(nxy, nxy))));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if ((int) dtid.x >= GiWidth || (int) dtid.y >= GiHeight)
        return;

    int2 p = int2(dtid.xy);
    float centerDepth = HalfColorDepthTex.Load(int3(p, 0)).a;
    float4 centerGi = InGiTex.Load(int3(p, 0));
    float4 centerSsr = (SsrEnabled > 0) ? InSsrTex.Load(int3(p, 0)) : float4(0, 0, 0, 0);
    float4 centerMeta = InGiMetaTex.Load(int3(p, 0));
    float4 centerDir = InGiDirTex.Load(int3(p, 0));

    if (centerDepth >= 64000.0f || centerDepth >= SkyDepthThreshold)
    {
        OutDenoisedGiTex[p] = centerGi;
        OutDenoisedSsrTex[p] = centerSsr;
        OutDenoisedMetaTex[p] = centerMeta;
        OutDenoisedGiDirTex[p] = centerDir;
        return;
    }

    float2 uv = (float2(p) + 0.5f) / float2(max(GiWidth, 1), max(GiHeight, 1));
    float4 mat1 = Material1Tex.SampleLevel(LinearClampMat, uv, 0.0f);
    float effRoughness = saturate(mat1.r * (1.0f - 0.60f * mat1.b));

    float3 centerN = UnpackNormal(HalfNormalTex.Load(int3(p, 0)).xy);
    float centerLuma = Luma(centerGi.rgb);
    float centerSsrLuma = Luma(centerSsr.rgb);

    // 3x3 local variance pre-estimation + disocclusion kernel boost
    float varAcc = centerMeta.g * 0.40f;
    static const int2 vOff[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    [unroll]
    for (int vi = 0; vi < 4; ++vi)
        varAcc += InGiMetaTex.Load(int3(ClampGi(p + vOff[vi]), 0)).g * 0.15f;

    float histLen = max(centerMeta.a, 1.0f);
    float disoccBoost = lerp(2.2f, 1.0f, saturate((histLen - 1.0f) / 3.0f));
    float invSigmaZ = rcp(max(centerDepth * PhiDepth, 0.015f));
    float invSigmaL = rcp(max((varAcc * PhiLuma * disoccBoost) + 0.06f, 0.06f));
    float invSigmaSsr = rcp(max(centerSsrLuma * 0.65f + 0.12f, 0.12f));

    // Roughness + Hit-Distance guided SSR blur gate (mirror/contact stays crisp, rough glossy smooths)
    float ssrHitDist = max(centerMeta.b, 0.05f);
    float ssrBlur = saturate(effRoughness * 1.45f * saturate(ssrHitDist / max(centerDepth * 0.25f, 0.20f)));
    if (PassIndex > 0)
        ssrBlur *= saturate(effRoughness * 1.25f);

    static const int2 kOffsets[13] = {
        int2(0, 0),
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
        int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1),
        int2(-3, 0), int2(3, 0), int2(0, -3), int2(0, 3)
    };
    static const float kKernel[13] = {
        0.22f,
        0.10f, 0.10f, 0.10f, 0.10f,
        0.06f, 0.06f, 0.06f, 0.06f,
        0.035f, 0.035f, 0.035f, 0.035f
    };

    float4 giSum = centerGi * kKernel[0];
    float4 dirSum = centerDir * kKernel[0];
    float4 ssrSum = centerSsr * kKernel[0];
    float csSum = centerMeta.r * kKernel[0];
    float wGiSum = kKernel[0];
    float wSsrSum = kKernel[0];

    int step = max(StepSize, 1);
    float maxDepthCliff = max(centerDepth * 0.08f, 0.04f);

    [unroll]
    for (int i = 1; i < 13; ++i)
    {
        int2 q = ClampGi(p + kOffsets[i] * step);
        float tapDepth = HalfColorDepthTex.Load(int3(q, 0)).a;
        if (tapDepth >= 64000.0f || abs(centerDepth - tapDepth) > maxDepthCliff)
            continue;

        float3 tapN = UnpackNormal(HalfNormalTex.Load(int3(q, 0)).xy);
        float4 tapGi = InGiTex.Load(int3(q, 0));

        float wZ = exp(-abs(centerDepth - tapDepth) * invSigmaZ);
        float wN = pow(max(0.0f, dot(centerN, tapN)), PhiNormal);
        float wL = exp(-abs(centerLuma - Luma(tapGi.rgb)) * invSigmaL);
        float wGi = kKernel[i] * wZ * wN * wL;

        giSum += tapGi * wGi;
        dirSum += InGiDirTex.Load(int3(q, 0)) * wGi;
        csSum += InGiMetaTex.Load(int3(q, 0)).r * wGi;
        wGiSum += wGi;

        if (SsrEnabled > 0 && ssrBlur > 0.02f)
        {
            float4 tapSsr = InSsrTex.Load(int3(q, 0));
            float wLs = exp(-abs(centerSsrLuma - Luma(tapSsr.rgb)) * invSigmaSsr);
            float wSsr = kKernel[i] * wZ * wN * wLs * ssrBlur;
            ssrSum += tapSsr * wSsr;
            wSsrSum += wSsr;
        }
    }

    float invWGi = rcp(max(wGiSum, 1e-5f));
    float4 outDir = dirSum * invWGi;
    if (dot(outDir.xy, outDir.xy) > 1.0f)
        outDir.xy = normalize(outDir.xy);

    OutDenoisedGiTex[p] = giSum * invWGi;
    OutDenoisedSsrTex[p] = (SsrEnabled > 0) ? (ssrSum * rcp(max(wSsrSum, 1e-5f))) : float4(0, 0, 0, 0);
    OutDenoisedMetaTex[p] = float4(csSum * invWGi, centerMeta.g, centerMeta.b, centerMeta.a);
    OutDenoisedGiDirTex[p] = outDir;
}
