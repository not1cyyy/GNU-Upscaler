#include "nl_common.hlsli"

#ifdef VK_MODE
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    int HalfWidth;
    int HalfHeight;
    int Mip0Width;
    int Mip0Height;

    int Mip1Width;
    int Mip1Height;
    int Mip2Width;
    int Mip2Height;

    int Mip3Width;
    int Mip3Height;
    int Mip4Width;
    int Mip4Height;

    int HasCameraBasis;
    int FrameIndex;
    int _pad0;
    int _pad1;

    float SkyDepthThreshold;
    float TanHalfFov;
    float TanHalfFovAspect;
    float _pad2;

    float3 CameraRight;
    float _padCam0;
    float3 CameraUp;
    float _padCam1;
    float3 CameraFwd;
    float _padCam2;
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
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutPyramidMip0 : register(u0);

#ifdef VK_MODE
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutPyramidMip1 : register(u1);

#ifdef VK_MODE
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutPyramidMip2 : register(u2);

#ifdef VK_MODE
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutPyramidMip3 : register(u3);

#ifdef VK_MODE
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutPyramidMip4 : register(u4);

#ifdef VK_MODE
[[vk::binding(8, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutEighthNormal : register(u5);

#ifdef VK_MODE
[[vk::binding(9, 0)]]
#endif
RWStructuredBuffer<uint> SceneLightBuf : register(u6);

groupshared float4 s_mip[16][16];
groupshared float4 s_norm[16][16];

// Reused 3 x float4[256] reduction buffers (total LDS = 20 KB < 32 KB limit)
groupshared float4 s_red0[256];
groupshared float4 s_red1[256];
groupshared float4 s_red2[256];

float4 DepthWeightedDownsample(float4 a, float4 b, float4 c, float4 d)
{
    float minD = min(min(a.a, b.a), min(c.a, d.a));
    float maxD = max(max(a.a, b.a), max(c.a, d.a));
    float invScale = rcp(max(minD * 0.25f, 0.05f));

    // Prefer matching depth cluster so sky pixels do not bleed onto foreground geometry
    float wa = (a.a >= 64000.0f && minD < 64000.0f) ? 0.01f : exp2(-abs(a.a - minD) * invScale);
    float wb = (b.a >= 64000.0f && minD < 64000.0f) ? 0.01f : exp2(-abs(b.a - minD) * invScale);
    float wc = (c.a >= 64000.0f && minD < 64000.0f) ? 0.01f : exp2(-abs(c.a - minD) * invScale);
    float wd = (d.a >= 64000.0f && minD < 64000.0f) ? 0.01f : exp2(-abs(d.a - minD) * invScale);

    float wSum = max(wa + wb + wc + wd, 1e-4f);
    float3 col = (a.rgb * wa + b.rgb * wb + c.rgb * wc + d.rgb * wd) / wSum;
    return float4(col, minD);
}

int2 ClampHalf(int2 p)
{
    return clamp(p, int2(0, 0), int2(HalfWidth - 1, HalfHeight - 1));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID,
            uint tid : SV_GroupIndex)
{
    int2 m0Coord = int2(dtid.xy);
    int2 halfBase = m0Coord * 2;
    bool inM0 = (m0Coord.x < Mip0Width && m0Coord.y < Mip0Height);

    float4 cd00 = HalfColorDepthTex.Load(int3(ClampHalf(halfBase + int2(0, 0)), 0));
    float4 cd10 = HalfColorDepthTex.Load(int3(ClampHalf(halfBase + int2(1, 0)), 0));
    float4 cd01 = HalfColorDepthTex.Load(int3(ClampHalf(halfBase + int2(0, 1)), 0));
    float4 cd11 = HalfColorDepthTex.Load(int3(ClampHalf(halfBase + int2(1, 1)), 0));

    float4 m0 = DepthWeightedDownsample(cd00, cd10, cd01, cd11);
    if (inM0)
        OutPyramidMip0[m0Coord] = m0;

    float4 n00 = HalfNormalTex.Load(int3(ClampHalf(halfBase + int2(0, 0)), 0));
    float4 n10 = HalfNormalTex.Load(int3(ClampHalf(halfBase + int2(1, 0)), 0));
    float4 n01 = HalfNormalTex.Load(int3(ClampHalf(halfBase + int2(0, 1)), 0));
    float4 n11 = HalfNormalTex.Load(int3(ClampHalf(halfBase + int2(1, 1)), 0));

    float2 nAvg = (n00.xy + n10.xy + n01.xy + n11.xy) * 0.25f;
    float fMax = max(max(n00.z, n10.z), max(n01.z, n11.z));
    float l0 = Luma(cd00.rgb);
    float l1 = Luma(cd10.rgb);
    float l2 = Luma(cd01.rgb);
    float l3 = Luma(cd11.rgb);
    float lMean = (l0 + l1 + l2 + l3) * 0.25f;
    float lVar = (abs(l0 - lMean) + abs(l1 - lMean) + abs(l2 - lMean) + abs(l3 - lMean)) * 0.25f / max(lMean + 0.1f, 0.1f);

    s_mip[gtid.y][gtid.x] = m0;
    s_norm[gtid.y][gtid.x] = float4(nAvg, fMax, lVar);

    // Accumulate Scene L1 SH & Dominant Key Light candidate per mip-0 pixel
    float4 sh0 = float4(0, 0, 0, 0);
    float4 shX = float4(0, 0, 0, 0);
    float4 shY = float4(0, 0, 0, 0);
    float4 shZ = float4(0, 0, 0, 0);
    float4 kdW = float4(0, 0, 0, 0);
    float4 kcW = float4(0, 0, 0, 0);

    if (inM0)
    {
        float2 uv = (float2(m0Coord) + 0.5f) / float2(max(Mip0Width, 1), max(Mip0Height, 1));
        float3 viewRay = normalize(ReconstructViewPos(uv, 1.0f, TanHalfFov, TanHalfFovAspect));
        float3 col = clamp(m0.rgb, 0.0f, 8.0f);
        float lum = Luma(col);
        bool isSky = (m0.a >= 64000.0f || m0.a >= SkyDepthThreshold);

        float3 dirView;
        float sampleW = 1.0f;
        if (isSky)
        {
            dirView = viewRay;
            sampleW = 1.4f;
            sh0.w = 1.0f;
        }
        else
        {
            float2 nxy = clamp(nAvg, -1.0f, 1.0f);
            dirView = normalize(float3(nxy, -sqrt(max(0.0f, 1.0f - dot(nxy, nxy)))));
        }

        float3 dir = (HasCameraBasis > 0) ? ViewToWorldDir(dirView, CameraRight, CameraUp, CameraFwd) : dirView;

        sh0.rgb = col * sampleW;
        shX = float4(col * (dir.x * sampleW), 1.0f);
        shY = float4(col * (dir.y * sampleW), 0.0f);
        shZ = float4(col * (dir.z * sampleW), 0.0f);

        // Dominant key light weighted toward bright directional highlights & bright sky regions
        float brightW = max(lum - 0.12f, 0.0f) * (isSky ? 1.8f : 1.0f);
        brightW = min(brightW * brightW, 12.0f);
        kdW = float4(dir * brightW, brightW);
        kcW = float4(col * brightW, lum);
    }

    s_red0[tid] = sh0;
    s_red1[tid] = shX;
    s_red2[tid] = shY;

    GroupMemoryBarrierWithGroupSync();

    // Mip 1 (1/8-res) + EighthNormalTex
    if ((gtid.x & 1) == 0 && (gtid.y & 1) == 0)
    {
        float4 a = s_mip[gtid.y][gtid.x];
        float4 b = s_mip[gtid.y][gtid.x + 1];
        float4 c = s_mip[gtid.y + 1][gtid.x];
        float4 d = s_mip[gtid.y + 1][gtid.x + 1];
        float4 m1 = DepthWeightedDownsample(a, b, c, d);
        s_mip[gtid.y][gtid.x] = m1;

        int2 m1Coord = m0Coord >> 1;
        if (m1Coord.x < Mip1Width && m1Coord.y < Mip1Height)
        {
            OutPyramidMip1[m1Coord] = m1;

            float4 na = s_norm[gtid.y][gtid.x];
            float4 nb = s_norm[gtid.y][gtid.x + 1];
            float4 nc = s_norm[gtid.y + 1][gtid.x];
            float4 nd = s_norm[gtid.y + 1][gtid.x + 1];
            float2 n1 = (na.xy + nb.xy + nc.xy + nd.xy) * 0.25f;
            float f1 = max(max(na.z, nb.z), max(nc.z, nd.z));
            float v1 = (na.w + nb.w + nc.w + nd.w) * 0.25f;
            OutEighthNormal[m1Coord] = float4(n1, f1, v1);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Mip 2 (1/16-res)
    if ((gtid.x & 3) == 0 && (gtid.y & 3) == 0)
    {
        float4 m2 = DepthWeightedDownsample(s_mip[gtid.y][gtid.x], s_mip[gtid.y][gtid.x + 2],
                                            s_mip[gtid.y + 2][gtid.x], s_mip[gtid.y + 2][gtid.x + 2]);
        s_mip[gtid.y][gtid.x] = m2;
        int2 m2Coord = m0Coord >> 2;
        if (m2Coord.x < Mip2Width && m2Coord.y < Mip2Height)
            OutPyramidMip2[m2Coord] = m2;
    }
    GroupMemoryBarrierWithGroupSync();

    // Mip 3 (1/32-res)
    if ((gtid.x & 7) == 0 && (gtid.y & 7) == 0)
    {
        float4 m3 = DepthWeightedDownsample(s_mip[gtid.y][gtid.x], s_mip[gtid.y][gtid.x + 4],
                                            s_mip[gtid.y + 4][gtid.x], s_mip[gtid.y + 4][gtid.x + 4]);
        s_mip[gtid.y][gtid.x] = m3;
        int2 m3Coord = m0Coord >> 3;
        if (m3Coord.x < Mip3Width && m3Coord.y < Mip3Height)
            OutPyramidMip3[m3Coord] = m3;
    }
    GroupMemoryBarrierWithGroupSync();

    // Mip 4 (1/64-res)
    if (tid == 0)
    {
        float4 m4 = DepthWeightedDownsample(s_mip[0][0], s_mip[0][8], s_mip[8][0], s_mip[8][8]);
        int2 m4Coord = m0Coord >> 4;
        if (m4Coord.x < Mip4Width && m4Coord.y < Mip4Height)
            OutPyramidMip4[m4Coord] = m4;
    }

    // Parallel reduction batch 1: sh0, shX, shY (256 -> 1)
    [unroll]
    for (uint s = 128u; s > 0u; s >>= 1u)
    {
        if (tid < s)
        {
            s_red0[tid] += s_red0[tid + s];
            s_red1[tid] += s_red1[tid + s];
            s_red2[tid] += s_red2[tid + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    float4 sumSh0 = s_red0[0];
    float4 sumShX = s_red1[0];
    float4 sumShY = s_red2[0];
    GroupMemoryBarrierWithGroupSync();

    // Parallel reduction batch 2: shZ, kdW, kcW (256 -> 1)
    s_red0[tid] = shZ;
    s_red1[tid] = kdW;
    s_red2[tid] = kcW;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint s2 = 128u; s2 > 0u; s2 >>= 1u)
    {
        if (tid < s2)
        {
            s_red0[tid] += s_red0[tid + s2];
            s_red1[tid] += s_red1[tid + s2];
            s_red2[tid] += s_red2[tid + s2];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid == 0)
    {
        float4 sumShZ = s_red0[0];
        float4 sumKdW = s_red1[0];
        float4 sumKcW = s_red2[0];

        float sc = 16.0f;
        uint dummy;
        InterlockedAdd(SceneLightBuf[0], uint(round(clamp(sumSh0.r * sc, 0.0f, 65000.0f))), dummy);
        InterlockedAdd(SceneLightBuf[1], uint(round(clamp(sumSh0.g * sc, 0.0f, 65000.0f))), dummy);
        InterlockedAdd(SceneLightBuf[2], uint(round(clamp(sumSh0.b * sc, 0.0f, 65000.0f))), dummy);
        InterlockedAdd(SceneLightBuf[3], uint(round(clamp(sumKcW.w * sc, 0.0f, 65000.0f))), dummy);
        InterlockedAdd(SceneLightBuf[4], uint(round(sumShX.w)), dummy);
        InterlockedAdd(SceneLightBuf[5], uint(round(sumSh0.w)), dummy);

        InterlockedAdd(SceneLightBuf[6],  asuint(int(round(clamp(sumShX.r * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[7],  asuint(int(round(clamp(sumShX.g * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[8],  asuint(int(round(clamp(sumShX.b * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[9],  asuint(int(round(clamp(sumShY.r * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[10], asuint(int(round(clamp(sumShY.g * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[11], asuint(int(round(clamp(sumShY.b * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[12], asuint(int(round(clamp(sumShZ.r * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[13], asuint(int(round(clamp(sumShZ.g * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[14], asuint(int(round(clamp(sumShZ.b * sc, -65000.0f, 65000.0f)))), dummy);

        InterlockedAdd(SceneLightBuf[15], asuint(int(round(clamp(sumKdW.x * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[16], asuint(int(round(clamp(sumKdW.y * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[17], asuint(int(round(clamp(sumKdW.z * sc, -65000.0f, 65000.0f)))), dummy);
        InterlockedAdd(SceneLightBuf[18], uint(round(clamp(sumKdW.w * sc, 0.0f, 65000.0f))), dummy);

        InterlockedAdd(SceneLightBuf[19], uint(round(clamp(sumKcW.r * sc, 0.0f, 65000.0f))), dummy);
        InterlockedAdd(SceneLightBuf[20], uint(round(clamp(sumKcW.g * sc, 0.0f, 65000.0f))), dummy);
        InterlockedAdd(SceneLightBuf[21], uint(round(clamp(sumKcW.b * sc, 0.0f, 65000.0f))), dummy);
    }
}
