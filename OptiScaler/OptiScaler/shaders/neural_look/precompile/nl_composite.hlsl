#include "nl_common.hlsli"

#ifdef VK_MODE
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    int OutputWidth;
    int OutputHeight;
    int GiWidth;
    int GiHeight;

    int RenderWidth;
    int RenderHeight;
    int DepthWidth;
    int DepthHeight;

    int DepthSubrectBaseX;
    int DepthSubrectBaseY;
    int MvSubrectBaseX;
    int MvSubrectBaseY;

    int MotionWidth;
    int MotionHeight;
    int DisplaySizeMV;
    int LowResMV;

    int DepthIsLinear;
    int DepthIsReversed;
    int InColorEncodeMode;
    int OutColorEncodeMode;

    int GiEnabled;
    int SsrEnabled;
    int HasGBufferNormals;
    int HasGBufferAlbedo;

    int HasGBufferRoughness;
    int HasGBufferMetallic;
    int HasGBufferSubsurface;
    int HasCameraBasis;

    int WorldHashEnabled;
    int DebugView;
    int FrameIndex;
    int _pad0;

    float DepthLinearA;
    float DepthLinearB;
    float DepthLinearC;
    float SkyDepthThreshold;

    float MvScaleX;
    float MvScaleY;
    float JitterOffsetX;
    float JitterOffsetY;

    float TanHalfFov;
    float TanHalfFovAspect;
    float GiIntensity;
    float AoIntensity;

    float SsrIntensity;
    float RelightIntensity;
    float VolumetricGlow;
    float NeuralDetail;

    float SpecularRelief;
    float ToneContrast;
    float SssIntensity;
    float ContactShadowIntensity;

    float SplitScreenPos;
    float FusedRcasSharpness;
    float PreExposure;
    float _pad1;

    int EighthWidth;
    int EighthHeight;
    int _pad2;
    int _pad3;

    int ColorSubrectBaseX;
    int ColorSubrectBaseY;
    int PreUpscale;
    int _padPre;

    float3 CameraPos;
    float _padCam0;
    float3 CameraRight;
    float _padCam1;
    float3 CameraUp;
    float _padCam2;
    float3 CameraFwd;
    float _padCam3;
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> SourceColor : register(t0);

#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float> DepthTex : register(t1);

#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
Texture2D<float2> MotionTex : register(t2);

#ifdef VK_MODE
[[vk::binding(4, 0)]]
#endif
Texture2D<float4> DenoisedGiTex : register(t3);

#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
Texture2D<float4> HalfNormalTex : register(t4);

#ifdef VK_MODE
[[vk::binding(6, 0)]]
#endif
Texture2D<float4> DenoisedSsrTex : register(t5);

#ifdef VK_MODE
[[vk::binding(7, 0)]]
#endif
Texture2D<float4> DenoisedMetaTex : register(t6);

#ifdef VK_MODE
[[vk::binding(8, 0)]]
#endif
Texture2D<float4> DenoisedGiDirTex : register(t7);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]]
#endif
Texture2D<float4> ColorPyramidTex : register(t8);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]]
#endif
Texture2D<float4> Material0Tex : register(t9);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]]
#endif
Texture2D<float4> Material1Tex : register(t10);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(12, 0)]]
#endif
Texture2D<float4> Material2Tex : register(t11);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(13, 0)]]
#endif
Texture3D<float4> BilateralGrid0Tex : register(t12);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(14, 0)]]
#endif
Texture3D<float4> BilateralGrid1Tex : register(t13);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(15, 0)]]
#endif
Texture3D<float4> BilateralGrid2Tex : register(t14);

#ifdef VK_MODE
[[vk::binding(16, 0)]]
#endif
Texture2D<float4> GBufferAlbedoTex : register(t15);

#ifdef VK_MODE
[[vk::binding(17, 0)]]
#endif
Texture2D<float4> GBufferRoughnessTex : register(t16);

#ifdef VK_MODE
[[vk::binding(18, 0)]]
#endif
Texture2D<float4> GBufferMetallicTex : register(t17);

#ifdef VK_MODE
[[vk::binding(19, 0)]]
#endif
Texture2D<float4> GBufferSubsurfaceTex : register(t18);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]]
SamplerState LinearClampPyr : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]]
SamplerState LinearClampMat0 : register(s1);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]]
SamplerState LinearClampMat1 : register(s2);
[[vk::combinedImageSampler]] [[vk::binding(12, 0)]]
SamplerState LinearClampMat2 : register(s3);
[[vk::combinedImageSampler]] [[vk::binding(13, 0)]]
SamplerState LinearClampBg0 : register(s4);
[[vk::combinedImageSampler]] [[vk::binding(14, 0)]]
SamplerState LinearClampBg1 : register(s5);
[[vk::combinedImageSampler]] [[vk::binding(15, 0)]]
SamplerState LinearClampBg2 : register(s6);
#else
SamplerState LinearClamp : register(s0);
#define LinearClampPyr LinearClamp
#define LinearClampMat0 LinearClamp
#define LinearClampMat1 LinearClamp
#define LinearClampMat2 LinearClamp
#define LinearClampBg0 LinearClamp
#define LinearClampBg1 LinearClamp
#define LinearClampBg2 LinearClamp
#endif

#ifdef VK_MODE
[[vk::binding(20, 0)]]
#endif
StructuredBuffer<float4> ResolvedSceneLightBuf : register(t19);

#ifdef VK_MODE
[[vk::binding(21, 0)]]
#endif
StructuredBuffer<uint> WorldHashBuf : register(t20);

#ifdef VK_MODE
[[vk::binding(22, 0)]]
#endif
RWTexture2D<float4> DestColor : register(u0);

// Packed FP16 LDS cache (~7.6 KB total < 32 KB -> high warp occupancy)
groupshared uint2 s_color[18][18];
groupshared float s_depth[18][18];
groupshared uint2 s_halfGi[11][11];
groupshared uint2 s_halfNorm[11][11];
groupshared uint2 s_halfSsr[11][11];
groupshared uint2 s_halfDir[11][11];

int2 ClampOutputCoord(int2 p)
{
    return int2(clamp(p.x, 0, OutputWidth - 1), clamp(p.y, 0, OutputHeight - 1));
}

int2 ClampGiCoord(int2 p)
{
    return int2(clamp(p.x, 0, GiWidth - 1), clamp(p.y, 0, GiHeight - 1));
}

int2 ClampDepthCoord(int2 p)
{
    int2 minC = int2(DepthSubrectBaseX, DepthSubrectBaseY);
    int2 maxC = int2(min(DepthSubrectBaseX + RenderWidth - 1, DepthWidth - 1),
                     min(DepthSubrectBaseY + RenderHeight - 1, DepthHeight - 1));
    return clamp(p, minC, max(minC, maxC));
}

int2 ClampMotionCoord(int2 p)
{
    int activeW = (DisplaySizeMV > 0 && PreUpscale == 0) ? OutputWidth : RenderWidth;
    int activeH = (DisplaySizeMV > 0 && PreUpscale == 0) ? OutputHeight : RenderHeight;
    int2 minC = int2(MvSubrectBaseX, MvSubrectBaseY);
    int2 maxC = int2(min(MvSubrectBaseX + activeW - 1, MotionWidth - 1),
                     min(MvSubrectBaseY + activeH - 1, MotionHeight - 1));
    return clamp(p, minC, max(minC, maxC));
}

int2 UvToDepthCoord(float2 uv)
{
    float2 jitterUv = (PreUpscale > 0)
                          ? uv
                          : (uv - float2(JitterOffsetX, JitterOffsetY) / float2(max(RenderWidth, 1), max(RenderHeight, 1)));
    int2 c = int2(DepthSubrectBaseX, DepthSubrectBaseY) + int2(saturate(jitterUv) * float2(RenderWidth, RenderHeight));
    return ClampDepthCoord(c);
}

min16float3 MultiBounceAO(min16float visibility, min16float3 albedo)
{
    min16float3 a = min16float(2.0404f) * albedo - min16float(0.3324f);
    min16float3 b = min16float(-4.7951f) * albedo + min16float(0.6417f);
    min16float3 c = min16float(2.7552f) * albedo + min16float(0.6903f);
    min16float v = saturate(visibility);
    return max(v.xxx, ((v * a + b) * v + c) * v);
}

float ComputeSpecularOcclusion(float NdotV, float ao, float roughness)
{
    return saturate(pow(max(NdotV + ao, 1e-4f), exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao);
}

// Hue-preserving Subtle Filmic Toe & Soft Highlight Shoulder (prevents double-tonemapping & desert neon clipping)
float3 ApplySubtleFilmicCurve(float3 linearCol, float contrastStr)
{
    float l = max(Luma(linearCol), 0.0f);
    if (l <= 1e-5f)
        return float3(0.0f, 0.0f, 0.0f);

    // Gentle shadow toe below 0.18 + soft highlight roll-off above 1.15
    float toe = lerp(l, l * l / max(l + 0.06f * contrastStr, 1e-4f), (1.0f - smoothstep(0.02f, 0.24f, l)) * contrastStr * 0.55f);
    float shoulder = toe / (1.0f + max(toe - 1.15f, 0.0f) * contrastStr * 0.16f);
    float targetL = lerp(toe, shoulder, smoothstep(0.85f, 2.5f, toe));

    return max(linearCol * (targetL / l), 0.0f);
}

// LDS-cached Strict Depth+Normal Joint-Bilateral Upsample (zero silhouette bleed)
void BilateralUpsampleHalfRes(int2 ldsHalf, float centerDepth, float3 centerNormRef, float centerFlicker, bool sampleSsr,
                              out float4 outGi, out float4 outSsr,
                              out float outFlicker, out float3 outViewNormal, out float3 outBentNormal, out float3 outGiDir)
{
    float invSigmaLog2 = 1.442695f / max(centerDepth * 0.028f, 0.015f);
    float maxDepthCliff = max(centerDepth * 0.08f, 0.04f);

    static const int2 offsets[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    static const float weights[5] = { 0.36f, 0.16f, 0.16f, 0.16f, 0.16f };

    float4 giAccum = float4(0, 0, 0, 0);
    float4 ssrAccum = float4(0, 0, 0, 0);
    float3 normAccum = float3(0, 0, 0);
    float3 bentAccum = float3(0, 0, 0);
    float3 dirAccum = float3(0, 0, 0);
    float wSum = 0.0f;
    float maxNFlick = 0.0f;
    float sumNFlick = 0.0f;

    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        int2 hp = ldsHalf + offsets[i];
        float4 giTap = UnpackFloat4Half(s_halfGi[hp.y][hp.x]);
        float4 hNormTap = UnpackFloat4Half(s_halfNorm[hp.y][hp.x]);
        float4 dirTap = UnpackFloat4Half(s_halfDir[hp.y][hp.x]);

        float2 tn = clamp(hNormTap.xy, -1.0f, 1.0f);
        float3 tNorm = float3(tn, -sqrt(max(0.0f, 1.0f - dot(tn, tn))));

        float w = weights[i];
        if (i > 0)
        {
            float dDiff = abs(centerDepth - hNormTap.w);
            if (dDiff > maxDepthCliff)
                w = 0.0f;
            else
            {
                float wZ = exp2(-dDiff * invSigmaLog2);
                float wN = pow(max(0.0f, dot(centerNormRef, tNorm)), 12.0f);
                w *= wZ * wN;
            }
            float tf = saturate(hNormTap.z);
            maxNFlick = max(maxNFlick, tf);
            sumNFlick += tf;
        }
        else
        {
            w += 1e-5f;
        }

        float2 bn = clamp(dirTap.xy, -1.0f, 1.0f);
        float3 bNorm = float3(bn, -sqrt(max(0.0f, 1.0f - dot(bn, bn))));
        float3 gDir = OctDecode(dirTap.zw);

        giAccum += giTap * w;
        normAccum += tNorm * w;
        bentAccum += bNorm * w;
        dirAccum += gDir * w;
        if (sampleSsr)
            ssrAccum += UnpackFloat4Half(s_halfSsr[hp.y][hp.x]) * w;
        wSum += w;
    }

    float invW = rcp(max(wSum, 1e-5f));
    outGi = giAccum * invW;
    outSsr = sampleSsr ? (ssrAccum * invW) : float4(0, 0, 0, 0);
    outFlicker = saturate(max(centerFlicker, maxNFlick * 0.65f + (sumNFlick * 0.25f) * 0.35f));
    outViewNormal = (dot(normAccum, normAccum) > 1e-6f) ? normalize(normAccum) : centerNormRef;
    outBentNormal = (dot(bentAccum, bentAccum) > 1e-6f) ? normalize(bentAccum) : outViewNormal;
    outGiDir = (dot(dirAccum, dirAccum) > 1e-6f) ? normalize(dirAccum) : outBentNormal;
}

// Fast-path bilinear on flat interior pixels; 4-tap Depth+Chroma joint-bilateral only near silhouettes
void UpsampleEighthMaterials(float2 uv, float centerDepth, float3 centerColor, float silEdge,
                             out float4 outMat0, out float4 outMat1, out float4 outMat2)
{
    if (silEdge < 0.04f)
    {
        outMat0 = Material0Tex.SampleLevel(LinearClampMat0, uv, 0.0f);
        outMat1 = Material1Tex.SampleLevel(LinearClampMat1, uv, 0.0f);
        outMat2 = Material2Tex.SampleLevel(LinearClampMat2, uv, 0.0f);
        return;
    }

    int maxX = max(EighthWidth - 1, 0);
    int maxY = max(EighthHeight - 1, 0);
    float2 fCoord = uv * float2(max(EighthWidth, 1), max(EighthHeight, 1)) - 0.5f;
    int2 baseC = int2(floor(fCoord));
    float2 f = frac(fCoord);

    float wBilin[4] = {
        (1.0f - f.x) * (1.0f - f.y),
        f.x * (1.0f - f.y),
        (1.0f - f.x) * f.y,
        f.x * f.y
    };
    int2 off[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

    float3 cNorm = centerColor / max(Luma(centerColor) + 0.08f, 0.08f);
    float invSigmaZ = 1.442695f / max(centerDepth * 0.055f, 0.03f);

    float4 m0Acc = float4(0, 0, 0, 0);
    float4 m1Acc = float4(0, 0, 0, 0);
    float4 m2Acc = float4(0, 0, 0, 0);
    float wSum = 0.0f;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        int2 c8 = clamp(baseC + off[i], int2(0, 0), int2(maxX, maxY));
        float2 uv8 = (float2(c8) + 0.5f) / float2(max(EighthWidth, 1), max(EighthHeight, 1));
        float4 pyr1 = ColorPyramidTex.SampleLevel(LinearClampPyr, uv8, 1.0f);
        float d8 = pyr1.a;
        float3 tNorm = max(pyr1.rgb, 0.0f) / max(Luma(max(pyr1.rgb, 0.0f)) + 0.08f, 0.08f);

        float wZ = (d8 >= 64000.0f) ? 0.01f : exp2(-abs(centerDepth - d8) * invSigmaZ);
        float wC = exp2(-dot(cNorm - tNorm, cNorm - tNorm) * 6.0f);
        float w = wBilin[i] * wZ * wC + 1e-4f;

        m0Acc += Material0Tex.Load(int3(c8, 0)) * w;
        m1Acc += Material1Tex.Load(int3(c8, 0)) * w;
        m2Acc += Material2Tex.Load(int3(c8, 0)) * w;
        wSum += w;
    }

    float invW = rcp(max(wSum, 1e-5f));
    outMat0 = m0Acc * invW;
    outMat1 = m1Acc * invW;
    outMat2 = m2Acc * invW;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    int2 groupBase = int2(gid.xy) * 16 - 1;
    int2 colorBase = int2(ColorSubrectBaseX, ColorSubrectBaseY);
    uint linearIdx = gtid.y * 16u + gtid.x;
    float2 invOut = 1.0f / float2(max(OutputWidth, 1), max(OutputHeight, 1));
    float2 outToGi = float2(GiWidth, GiHeight) * invOut;
    float preExp = max(PreExposure, 1e-4f);
    float invPreExp = rcp(preExp);

    // Populate 18x18 color + depth (packed FP16 color + linear depth)
    [unroll]
    for (uint passIdx = 0u; passIdx < 2u; ++passIdx)
    {
        uint idx = linearIdx + passIdx * 256u;
        if (idx < 324u)
        {
            uint sx = idx % 18u;
            uint sy = idx / 18u;
            int2 p = ClampOutputCoord(groupBase + int2(sx, sy));
            float4 raw = SourceColor.Load(int3(colorBase + p, 0));
            s_color[sy][sx] = PackFloat4Half(float4(EncodeColor(raw.rgb, InColorEncodeMode) * invPreExp, raw.a));

            float2 pUv = (float2(p) + 0.5f) * invOut;
            float rawD = DepthTex.Load(int3(UvToDepthCoord(pUv), 0)).r;
            s_depth[sy][sx] = LinearizeDepthValue(rawD, DepthIsLinear, DepthIsReversed,
                                                  DepthLinearA, DepthLinearB, DepthLinearC, SkyDepthThreshold);
        }
    }

    // Populate 11x11 half-res GI/Normal/SSR/Dir LDS cache
    int2 halfTileBase = int2((float2(gid.xy) * 16.0f + 0.5f) * outToGi) - 1;
    if (linearIdx < 121u)
    {
        uint hx = linearIdx % 11u;
        uint hy = linearIdx / 11u;
        int2 hc = ClampGiCoord(halfTileBase + int2(hx, hy));
        s_halfGi[hy][hx] = PackFloat4Half(DenoisedGiTex.Load(int3(hc, 0)));
        s_halfNorm[hy][hx] = PackFloat4Half(HalfNormalTex.Load(int3(hc, 0)));
        s_halfSsr[hy][hx] = PackFloat4Half(DenoisedSsrTex.Load(int3(hc, 0)));
        s_halfDir[hy][hx] = PackFloat4Half(DenoisedGiDirTex.Load(int3(hc, 0)));
    }
    GroupMemoryBarrierWithGroupSync();

    if ((int) dtid.x >= OutputWidth || (int) dtid.y >= OutputHeight)
        return;

    int2 outCoord = int2(dtid.xy);
    int2 destCoord = colorBase + outCoord;
    int2 ldsCoord = int2(gtid.xy) + 1;
    float2 uv = (float2(outCoord) + 0.5f) * invOut;

    float4 centerPacked = UnpackFloat4Half(s_color[ldsCoord.y][ldsCoord.x]);
    float3 centerColor = centerPacked.rgb;
    float origAlpha = centerPacked.a;

    if (SplitScreenPos > 0.01f && SplitScreenPos < 0.99f)
    {
        int splitX = int(SplitScreenPos * float(OutputWidth));
        if (abs(outCoord.x - splitX) <= 1)
        {
            DestColor[destCoord] = float4(EncodeColor(float3(1.0f, 1.0f, 1.0f), OutColorEncodeMode), origAlpha);
            return;
        }
        if (outCoord.x < splitX)
        {
            DestColor[destCoord] = SourceColor.Load(int3(destCoord, 0));
            return;
        }
    }

    float centerDepth = s_depth[ldsCoord.y][ldsCoord.x];
    float skyMask = 1.0f - smoothstep(SkyDepthThreshold * 0.65f, SkyDepthThreshold, centerDepth);
    if (centerDepth >= 64000.0f)
        skyMask = 0.0f;

    float3 cL  = UnpackFloat4Half(s_color[ldsCoord.y][ldsCoord.x - 1]).rgb;
    float3 cR  = UnpackFloat4Half(s_color[ldsCoord.y][ldsCoord.x + 1]).rgb;
    float3 cU  = UnpackFloat4Half(s_color[ldsCoord.y - 1][ldsCoord.x]).rgb;
    float3 cD  = UnpackFloat4Half(s_color[ldsCoord.y + 1][ldsCoord.x]).rgb;
    float3 cUL = UnpackFloat4Half(s_color[ldsCoord.y - 1][ldsCoord.x - 1]).rgb;
    float3 cUR = UnpackFloat4Half(s_color[ldsCoord.y - 1][ldsCoord.x + 1]).rgb;
    float3 cDL = UnpackFloat4Half(s_color[ldsCoord.y + 1][ldsCoord.x - 1]).rgb;
    float3 cDR = UnpackFloat4Half(s_color[ldsCoord.y + 1][ldsCoord.x + 1]).rgb;

    float centerLuma = Luma(centerColor);
    float lumaL = Luma(cL), lumaR = Luma(cR), lumaU = Luma(cU), lumaD = Luma(cD);
    float lumaUL = Luma(cUL), lumaUR = Luma(cUR), lumaDL = Luma(cDL), lumaDR = Luma(cDR);

    float dL  = s_depth[ldsCoord.y][ldsCoord.x - 1];
    float dR  = s_depth[ldsCoord.y][ldsCoord.x + 1];
    float dU  = s_depth[ldsCoord.y - 1][ldsCoord.x];
    float dD  = s_depth[ldsCoord.y + 1][ldsCoord.x];
    float dUL = s_depth[ldsCoord.y - 1][ldsCoord.x - 1];
    float dUR = s_depth[ldsCoord.y - 1][ldsCoord.x + 1];
    float dDL = s_depth[ldsCoord.y + 1][ldsCoord.x - 1];
    float dDR = s_depth[ldsCoord.y + 1][ldsCoord.x + 1];

    // Anti-Halo Edge Guards (no outlines or glow on silhouettes)
    float minND = min(centerDepth, min(min(min(dL, dR), min(dU, dD)), min(min(dUL, dUR), min(dDL, dDR))));
    float maxND = max(centerDepth, max(max(max(dL, dR), max(dU, dD)), max(max(dUL, dUR), max(dDL, dDR))));
    float maxRelDepthJump = (maxND - minND) / max(minND, 0.05f);
    float silEdge = smoothstep(0.035f, 0.14f, maxRelDepthJump);
    float fgSilEdge = smoothstep(0.08f, 0.25f, (maxND - centerDepth) / max(centerDepth, 0.05f));

    // Scene Key Light & World Up
    float3 sh0 = max(ResolvedSceneLightBuf[0].rgb, 0.01f);
    float3 shX = ResolvedSceneLightBuf[1].rgb;
    float3 shY = ResolvedSceneLightBuf[2].rgb;
    float3 shZ = ResolvedSceneLightBuf[3].rgb;
    float4 keyDirData = ResolvedSceneLightBuf[4];
    float3 keyDirView = normalize(keyDirData.xyz);
    float keyStrength = saturate(keyDirData.w);
    float3 keyColor = max(ResolvedSceneLightBuf[5].rgb, 0.05f);
    float3 keyTint = clamp(keyColor / max(Luma(keyColor), 1e-4f), 0.72f, 1.30f);
    float3 worldUpInView = normalize(ResolvedSceneLightBuf[6].xyz);

    float3 viewPos = ReconstructViewPos(uv, centerDepth, TanHalfFov, TanHalfFovAspect);
    float3 viewDir = normalize(viewPos);
    float3 toEye = -viewDir;

    // Pure Distant Beer-Lambert Aerial Perspective (> 25m only, zero near bloom/outline halo!)
    float aerialFactor = 0.0f;
    float3 aerialTint = sh0 * 0.30f;
    if (VolumetricGlow > 1e-4f && centerDepth < 64000.0f)
    {
        float farDist = max(centerDepth - 25.0f, 0.0f);
        aerialFactor = saturate(1.0f - exp(-farDist * 0.0028f)) * min(VolumetricGlow * 0.10f, 0.12f) * skyMask;
    }

    // Fast skybox early-out
    if (skyMask <= 1e-4f && DebugView == 0)
    {
        DestColor[destCoord] = float4(EncodeColor(max(centerColor * preExp, 0.0f), OutColorEncodeMode), origAlpha);
        return;
    }

    int2 halfCoord = ClampGiCoord(int2((float2(outCoord) + 0.5f) * outToGi));
    int2 ldsHalf = clamp(halfCoord - halfTileBase, int2(1, 1), int2(9, 9));
    float4 hNormCenter = UnpackFloat4Half(s_halfNorm[ldsHalf.y][ldsHalf.x]);
    float2 nxy = clamp(hNormCenter.xy, -1.0f, 1.0f);
    float3 viewNormal = float3(nxy, -sqrt(max(0.0f, 1.0f - dot(nxy, nxy))));
    float centerFlicker = saturate(hNormCenter.z);

    // Depth-aware neighborhood averages (zero cross-silhouette haloing in micro-gradients)
    float invDepthSig = 1.442695f / max(centerDepth * 0.025f, 0.015f);
    float wdL  = exp2(-abs(dL  - centerDepth) * invDepthSig);
    float wdR  = exp2(-abs(dR  - centerDepth) * invDepthSig);
    float wdU  = exp2(-abs(dU  - centerDepth) * invDepthSig);
    float wdD  = exp2(-abs(dD  - centerDepth) * invDepthSig);
    float wdUL = exp2(-abs(dUL - centerDepth) * invDepthSig);
    float wdUR = exp2(-abs(dUR - centerDepth) * invDepthSig);
    float wdDL = exp2(-abs(dDL - centerDepth) * invDepthSig);
    float wdDR = exp2(-abs(dDR - centerDepth) * invDepthSig);

    float pC  = sqrt(max(centerLuma, 0.0f));
    float pL  = sqrt(max(lumaL,  0.0f)), pR  = sqrt(max(lumaR,  0.0f));
    float pU  = sqrt(max(lumaU,  0.0f)), pD  = sqrt(max(lumaD,  0.0f));
    float pUL = sqrt(max(lumaUL, 0.0f)), pUR = sqrt(max(lumaUR, 0.0f));
    float pDL = sqrt(max(lumaDL, 0.0f)), pDR = sqrt(max(lumaDR, 0.0f));

    float wPSum = wdL + wdR + wdU + wdD + wdUL + wdUR + wdDL + wdDR + 1e-4f;
    float pAvg = (pL * wdL + pR * wdR + pU * wdU + pD * wdD +
                  pUL * wdUL + pUR * wdUR + pDL * wdDL + pDR * wdDR + pC * 1e-4f) / wPSum;
    float pVar = (abs(pL - pAvg) * wdL + abs(pR - pAvg) * wdR + abs(pU - pAvg) * wdU + abs(pD - pAvg) * wdD) /
                 max(wdL + wdR + wdU + wdD + 1e-4f, 1e-4f);

    float meshH = abs(2.0f * pC - pL - pR) * min(wdL, wdR);
    float meshV = abs(2.0f * pC - pU - pD) * min(wdU, wdD);
    float meshEnergy = max(meshH, meshV) + pVar * 1.5f;
    float meshGuard = rcp(1.0f + meshEnergy * meshEnergy * 32.0f);
    float2 microGrad = clamp(float2((pR - pL) * min(wdL, wdR), (pD - pU) * min(wdU, wdD)) /
                             max(pC + 0.18f, 0.18f), -0.85f, 0.85f);

    // Joint-bilateral LDS upsample of GI, SSR, ViewNormal, BentNormal & GI Dir
    float3 giBounce = float3(0, 0, 0);
    float contactAo = 1.0f;
    float contactShadow = 1.0f;
    float4 ssrSample = float4(0, 0, 0, 0);
    float flickerSoft = centerFlicker;
    float3 bentNormal = viewNormal;
    float3 giDir = viewNormal;

    if (GiEnabled > 0)
    {
        float4 giSample;
        bool needSsr = (SsrEnabled > 0 && SsrIntensity > 1e-4f) || (DebugView == 5);
        BilateralUpsampleHalfRes(ldsHalf, centerDepth, viewNormal, centerFlicker, needSsr,
                                 giSample, ssrSample, flickerSoft, viewNormal, bentNormal, giDir);
        giBounce = max(giSample.rgb, 0.0f);
        contactAo = lerp(1.0f, saturate(giSample.a), saturate(AoIntensity) * skyMask * (1.0f - flickerSoft));
        float rawCs = saturate(DenoisedMetaTex.Load(int3(halfCoord, 0)).r);
        contactShadow = lerp(1.0f, rawCs, saturate(ContactShadowIntensity) * skyMask * (1.0f - flickerSoft));
    }
    else
    {
        static const int2 dOffsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        float invSigmaLog2 = 1.442695f / max(centerDepth * 0.028f, 0.015f);
        float3 normAccum = viewNormal * 0.36f;
        float maxNF = 0.0f, sumNF = 0.0f;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            int2 hp = ldsHalf + dOffsets[i];
            float4 hTap = UnpackFloat4Half(s_halfNorm[hp.y][hp.x]);
            float tf = saturate(hTap.z);
            maxNF = max(maxNF, tf);
            sumNF += tf;
            float dDiff = abs(centerDepth - hTap.w);
            float wn = (dDiff > max(centerDepth * 0.08f, 0.04f)) ? 0.0f : (0.16f * exp2(-dDiff * invSigmaLog2));
            float2 tn = clamp(hTap.xy, -1.0f, 1.0f);
            normAccum += float3(tn, -sqrt(max(0.0f, 1.0f - dot(tn, tn)))) * wn;
        }
        viewNormal = (dot(normAccum, normAccum) > 1e-6f) ? normalize(normAccum) : viewNormal;
        bentNormal = viewNormal;
        giDir = viewNormal;
        flickerSoft = saturate(max(centerFlicker, maxNF * 0.65f + (sumNF * 0.25f) * 0.35f));
    }

    float NdotV = saturate(dot(toEye, viewNormal));
    float safeNdotV = lerp(NdotV, max(NdotV, 0.45f), silEdge);
    float bentNdotV = saturate(dot(toEye, bentNormal));
    float solidMask = skyMask * (1.0f - flickerSoft);
    float edgeSafeMask = solidMask * (1.0f - silEdge) * (1.0f - fgSilEdge);
    float detailGuard = solidMask * (1.0f - silEdge * 0.90f);

    // Upsample 1/8-res Semantic Materials (fast hardware bilinear inside surfaces, bilateral at silhouettes)
    float4 mat0, mat1, mat2;
    UpsampleEighthMaterials(uv, centerDepth, centerColor, silEdge, mat0, mat1, mat2);

    float3 albedo = saturate(mat0.rgb);
    float directMask = saturate(mat0.a);
    float roughness = saturate(mat1.r);
    float metallic = saturate(mat1.g);
    float wetness = saturate(mat1.b);
    float skinProb = saturate(mat1.a);
    float fabricProb = saturate(mat2.x);
    float sandProb = saturate(mat2.y);
    float soilProb = saturate(mat2.z);
    float grassProb = saturate(mat2.w);
    float hardProb = saturate(1.0f - (skinProb + fabricProb + sandProb + soilProb + grassProb));

    int2 gCoord = UvToDepthCoord(uv);
    if (HasGBufferAlbedo > 0)
        albedo = saturate(GBufferAlbedoTex.Load(int3(gCoord, 0)).rgb);
    if (HasGBufferRoughness > 0)
        roughness = saturate(GBufferRoughnessTex.Load(int3(gCoord, 0)).r);
    if (HasGBufferMetallic > 0)
        metallic = saturate(GBufferMetallicTex.Load(int3(gCoord, 0)).r);
    if (HasGBufferSubsurface > 0)
        skinProb = max(skinProb, saturate(GBufferSubsurfaceTex.Load(int3(gCoord, 0)).r));

    float maxSem = max(max(max(skinProb, fabricProb), max(sandProb, soilProb)), max(grassProb, hardProb));
    float confGate = smoothstep(0.28f, 0.65f, maxSem);
    float skinW = smoothstep(0.32f, 0.72f, skinProb) * confGate;
    float fabricW = smoothstep(0.32f, 0.72f, fabricProb) * confGate;
    float sandW = smoothstep(0.32f, 0.72f, sandProb) * confGate;
    float soilW = smoothstep(0.32f, 0.72f, soilProb) * confGate;
    float grassW = smoothstep(0.32f, 0.72f, grassProb) * confGate;
    float wetSoilW = soilW * wetness;

    float effRoughness = lerp(roughness, 0.14f, wetSoilW);
    float smoothness = saturate(1.0f - effRoughness);

    float3 shadedNormal = viewNormal;
    if (HasGBufferNormals == 0)
    {
        float microStrength = (soilW * 0.22f + sandW * 0.16f + hardProb * 0.10f + fabricW * 0.08f) *
                              (1.0f - skinW) * (1.0f - grassW) * edgeSafeMask * meshGuard;
        if (microStrength > 1e-4f)
            shadedNormal = normalize(viewNormal + float3(-microGrad.x, microGrad.y, 0.0f) * microStrength);
    }

    // Lean Depth-Guarded Skin Subsurface Scattering (LDS 4-tap + 1 mip tap, capped luma)
    float3 litColor = centerColor;
    if (skinProb > 0.08f && SssIntensity > 1e-4f && solidMask > 0.05f)
    {
        float sssInvSigma = rcp(max(centerDepth * 0.024f, 0.015f));
        float3 sumCol = centerColor * 0.36f;
        float wSssSum = 0.36f;

        static const int2 sssTapOff[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        [unroll]
        for (int st = 0; st < 4; ++st)
        {
            int2 sp = ldsCoord + sssTapOff[st];
            float sd = s_depth[sp.y][sp.x];
            float3 sc = UnpackFloat4Half(s_color[sp.y][sp.x]).rgb;
            float sw = exp(-abs(centerDepth - sd) * sssInvSigma) * exp(-length(sc - centerColor) * 1.5f) * 0.16f;
            sumCol += sc * sw;
            wSssSum += sw;
        }
        float3 ring1 = sumCol / max(wSssSum, 1e-4f);

        float sssPyrSigma = rcp(max(centerDepth * 0.032f, 0.020f));
        float4 pyr0 = ColorPyramidTex.SampleLevel(LinearClampPyr, uv, 0.0f);
        float w2 = exp(-abs(centerDepth - pyr0.a) * sssPyrSigma) * (1.0f - silEdge) * (1.0f - fgSilEdge);
        float3 ring2 = lerp(ring1, max(pyr0.rgb, 0.0f), w2);

        float3 diffusedSkin = float3(
            lerp(ring1.r, ring2.r, 0.65f),
            lerp(ring1.g, ring2.g, 0.40f),
            lerp(centerColor.b, ring1.b, 0.50f)
        ) * float3(1.04f, 0.99f, 0.97f);

        float diffLuma = Luma(diffusedSkin);
        if (diffLuma > centerLuma * 1.12f + 0.01f)
            diffusedSkin *= (centerLuma * 1.12f + 0.01f) / max(diffLuma, 1e-4f);

        float3 rgbFalloff = float3(0.75f, 0.38f, 0.16f) * saturate(skinProb * SssIntensity * 0.85f) *
                            solidMask * (1.0f - silEdge * 0.85f);
        litColor = lerp(litColor, max(diffusedSkin, 0.0f), rgbFalloff);
    }

    // Wet soil diffuse darkening before GI/AO
    litColor *= lerp(1.0f, 0.74f, wetSoilW * solidMask);

    // Multi-Bounce AO + Contact Shadow + Sunlit-Equilibrium GI (zero desert orange wash)
    float specOcc = 1.0f;
    if (GiEnabled > 0)
    {
        float indirectWeight = saturate(1.0f - directMask * 0.68f) * (1.0f - skinW * 0.55f);

        // Conservative energy-bounded ambient de-lighting in shadowed regions
        float3 ambientFloor = min(litColor * 0.34f, albedo * max(Luma(sh0) * 0.28f, 0.030f)) *
                              indirectWeight * (1.0f - silEdge * 0.50f) *
                              saturate(GiIntensity * 0.45f + AoIntensity * 0.25f);
        litColor = max(litColor - ambientFloor * solidMask, 0.0f);

        float aoFloor = lerp(0.18f, 0.32f, sandW);
        float effectiveAo = lerp(1.0f, max(contactAo, aoFloor), indirectWeight);
        float3 coloredAo = float3(MultiBounceAO(min16float(effectiveAo), min16float3(albedo)));
        specOcc = ComputeSpecularOcclusion(bentNdotV, effectiveAo, effRoughness);
        float csFactor = lerp(1.0f, max(contactShadow, 0.35f), 0.80f * solidMask);
        litColor = litColor * lerp(float3(1, 1, 1), coloredAo, solidMask) * csFactor;

        float dirGiShade = 0.58f + 0.42f * saturate(dot(shadedNormal, giDir));
        float sunAtten = (1.0f - directMask * 0.72f) * rcp(1.0f + centerLuma * 1.35f);
        float3 bounceAlbedo = max(clamp(albedo, 0.05f, 0.88f),
                                  float3(0.12f, 0.12f, 0.12f) * (1.0f - metallic * 0.50f));

        litColor += giBounce * bounceAlbedo * dirGiShade * (1.0f - metallic * 0.50f) *
                    (GiIntensity * (0.55f + 0.85f * sunAtten)) * solidMask;

        // Subtle Bent-Normal Sky Fill in shadows
        float3 bentWorld = (HasCameraBasis > 0) ? ViewToWorldDir(bentNormal, CameraRight, CameraUp, CameraFwd) : bentNormal;
        float3 skyFill = max(sh0 + shX * bentWorld.x + shY * bentWorld.y + shZ * bentWorld.z, 0.0f);
        litColor += skyFill * albedo * effectiveAo * indirectWeight * (0.14f * GiIntensity) * solidMask * (1.0f - silEdge * 0.4f);

        // Indirect specular sheen (strictly interior-only via edgeSafeMask)
        float3 indSpecF0 = lerp(float3(0.04f, 0.04f, 0.04f), bounceAlbedo, metallic);
        float indFresnel = 0.25f + 0.75f * pow(1.0f - safeNdotV, 4.0f);
        litColor += giBounce * indSpecF0 * indFresnel * pow(smoothness, 1.5f) * specOcc *
                    (1.0f - skinW * 0.80f) * (1.0f - fabricW * 0.75f) * (GiIntensity * 0.42f) * edgeSafeMask;
    }

    // Ray-Traced Grazing & Metallic / Wet-Soil SSR (gated by edgeSafeMask)
    if (GiEnabled > 0 && SsrEnabled > 0 && SsrIntensity > 1e-4f && effRoughness < 0.80f)
    {
        float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
        f0 = lerp(f0, float3(0.028f, 0.028f, 0.028f), skinW);
        float fresnel = pow(1.0f - safeNdotV, 5.0f);
        float3 specF = f0 + (1.0f - f0) * fresnel;
        float glossGate = pow(smoothness, 1.55f) * (1.0f - skinW * 0.85f) * (1.0f - fabricW * 0.78f);
        float reflWeight = saturate((max(specF.r, max(specF.g, specF.b)) * 1.35f + metallic * 0.24f + wetSoilW * 0.28f) *
                                    glossGate * SsrIntensity * 1.25f) *
                           edgeSafeMask * specOcc * saturate(ssrSample.a);
        float3 reflTint = lerp(float3(1, 1, 1), albedo, metallic * 0.70f);
        litColor = lerp(litColor, ssrSample.rgb * reflTint, reflWeight);
    }

    // Scene L1 SH 3D Sculptural Relighting + Material-Specific Micro-BRDFs (ZERO silhouette outline glow!)
    if (RelightIntensity > 1e-4f)
    {
        float3 shNormal = (HasCameraBasis > 0) ? ViewToWorldDir(bentNormal, CameraRight, CameraUp, CameraFwd) : bentNormal;
        float3 shDirRad = max(sh0 + shX * shNormal.x + shY * shNormal.y + shZ * shNormal.z, 0.0f);
        float shMeanL = max(Luma(sh0), 0.04f);
        float3 shRatio = clamp(shDirRad / shMeanL, 0.55f, 1.50f) - 1.0f;

        float rawKeyDot = dot(shadedNormal, keyDirView);
        float wrapKeyDot = (rawKeyDot + 0.20f * fabricW) / (1.0f + 0.20f * fabricW);
        float macroKey = (wrapKeyDot - 0.10f) * 0.11f * keyStrength;
        float hemiTerm = dot(shadedNormal, worldUpInView) * 0.12f;
        float3 sculptTint = (macroKey + hemiTerm >= 0.0f) ? keyTint : float3(0.90f, 0.95f, 1.06f);
        float3 sculptVec = (shRatio * 0.18f + (macroKey + hemiTerm) * sculptTint) * RelightIntensity * solidMask;
        litColor *= clamp(1.0f + sculptVec, 0.50f, 1.38f);

        // 1) FABRIC: Charlie / Estevez-Kulla grazing velvet sheen + fold depth
        if (fabricW > 0.04f)
        {
            float invSin = rcp(max(sqrt(max(1.0f - safeNdotV * safeNdotV, 0.01f)), 0.15f));
            float charlie = saturate(pow(1.0f - safeNdotV, 3.2f) * (0.65f + 0.35f * invSin) * 0.42f);
            float sheen = charlie * fabricW * RelightIntensity * edgeSafeMask * specOcc * 0.18f;
            litColor += lerp(albedo, keyTint, 0.35f) * sheen * (centerLuma * 0.80f + 0.04f);
        }

        // 2) SAND: Oren-Nayar backscatter + sunlit sand chroma restraint + sparse quartz micro-glints
        if (sandW > 0.04f)
        {
            float nDotK = saturate(dot(shadedNormal, keyDirView));
            float orenNayarBoost = (0.12f * (1.0f - safeNdotV) * (0.4f + 0.6f * nDotK)) * sandW * RelightIntensity * edgeSafeMask;
            litColor *= (1.0f + orenNayarBoost);

            // Prevent sunlit desert sand from oversaturating into orange
            float curL = Luma(litColor);
            float curChroma = max(litColor.r, max(litColor.g, litColor.b)) - min(litColor.r, min(litColor.g, litColor.b));
            float excessChroma = saturate((curChroma - 0.18f) * 2.5f) * saturate((sandW - 0.10f) * directMask * 0.16f);
            litColor = lerp(litColor, curL.xxx, excessChroma);

            if (HasCameraBasis > 0 && centerDepth < 14.0f)
            {
                float3 wPos = ViewToWorldPos(viewPos, CameraPos, CameraRight, CameraUp, CameraFwd);
                int3 grainCell = int3(floor(wPos * 68.0f));
                uint gh = Hash3UInt(grainCell);
                float glintRand = float(gh & 0x3FFu) / 1023.0f;
                if (glintRand > 0.968f)
                {
                    float3 halfVec = normalize(keyDirView + toEye);
                    float glintAlign = pow(saturate(dot(shadedNormal, halfVec)), 30.0f);
                    float distFade = 1.0f - smoothstep(4.0f, 14.0f, centerDepth);
                    litColor += keyTint * glintAlign * distFade * sandW * edgeSafeMask * 0.45f * (glintRand - 0.968f) / 0.032f;
                }
            }
        }

        // 3) SOIL: Clumpy micro-cavity relief
        if (soilW > 0.04f)
        {
            float microCav = saturate(-(pC - pAvg) / max(pAvg + 0.10f, 0.10f));
            litColor *= (1.0f - microCav * soilW * (1.0f - wetSoilW) * 0.16f * edgeSafeMask);
        }

        // 4) GRASS / FOLIAGE: Subtle backlit chlorophyll transmission
        if (grassW > 0.04f)
        {
            float backLight = pow(saturate(dot(viewDir, -keyDirView)), 2.5f);
            float trans = backLight * grassW * (RelightIntensity * 0.35f + SssIntensity * 0.30f) * solidMask * (1.0f - silEdge * 0.75f);
            litColor += albedo * keyTint * float3(1.02f, 1.08f, 0.85f) * trans * (0.24f + Luma(giBounce));
        }
    }

    // Crisp Specular Highlight Clarity (suppressed on fabric, grass, and silhouettes)
    if (SpecularRelief > 1e-4f)
    {
        float microMag = length(microGrad) * meshGuard;
        float grazing = pow(1.0f - safeNdotV, 3.2f) * smoothness;
        float specSignal = 0.50f * grazing + 0.50f * saturate(microMag * 1.15f) * smoothness;
        float matSpecGate = (1.0f - skinW * 0.55f) * (1.0f - fabricW * 0.80f) * (1.0f - grassW * 0.85f);
        float specMod = SpecularRelief * edgeSafeMask * specOcc * specSignal * 0.22f * matSpecGate;
        litColor *= (1.0f + specMod);
    }

    if (aerialFactor > 1e-4f)
    {
        litColor = lerp(litColor, max(litColor, aerialTint), aerialFactor);
    }

    // 3D Bilateral Grid Trilinear Slicing (depth-safe tone & shadow-lift only; no pre-upscale high-pass ringing)
    float3 finalColor = litColor;
    float3 bgDiag = float3(0.5f, 0.5f, 0.5f);

    if (NeuralDetail > 1e-4f || DebugView == 4)
    {
        float lumaZ = saturate(log2(1.0f + max(Luma(litColor), 0.0f)) * 0.25f);
        float3 gridUv = float3(uv, clamp(lumaZ * (7.0f / 8.0f) + (0.5f / 8.0f), 0.0625f, 0.9375f));

        float4 bg0 = BilateralGrid0Tex.SampleLevel(LinearClampBg0, gridUv, 0.0f);
        float4 bg1 = BilateralGrid1Tex.SampleLevel(LinearClampBg1, gridUv, 0.0f);
        float4 bg2 = BilateralGrid2Tex.SampleLevel(LinearClampBg2, gridUv, 0.0f);

        float strength = saturate(NeuralDetail * 0.45f) * detailGuard;
        float3 gain = 1.0f + clamp(bg0.rgb, -0.14f, 0.16f) * strength;
        float shadowLift = clamp(bg0.a, -0.06f, 0.06f) * strength * rcp(1.0f + centerLuma * 2.5f);
        float chromaBoost = clamp(bg2.x, -0.12f, 0.14f) * strength;

        finalColor = litColor * max(gain + shadowLift, 0.70f);
        float finalLuma = Luma(finalColor);
        finalColor = finalLuma + (finalColor - finalLuma) * (1.0f + chromaBoost);

        bgDiag = saturate(float3(bg0.r * 2.0f + 0.5f, bg1.a * 2.0f + 0.5f, bg2.x * 2.0f + 0.5f));
    }

    // Hue-Preserving Subtle Filmic Dynamic Range Protection (no double-tonemapping oversaturation!)
    if (ToneContrast > 1e-4f)
        finalColor = ApplySubtleFilmicCurve(finalColor, clamp(ToneContrast, 0.0f, 1.0f));

    if (DebugView == 1)
        finalColor = giBounce * 2.5f;
    else if (DebugView == 2)
        finalColor = contactAo.xxx;
    else if (DebugView == 3)
        finalColor = viewNormal * 0.5f + 0.5f;
    else if (DebugView == 4)
        finalColor = bgDiag;
    else if (DebugView == 5)
        finalColor = ssrSample.rgb * ssrSample.a * 1.5f;
    else if (DebugView == 6)
        finalColor = float3(effRoughness, metallic, skinProb);
    else if (DebugView == 7)
        finalColor = float3(skinProb, contactShadow, grassProb);
    else if (DebugView == 8)
    {
        if (WorldHashEnabled > 0 && HasCameraBasis > 0)
        {
            float3 worldPos = ViewToWorldPos(viewPos, CameraPos, CameraRight, CameraUp, CameraFwd);
            float3 worldNorm = ViewToWorldDir(viewNormal, CameraRight, CameraUp, CameraFwd);
            float3 hashRad;
            finalColor = LookupWorldHash(WorldHashBuf, worldPos, worldNorm, centerDepth, FrameIndex, hashRad)
                             ? hashRad
                             : float3(0.05f, 0.0f, 0.05f);
        }
        else
        {
            finalColor = float3(0.15f, 0.0f, 0.0f);
        }
    }
    else if (DebugView == 9)
    {
        // Skin=Peach, Fabric=Magenta, Sand=Gold, Soil=Sienna, Grass=Green, Hard=SteelBlue
        finalColor = skinProb   * float3(1.00f, 0.65f, 0.50f) +
                     fabricProb * float3(0.95f, 0.15f, 0.85f) +
                     sandProb   * float3(0.98f, 0.82f, 0.22f) +
                     soilProb   * float3(0.55f, 0.28f, 0.10f) +
                     grassProb  * float3(0.18f, 0.88f, 0.22f) +
                     hardProb   * float3(0.22f, 0.52f, 0.92f);
    }

    if (DebugView == 0)
        finalColor *= preExp;

    finalColor = max(finalColor, 0.0f);
    DestColor[destCoord] = float4(EncodeColor(finalColor, OutColorEncodeMode), origAlpha);
}
