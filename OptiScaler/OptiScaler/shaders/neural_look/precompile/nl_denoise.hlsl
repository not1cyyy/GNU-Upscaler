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
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutDenoisedGiTex : register(u0);

#ifdef VK_MODE
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutDenoisedSsrTex : register(u1);

#ifdef VK_MODE
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutDenoisedMetaTex : register(u2);

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

    if (centerDepth >= 64000.0f || centerDepth >= SkyDepthThreshold)
    {
        OutDenoisedGiTex[p] = centerGi;
        OutDenoisedSsrTex[p] = centerSsr;
        OutDenoisedMetaTex[p] = centerMeta;
        return;
    }

    float3 centerN = UnpackNormal(HalfNormalTex.Load(int3(p, 0)).xy);
    float centerLuma = Luma(centerGi.rgb);
    float invSigmaZ = rcp(max(centerDepth * PhiDepth, 0.02f));
    float invSigmaL = rcp(max(centerMeta.g * PhiLuma + 0.08f, 0.08f));

    static const int2 kOffsets[9] = {
        int2(0, 0),
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
        int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1)
    };
    static const float kKernel[9] = {
        0.25f,
        0.125f, 0.125f, 0.125f, 0.125f,
        0.0625f, 0.0625f, 0.0625f, 0.0625f
    };

    float4 giSum = centerGi * kKernel[0];
    float4 ssrSum = centerSsr * kKernel[0];
    float csSum = centerMeta.r * kKernel[0];
    float wSum = kKernel[0];

    int step = max(StepSize, 1);

    [unroll]
    for (int i = 1; i < 9; ++i)
    {
        int2 q = ClampGi(p + kOffsets[i] * step);
        float tapDepth = HalfColorDepthTex.Load(int3(q, 0)).a;
        if (tapDepth >= 64000.0f)
            continue;

        float3 tapN = UnpackNormal(HalfNormalTex.Load(int3(q, 0)).xy);
        float4 tapGi = InGiTex.Load(int3(q, 0));

        float wZ = exp(-abs(centerDepth - tapDepth) * invSigmaZ);
        float wN = pow(max(0.0f, dot(centerN, tapN)), PhiNormal);
        float wL = exp(-abs(centerLuma - Luma(tapGi.rgb)) * invSigmaL);
        float w = kKernel[i] * wZ * wN * wL;

        giSum += tapGi * w;
        csSum += InGiMetaTex.Load(int3(q, 0)).r * w;
        if (SsrEnabled > 0)
            ssrSum += InSsrTex.Load(int3(q, 0)) * w;
        wSum += w;
    }

    float invW = rcp(max(wSum, 1e-5f));
    OutDenoisedGiTex[p] = giSum * invW;
    OutDenoisedSsrTex[p] = (SsrEnabled > 0) ? (ssrSum * invW) : float4(0, 0, 0, 0);
    OutDenoisedMetaTex[p] = float4(csSum * invW, centerMeta.g, centerMeta.b, centerMeta.a);
}
