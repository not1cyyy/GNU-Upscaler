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
groupshared float s_halfCs[11][11];

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
    int activeW = (DisplaySizeMV > 0)
                      ? ((PreUpscale == 0) ? OutputWidth : max(MotionWidth - MvSubrectBaseX, 1))
                      : RenderWidth;
    int activeH = (DisplaySizeMV > 0)
                      ? ((PreUpscale == 0) ? OutputHeight : max(MotionHeight - MvSubrectBaseY, 1))
                      : RenderHeight;
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

    // Deeper shadow toe + gentle midtone S-curve contrast + soft HDR highlight shoulder
    float toe = l * l / max(l + 0.085f * contrastStr, 1e-4f);
    float toeW = (1.0f - smoothstep(0.015f, 0.30f, l)) * contrastStr * 0.72f;
    float l1 = lerp(l, toe, toeW);

    float pivot = 0.28f;
    float sPow = 1.0f + contrastStr * 0.24f;
    float lS = pivot * pow(max(l1 / pivot, 1e-5f), sPow);
    float sBlend = smoothstep(0.0f, 0.06f, l1) * (1.0f - smoothstep(0.85f, 2.0f, l1));
    float l2 = lerp(l1, lS, sBlend * 0.65f);

    float shoulder = l2 / (1.0f + max(l2 - 1.10f, 0.0f) * contrastStr * 0.20f);
    float targetL = lerp(l2, shoulder, smoothstep(0.80f, 2.40f, l2));

    return max(linearCol * (targetL / l), 0.0f);
}

// Full-res silhouette-safe normal reconstruction directly from LDS depth (zero 2x2 staircasing)
float3 ReconstructFullResViewNormal(int2 outCoord, float2 uv, float2 invOut,
                                    float dC, float dL, float dR, float dU, float dD,
                                    float dUL, float dUR, float dDL, float dDR)
{
    float invDC = rcp(max(dC, 1e-4f));
    float3 qC  = ReconstructViewPos(uv,                                1.0f,        TanHalfFov, TanHalfFovAspect);
    float3 qL  = ReconstructViewPos(uv + float2(-invOut.x, 0.0f),      dL  * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qR  = ReconstructViewPos(uv + float2( invOut.x, 0.0f),      dR  * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qU  = ReconstructViewPos(uv + float2(0.0f, -invOut.y),      dU  * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qD  = ReconstructViewPos(uv + float2(0.0f,  invOut.y),      dD  * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qUL = ReconstructViewPos(uv + float2(-invOut.x, -invOut.y), dUL * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qUR = ReconstructViewPos(uv + float2( invOut.x, -invOut.y), dUR * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qDL = ReconstructViewPos(uv + float2(-invOut.x,  invOut.y), dDL * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qDR = ReconstructViewPos(uv + float2( invOut.x,  invOut.y), dDR * invDC, TanHalfFov, TanHalfFovAspect);

    float iC = invDC;
    float iL = rcp(max(dL, 1e-4f));
    float iR = rcp(max(dR, 1e-4f));
    float iU = rcp(max(dU, 1e-4f));
    float iD = rcp(max(dD, 1e-4f));

    bool hasL = outCoord.x > 0;
    bool hasR = outCoord.x < (OutputWidth - 1);
    bool hasU = outCoord.y > 0;
    bool hasD = outCoord.y < (OutputHeight - 1);

    float diL = iC - iL;
    float diR = iR - iC;
    float tolX = max(abs(diL) + abs(diR), iC * 0.008f) * 0.45f + iC * 0.004f;
    bool coplanarX = (hasL && hasR && dL < 64000.0f && dR < 64000.0f && abs(diR - diL) < tolX);

    float3 dx;
    if (coplanarX)
        dx = 0.5f * (qR - qL);
    else if (hasL && hasR)
        dx = (abs(diR) < abs(diL)) ? (qR - qC) : (qC - qL);
    else if (hasR)
        dx = qR - qC;
    else if (hasL)
        dx = qC - qL;
    else
        dx = float3(2.0f * TanHalfFovAspect * invOut.x, 0.0f, 0.0f);

    float diU = iC - iU;
    float diD = iD - iC;
    float tolY = max(abs(diU) + abs(diD), iC * 0.008f) * 0.45f + iC * 0.004f;
    bool coplanarY = (hasU && hasD && dU < 64000.0f && dD < 64000.0f && abs(diD - diU) < tolY);

    float3 dy;
    if (coplanarY)
        dy = 0.5f * (qU - qD);
    else if (hasU && hasD)
        dy = (abs(diD) < abs(diU)) ? (qC - qD) : (qU - qC);
    else if (hasU)
        dy = qU - qC;
    else if (hasD)
        dy = qC - qD;
    else
        dy = float3(0.0f, 2.0f * TanHalfFov * invOut.y, 0.0f);

    float3 n = cross(dx, dy);

    if (coplanarX && coplanarY && dUL < 64000.0f && dUR < 64000.0f && dDL < 64000.0f && dDR < 64000.0f)
    {
        float gX = 0.5f * (iR - iL);
        float gY = 0.5f * (iD - iU);
        float diagTol = max(tolX, tolY) * 1.5f;
        float eUL = abs(rcp(max(dUL, 1e-4f)) - (iC - gX - gY));
        float eUR = abs(rcp(max(dUR, 1e-4f)) - (iC + gX - gY));
        float eDL = abs(rcp(max(dDL, 1e-4f)) - (iC - gX + gY));
        float eDR = abs(rcp(max(dDR, 1e-4f)) - (iC + gX + gY));
        if (max(max(eUL, eUR), max(eDL, eDR)) < diagTol)
        {
            float3 nDiag = cross(0.5f * (qUR - qDL), 0.5f * (qUL - qDR));
            if (dot(nDiag, n) < 0.0f)
                nDiag = -nDiag;
            n += nDiag * 0.5f;
        }
    }

    float lenSq = dot(n, n);
    if (lenSq < 1e-12f)
        return float3(0.0f, 0.0f, -1.0f);

    n *= rsqrt(lenSq);
    if (dot(n, qC) > 0.0f)
        n = -n;
    if (n.z > -0.005f)
    {
        n.z = -0.005f;
        n = normalize(n);
    }
    return n;
}

// Strict 2x2 Bilinear Joint-Bilateral Upsample (full-res normal & depth-checked on all 4 taps)
void BilateralUpsampleHalfRes(int2 ldsHalfBase, float2 fracHalf, float centerDepth, float3 fullResNorm,
                              bool sampleSsr,
                              out float4 outGi, out float outCs, out float4 outSsr,
                              out float outFlicker, out float3 outHalfNorm, out float3 outBentNormal,
                              out float3 outGiDir, out float outValid)
{
    float invSigmaLog2 = 1.442695f / max(centerDepth * 0.018f, 0.012f);
    float maxDepthCliff = max(centerDepth * 0.055f, 0.030f);

    static const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
    float wBilin[4] = {
        (1.0f - fracHalf.x) * (1.0f - fracHalf.y),
        fracHalf.x * (1.0f - fracHalf.y),
        (1.0f - fracHalf.x) * fracHalf.y,
        fracHalf.x * fracHalf.y
    };

    float4 giAccum = float4(0, 0, 0, 0);
    float csAccum = 0.0f;
    float4 ssrAccum = float4(0, 0, 0, 0);
    float3 normAccum = float3(0, 0, 0);
    float3 bentAccum = float3(0, 0, 0);
    float3 dirAccum = float3(0, 0, 0);
    float flickAccum = 0.0f;
    float wSum = 0.0f;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        int2 hp = ldsHalfBase + offsets[i];
        float4 hNormTap = UnpackFloat4Half(s_halfNorm[hp.y][hp.x]);
        float tapDepth = hNormTap.w;
        float dDiff = abs(centerDepth - tapDepth);
        if (tapDepth >= 64000.0f || dDiff > maxDepthCliff)
            continue;

        float2 tn = clamp(hNormTap.xy, -1.0f, 1.0f);
        float3 tNorm = float3(tn, -sqrt(max(0.0f, 1.0f - dot(tn, tn))));

        float wZ = exp2(-dDiff * invSigmaLog2);
        float wN = pow(max(0.0f, dot(fullResNorm, tNorm)), 6.0f);
        float w = (wBilin[i] + 1e-3f) * wZ * (0.05f + 0.95f * wN);

        float4 giTap = UnpackFloat4Half(s_halfGi[hp.y][hp.x]);
        float4 dirTap = UnpackFloat4Half(s_halfDir[hp.y][hp.x]);
        float2 bn = clamp(dirTap.xy, -1.0f, 1.0f);
        float3 bNorm = float3(bn, -sqrt(max(0.0f, 1.0f - dot(bn, bn))));
        float3 gDir = OctDecode(dirTap.zw);

        giAccum += giTap * w;
        csAccum += s_halfCs[hp.y][hp.x] * w;
        normAccum += tNorm * w;
        bentAccum += bNorm * w;
        dirAccum += gDir * w;
        flickAccum += saturate(hNormTap.z) * w;
        if (sampleSsr)
            ssrAccum += UnpackFloat4Half(s_halfSsr[hp.y][hp.x]) * w;
        wSum += w;
    }

    outValid = saturate(wSum * 8.0f);
    if (wSum > 1e-4f)
    {
        float invW = rcp(wSum);
        float4 giMean = giAccum * invW;
        float csMean = csAccum * invW;
        float4 ssrMean = sampleSsr ? (ssrAccum * invW) : float4(0, 0, 0, 0);
        float3 hNormMean = (dot(normAccum, normAccum) > 1e-6f) ? normalize(normAccum) : fullResNorm;
        float3 bentMean = (dot(bentAccum, bentAccum) > 1e-6f) ? normalize(bentAccum) : fullResNorm;
        float3 dirMean = (dot(dirAccum, dirAccum) > 1e-6f) ? normalize(dirAccum) : fullResNorm;

        outGi = float4(giMean.rgb * outValid, lerp(1.0f, saturate(giMean.a), outValid));
        outCs = lerp(1.0f, saturate(csMean), outValid);
        outSsr = float4(ssrMean.rgb, ssrMean.a * outValid);
        outFlicker = saturate(flickAccum * invW * outValid);
        outHalfNorm = hNormMean;
        outBentNormal = normalize(lerp(fullResNorm, bentMean, outValid));
        outGiDir = normalize(lerp(fullResNorm, dirMean, outValid));
    }
    else
    {
        outGi = float4(0.0f, 0.0f, 0.0f, 1.0f);
        outCs = 1.0f;
        outSsr = float4(0.0f, 0.0f, 0.0f, 0.0f);
        outFlicker = 0.0f;
        outHalfNorm = fullResNorm;
        outBentNormal = fullResNorm;
        outGiDir = fullResNorm;
    }
}

// Flat-top bilateral normal smoother: melts 5-20 deg low-poly mesh facet lines on curved skin/bodies
// into silky Phong-like normals while keeping >45 deg sharp rock/crate corners & silhouettes razor-sharp.
float3 SmoothCurvedNormal(int2 halfTileBase, float2 fHalf, float centerDepth, float3 fullResNorm, float silEdge)
{
    float maxDepthCliff = max(centerDepth * 0.055f, 0.030f);
    float invSigmaZ = 1.442695f / max(centerDepth * 0.022f, 0.015f);

    int2 centerHalf = ClampGiCoord(int2(round(fHalf)));
    int2 ldsCenter = clamp(centerHalf - halfTileBase, int2(1, 1), int2(9, 9));

    // Center full-res sample anchors the filter
    float3 acc = fullResNorm * 0.35f;
    float wSum = 0.35f;

    // Stage A: 3x3 LDS half-res neighborhood (zero extra texture fetches)
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 lp = ldsCenter + int2(dx, dy);
            float4 hTap = UnpackFloat4Half(s_halfNorm[lp.y][lp.x]);
            float tapDepth = hTap.w;
            float dDiff = abs(centerDepth - tapDepth);
            if (tapDepth >= 64000.0f || dDiff > maxDepthCliff)
                continue;

            float2 tn = clamp(hTap.xy, -1.0f, 1.0f);
            float3 tNorm = float3(tn, -sqrt(max(0.0f, 1.0f - dot(tn, tn))));
            float wN = smoothstep(0.68f, 0.92f, dot(fullResNorm, tNorm));
            if (wN <= 1e-4f)
                continue;

            float2 tapPos = float2(halfTileBase + lp);
            float2 dPos = tapPos - fHalf;
            float wS = exp2(-dot(dPos, dPos) * 0.55f);
            float wZ = exp2(-dDiff * invSigmaZ);
            float w = wS * wZ * wN;
            acc += tNorm * w;
            wSum += w;
        }
    }

    // Stage B: Wide concentric ring (+-2, +-4, +-6 half-res = +-4..12 full-res px) for close-up curved interiors
    float closeUpCurve = (1.0f - smoothstep(1.8f, 3.4f, centerDepth)) * (1.0f - smoothstep(0.01f, 0.05f, silEdge));
    if (closeUpCurve > 0.02f)
    {
        static const int2 wideOffsets[12] = {
            int2(-2,  0), int2( 2,  0), int2( 0, -2), int2( 0,  2),
            int2(-4,  0), int2( 4,  0), int2( 0, -4), int2( 0,  4),
            int2(-3, -3), int2( 3, -3), int2(-3,  3), int2( 3,  3)
        };
        static const float wideSpatial[12] = {
            0.82f, 0.82f, 0.82f, 0.82f,
            0.55f, 0.55f, 0.55f, 0.55f,
            0.52f, 0.52f, 0.52f, 0.52f
        };

        [unroll]
        for (int k = 0; k < 12; ++k)
        {
            int2 hc = ClampGiCoord(centerHalf + wideOffsets[k]);
            float4 hTap = HalfNormalTex.Load(int3(hc, 0));
            float tapDepth = hTap.w;
            float dDiff = abs(centerDepth - tapDepth);
            if (tapDepth >= 64000.0f || dDiff > maxDepthCliff)
                continue;

            float2 tn = clamp(hTap.xy, -1.0f, 1.0f);
            float3 tNorm = float3(tn, -sqrt(max(0.0f, 1.0f - dot(tn, tn))));
            float wN = smoothstep(0.68f, 0.92f, dot(fullResNorm, tNorm));
            if (wN <= 1e-4f)
                continue;

            float wZ = exp2(-dDiff * invSigmaZ);
            float w = wideSpatial[k] * wZ * wN * closeUpCurve;
            acc += tNorm * w;
            wSum += w;
        }
    }

    float lenSq = dot(acc, acc);
    if (lenSq < 1e-8f)
        return fullResNorm;
    float3 smoothed = acc * rsqrt(lenSq);
    if (smoothed.z > -0.005f)
    {
        smoothed.z = -0.005f;
        smoothed = normalize(smoothed);
    }
    return smoothed;
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

            int2 dCoord = (PreUpscale > 0)
                              ? ClampDepthCoord(int2(DepthSubrectBaseX, DepthSubrectBaseY) + p)
                              : UvToDepthCoord((float2(p) + 0.5f) * invOut);
            float rawD = DepthTex.Load(int3(dCoord, 0)).r;
            s_depth[sy][sx] = LinearizeDepthValue(rawD, DepthIsLinear, DepthIsReversed,
                                                  DepthLinearA, DepthLinearB, DepthLinearC, SkyDepthThreshold);
        }
    }

    // Populate 11x11 half-res GI/Normal/SSR/Dir/CS LDS cache
    bool exact2x = (PreUpscale > 0 && abs(OutputWidth - GiWidth * 2) <= 1 && abs(OutputHeight - GiHeight * 2) <= 1);
    int2 halfTileBase = exact2x
                            ? (int2(gid.xy) * 8 - 1)
                            : (int2(floor((float2(gid.xy) * 16.0f + 0.5f) * outToGi - 0.5f)) - 1);
    if (linearIdx < 121u)
    {
        uint hx = linearIdx % 11u;
        uint hy = linearIdx / 11u;
        int2 hc = ClampGiCoord(halfTileBase + int2(hx, hy));
        s_halfGi[hy][hx] = PackFloat4Half(DenoisedGiTex.Load(int3(hc, 0)));
        s_halfNorm[hy][hx] = PackFloat4Half(HalfNormalTex.Load(int3(hc, 0)));
        s_halfSsr[hy][hx] = PackFloat4Half(DenoisedSsrTex.Load(int3(hc, 0)));
        s_halfDir[hy][hx] = PackFloat4Half(DenoisedGiDirTex.Load(int3(hc, 0)));
        s_halfCs[hy][hx] = saturate(DenoisedMetaTex.Load(int3(hc, 0)).r);
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

    float3 fullResNormal = ReconstructFullResViewNormal(outCoord, uv, invOut,
                                                        centerDepth, dL, dR, dU, dD, dUL, dUR, dDL, dDR);
    float3 viewNormal = fullResNormal;

    float2 fHalf = exact2x ? (float2(outCoord) * 0.5f) : ((float2(outCoord) + 0.5f) * outToGi - 0.5f);
    int2 baseHalf = int2(floor(fHalf));
    float2 fracHalf = frac(fHalf);
    int2 ldsHalfBase = clamp(baseHalf - halfTileBase, int2(0, 0), int2(9, 9));
    float centerFlicker = 0.0f;

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

    float effGi = clamp(GiIntensity, 0.0f, 2.0f);
    float effAo = clamp(AoIntensity, 0.0f, 2.0f);
    float effSsr = clamp(SsrIntensity, 0.0f, 2.0f);
    float effRelight = clamp(RelightIntensity, 0.0f, 2.0f);
    float effCs = clamp(ContactShadowIntensity, 0.0f, 2.0f);

    float upsampleValid = 1.0f;
    if (GiEnabled > 0)
    {
        float4 giSample;
        float rawCs;
        float3 halfNormUp;
        bool needSsr = (SsrEnabled > 0 && effSsr > 1e-4f) || (DebugView == 5);
        BilateralUpsampleHalfRes(ldsHalfBase, fracHalf, centerDepth, fullResNormal, needSsr,
                                 giSample, rawCs, ssrSample, flickerSoft, halfNormUp, bentNormal, giDir, upsampleValid);
        if (HasGBufferNormals > 0 && upsampleValid > 0.1f)
            viewNormal = normalize(lerp(fullResNormal, halfNormUp, upsampleValid));

        giBounce = max(giSample.rgb, 0.0f);
        float giL = Luma(giBounce);
        if (giL > 2.8f)
            giBounce *= 2.8f / giL;
        float aoPower = 0.45f + effAo * 1.10f;
        float shapedAo = pow(max(saturate(giSample.a), 1e-4f), aoPower);
        contactAo = lerp(1.0f, shapedAo, saturate(effAo * 2.5f) * skyMask * (1.0f - flickerSoft));

        float csPower = 0.55f + effCs * 0.85f;
        float shapedCs = pow(max(rawCs, 1e-4f), csPower);
        contactShadow = lerp(1.0f, shapedCs, saturate(effCs * 2.5f) * skyMask * (1.0f - flickerSoft));
    }
    else
    {
        float4 dummyGi, dummySsr;
        float dummyCs;
        float3 halfNormUp, dummyBent, dummyDir;
        BilateralUpsampleHalfRes(ldsHalfBase, fracHalf, centerDepth, fullResNormal, false,
                                 dummyGi, dummyCs, dummySsr, flickerSoft, halfNormUp, dummyBent, dummyDir, upsampleValid);
        if (HasGBufferNormals > 0 && upsampleValid > 0.1f)
            viewNormal = normalize(lerp(fullResNormal, halfNormUp, upsampleValid));
        bentNormal = viewNormal;
        giDir = viewNormal;
    }

    if (HasGBufferNormals == 0)
        viewNormal = SmoothCurvedNormal(halfTileBase, fHalf, centerDepth, fullResNormal, silEdge);

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

    float skinChromaGate = 1.0f;
    if (HasGBufferSubsurface == 0)
    {
        float invL = rcp(max(centerLuma + 0.05f, 0.05f));
        float rg = (centerColor.r - centerColor.g) * invL;
        float rb = (centerColor.r - centerColor.b) * invL;
        // Human skin spans fair/peach/olive/deep-brown with G cleanly between B and R (rejects crimson cloth & neutral grey)
        float skinHueRatio = (centerColor.g - centerColor.b) / max(centerColor.r - centerColor.b, 1e-4f);
        float hueBand = smoothstep(0.10f, 0.24f, skinHueRatio) * (1.0f - smoothstep(0.72f, 0.88f, skinHueRatio));
        float warmSkin = smoothstep(0.015f, 0.11f, rb) * smoothstep(-0.02f, 0.06f, rg) *
                         (1.0f - smoothstep(0.95f, 1.45f, rb)) * hueBand;
        float nearCharDist = 1.0f - smoothstep(12.0f, 28.0f, centerDepth);
        skinChromaGate = warmSkin * nearCharDist;
    }

    float maxSem = max(max(max(skinProb, fabricProb), max(sandProb, soilProb)), max(grassProb, hardProb));
    float confGate = smoothstep(0.28f, 0.65f, maxSem);
    float skinW = smoothstep(0.28f, 0.68f, skinProb) * confGate * skinChromaGate;
    float fabricW = smoothstep(0.32f, 0.72f, fabricProb) * confGate;
    float sandW = smoothstep(0.32f, 0.72f, sandProb) * confGate;
    float soilW = smoothstep(0.32f, 0.72f, soilProb) * confGate;
    float grassW = smoothstep(0.32f, 0.72f, grassProb) * confGate;
    float wetSoilW = soilW * wetness;

    float effRoughness = (HasGBufferRoughness > 0)
                             ? lerp(roughness, 0.14f, wetSoilW)
                             : lerp(roughness, max(roughness * 0.85f, 0.36f), wetSoilW * 0.35f);
    float smoothness = saturate(1.0f - effRoughness);

    float3 shadedNormal = viewNormal;
    if (HasGBufferNormals == 0)
    {
        float microStrength = (soilW * 0.22f + sandW * 0.16f + hardProb * 0.10f + fabricW * 0.08f + skinW * 0.030f) *
                              (1.0f - grassW) * edgeSafeMask * meshGuard;
        if (microStrength > 1e-4f)
            shadedNormal = normalize(viewNormal + float3(-microGrad.x, microGrad.y, 0.0f) * microStrength);
    }

    // 3-Layer Jimenez Skin SSS (Epidermis / Dermis / Subcutis) with crisp real-texture preservation
    float3 litColor = centerColor;
    float effSkinSss = saturate(skinProb * SssIntensity * 0.90f) * skinChromaGate;
    if (effSkinSss > 0.03f && SssIntensity > 1e-4f && solidMask > 0.05f)
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
        float3 epiDetail = centerColor - ring1;

        float sssPyrSigma = rcp(max(centerDepth * 0.032f, 0.020f));
        float4 pyr0 = ColorPyramidTex.SampleLevel(LinearClampPyr, uv, 0.0f);
        float4 pyr1 = ColorPyramidTex.SampleLevel(LinearClampPyr, uv, 1.0f);
        float silGuard = (1.0f - silEdge) * (1.0f - fgSilEdge);
        float w2 = exp(-abs(centerDepth - pyr0.a) * sssPyrSigma) * silGuard;
        float w3 = exp(-abs(centerDepth - pyr1.a) * (sssPyrSigma * 0.85f)) * silGuard;
        float3 ring2 = lerp(ring1, max(pyr0.rgb, 0.0f), w2);
        float3 ring3 = lerp(ring2, max(pyr1.rgb, 0.0f), w3 * 0.65f);

        // Real human skin optical mean free paths: R ~3.5mm (deep dermis), G ~1.0mm, B ~0.3mm (epidermis)
        float3 diffusedSkin = float3(
            lerp(ring2.r, ring3.r, 0.55f),
            lerp(ring1.g, ring2.g, 0.45f),
            lerp(centerColor.b, ring1.b, 0.40f)
        ) * float3(1.005f, 0.996f, 0.992f) + epiDetail * float3(0.45f, 0.80f, 0.92f);

        // Pre-integrated hemoglobin terminator warmth + subtle backlit thin-tissue translucency
        float skinNdotK = dot(shadedNormal, keyDirView);
        float termBand = exp(-pow((skinNdotK - 0.08f) / 0.34f, 2.0f)) * saturate(keyStrength * 1.15f + 0.15f);
        float3 dermisTint = float3(1.00f, 0.28f, 0.11f);
        float3 termScatter = dermisTint * max(ring3, centerColor) * termBand * effSkinSss *
                             (0.18f + 0.12f * (1.0f - directMask)) * solidMask * (1.0f - silEdge * 0.85f);

        float backView = pow(saturate(dot(viewDir, -keyDirView)), 2.2f);
        float backRim = pow(1.0f - safeNdotV, 2.2f) * backView * directMask * contactShadow;
        float3 backScatter = float3(1.00f, 0.24f, 0.09f) * max(ring2, centerColor) * backRim * effSkinSss *
                             0.26f * edgeSafeMask;

        float3 blendedSkin = lerp(litColor, max(diffusedSkin, 0.0f),
                                  float3(0.72f, 0.36f, 0.14f) * effSkinSss * solidMask * (1.0f - silEdge * 0.85f)) +
                             termScatter + backScatter;

        float diffLuma = Luma(blendedSkin);
        if (diffLuma > centerLuma * 1.15f + 0.01f)
            blendedSkin *= (centerLuma * 1.15f + 0.01f) / max(diffLuma, 1e-4f);

        litColor = max(blendedSkin, 0.0f);
    }

    // Subtle wet soil darkening (only strong when real GBuffer roughness confirms wet surface)
    float wetDarken = (HasGBufferRoughness > 0) ? 0.74f : 0.94f;
    litColor *= lerp(1.0f, wetDarken, wetSoilW * solidMask);

    // Multi-Bounce AO + Contact Shadow + Sunlit-Equilibrium GI (zero 1/8-res color blotches!)
    float specOcc = 1.0f;
    if (GiEnabled > 0)
    {
        float indirectWeight = saturate(1.0f - directMask * 0.48f) * (1.0f - skinW * 0.22f);

        // Texture-preserving scalar ambient de-lighting (no coarse 1/8-res albedo subtraction)
        float delightFrac = saturate(Luma(sh0) * 0.26f) * indirectWeight * (1.0f - silEdge * 0.50f) *
                            min(effGi * 0.26f + effAo * 0.18f, 0.34f);
        litColor *= (1.0f - delightFrac * solidMask * upsampleValid);

        float aoFloor = lerp(0.14f, 0.24f, sandW);
        float effectiveAo = lerp(1.0f, max(contactAo, aoFloor), indirectWeight);
        float3 aoAlbedo = lerp(albedo, Luma(albedo).xxx, skinW * 0.35f);
        float3 coloredAo = float3(MultiBounceAO(min16float(effectiveAo), min16float3(aoAlbedo)));
        specOcc = ComputeSpecularOcclusion(bentNdotV, effectiveAo, effRoughness);
        float csFactor = lerp(1.0f, max(contactShadow, 0.24f), 0.90f * solidMask);
        litColor = litColor * lerp(float3(1, 1, 1), coloredAo, solidMask) * csFactor;

        float dirGiShade = 0.48f + 0.52f * saturate(dot(shadedNormal, giDir));
        float sunAtten = (1.0f - directMask * 0.62f) * rcp(1.0f + centerLuma * 1.10f);
        float3 bounceAlbedo = max(clamp(albedo, 0.05f, 0.88f),
                                  float3(0.12f, 0.12f, 0.12f) * (1.0f - metallic * 0.50f));

        litColor += giBounce * bounceAlbedo * dirGiShade * (1.0f - metallic * 0.50f) *
                    (effGi * (0.64f + 0.96f * sunAtten)) * solidMask;

        // Subtle Bent-Normal Sky Fill in shadows (suppress in pitch-black caves to avoid 1/8-res tint lift)
        float3 bentWorld = (HasCameraBasis > 0) ? ViewToWorldDir(bentNormal, CameraRight, CameraUp, CameraFwd) : bentNormal;
        float3 skyFill = max(sh0 + shX * bentWorld.x + shY * bentWorld.y + shZ * bentWorld.z, 0.0f);
        litColor += skyFill * albedo * effectiveAo * indirectWeight * (0.18f * effGi) *
                    saturate(centerLuma * 6.0f + 0.12f) * solidMask * (1.0f - silEdge * 0.4f);

        // Indirect specular sheen (strictly interior-only via edgeSafeMask)
        float3 indSpecF0 = lerp(float3(0.04f, 0.04f, 0.04f), bounceAlbedo, metallic);
        float indFresnel = 0.25f + 0.75f * pow(1.0f - safeNdotV, 4.0f);
        litColor += giBounce * indSpecF0 * indFresnel * pow(smoothness, 1.5f) * specOcc *
                    (1.0f - skinW * 0.80f) * (1.0f - fabricW * 0.75f) * (effGi * 0.48f) * edgeSafeMask;
    }

    // Ray-Traced Grazing & Metallic SSR (strictly restricted to glossy/metallic surfaces, zero sand/soil puddles)
    if (GiEnabled > 0 && SsrEnabled > 0 && effSsr > 1e-4f && effRoughness < 0.62f)
    {
        float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
        f0 = lerp(f0, float3(0.028f, 0.028f, 0.028f), skinW);
        float fresnel = pow(1.0f - safeNdotV, 5.0f);
        float3 specF = f0 + (1.0f - f0) * fresnel;
        float glossGate = pow(saturate(1.0f - effRoughness / 0.62f), 1.65f) * (1.0f - skinW * 0.90f) *
                          (1.0f - fabricW * 0.85f) * (1.0f - sandW * 0.92f) * (1.0f - soilW * 0.85f) *
                          (1.0f - grassW * 0.95f);
        float ssrWetBoost = (HasGBufferRoughness > 0) ? (wetSoilW * 0.22f) : 0.0f;
        float reflWeight = min(saturate((max(specF.r, max(specF.g, specF.b)) * 1.25f + metallic * 0.24f + ssrWetBoost) *
                                        glossGate * effSsr * 1.30f),
                               0.70f) *
                           edgeSafeMask * specOcc * saturate(ssrSample.a);
        float3 reflTint = lerp(float3(1, 1, 1), albedo, metallic * 0.70f);
        litColor = lerp(litColor, ssrSample.rgb * reflTint, reflWeight);
    }

    // Scene L1 SH 3D Sculptural Relighting + Material-Specific Micro-BRDFs (ZERO silhouette outline glow!)
    if (effRelight > 1e-4f)
    {
        float3 relightNorm = normalize(lerp(shadedNormal, bentNormal, 0.30f * edgeSafeMask * upsampleValid));
        float3 shNormal = (HasCameraBasis > 0) ? ViewToWorldDir(relightNorm, CameraRight, CameraUp, CameraFwd) : relightNorm;
        float3 shDirRad = max(sh0 + shX * shNormal.x + shY * shNormal.y + shZ * shNormal.z, 0.0f);
        float shMeanL = max(Luma(sh0), 0.04f);
        float3 shRatio = clamp(shDirRad / shMeanL, 0.50f, 1.58f) - 1.0f;

        float rawKeyDot = dot(shadedNormal, keyDirView);
        float skinFabricWrap = 0.42f * skinW + 0.20f * fabricW;
        float wrapKeyDot = (rawKeyDot + skinFabricWrap) / (1.0f + skinFabricWrap);
        float macroKey = (wrapKeyDot - 0.12f) * 0.18f * keyStrength;
        float hemiTerm = dot(shadedNormal, worldUpInView) * 0.15f;
        float sculptScalar = macroKey + hemiTerm;
        // Positive lifts slightly warm with keyTint; negative darkens slightly cooler (more red reduction than blue)
        float3 sculptTint = (sculptScalar >= 0.0f) ? keyTint : float3(1.06f, 1.00f, 0.90f);
        float relightMask = solidMask * (1.0f - silEdge * 0.72f) * (1.0f - fgSilEdge * 0.60f) * lerp(1.0f, 0.60f, skinW);
        float3 sculptVec = (shRatio * 0.24f + sculptScalar * sculptTint) * effRelight * relightMask;
        float brightDamp = rcp(1.0f + max(centerLuma - 0.65f, 0.0f) * 0.85f);
        sculptVec = min(sculptVec, max(sculptVec * brightDamp, 0.0f));
        litColor *= clamp(1.0f + sculptVec, 0.45f, 1.50f);

        // 0) SKIN: Silky Dual-Lobe Sebum + Epidermal Sheen (neutral F0=0.028) + Vellus Peach-Fuzz Rim
        if (skinW > 0.04f)
        {
            float3 halfVec = normalize(keyDirView + toEye);
            float nDotH = saturate(dot(shadedNormal, halfVec));
            float sebumTight = pow(nDotH, 28.0f) * 0.068f;
            float sebumBroad = pow(nDotH, 8.0f) * 0.024f;
            float skinFresnel = 0.028f + 0.972f * pow(1.0f - safeNdotV, 5.0f);
            float fresnelGain = lerp(0.82f, 1.75f, saturate(skinFresnel * 6.0f));
            float keyVis = saturate(dot(shadedNormal, keyDirView) + 0.25f) * contactShadow;

            float3 sebumSpec = lerp(float3(1.0f, 1.0f, 1.0f), keyTint, 0.40f) * (sebumTight + sebumBroad) *
                               fresnelGain * keyVis * specOcc * edgeSafeMask * skinW *
                               (0.45f + 0.55f * saturate(SssIntensity)) * (centerLuma * 0.72f + 0.04f);

            float vellus = pow(1.0f - safeNdotV, 3.6f) * 0.072f * edgeSafeMask * specOcc * skinW *
                           saturate(SssIntensity * 0.75f + 0.25f);
            float3 vellusRim = lerp(sh0, keyTint, 0.45f) * vellus * (centerLuma * 0.62f + 0.03f);
            litColor += (sebumSpec + vellusRim) * saturate(effRelight * 0.85f + 0.30f);
        }

        // 1) FABRIC: Charlie / Estevez-Kulla grazing velvet sheen + fold depth
        if (fabricW > 0.04f)
        {
            float invSin = rcp(max(sqrt(max(1.0f - safeNdotV * safeNdotV, 0.01f)), 0.15f));
            float charlie = saturate(pow(1.0f - safeNdotV, 3.2f) * (0.65f + 0.35f * invSin) * 0.42f);
            float sheen = charlie * fabricW * effRelight * edgeSafeMask * specOcc * 0.18f;
            litColor += lerp(albedo, keyTint, 0.35f) * sheen * (centerLuma * 0.80f + 0.04f);
        }

        // 2) SAND: Oren-Nayar backscatter + sunlit sand chroma restraint (no synthetic sparkle dots)
        if (sandW > 0.04f)
        {
            float nDotK = saturate(dot(shadedNormal, keyDirView));
            float orenNayarBoost = (0.12f * (1.0f - safeNdotV) * (0.4f + 0.6f * nDotK)) * sandW * effRelight * edgeSafeMask;
            litColor *= (1.0f + orenNayarBoost);

            // Prevent sunlit desert sand from oversaturating into orange
            float curL = Luma(litColor);
            float curChroma = max(litColor.r, max(litColor.g, litColor.b)) - min(litColor.r, min(litColor.g, litColor.b));
            float excessChroma = saturate((curChroma - 0.18f) * 2.5f) * saturate((sandW - 0.10f) * directMask * 0.16f);
            litColor = lerp(litColor, curL.xxx, excessChroma);
        }

        // 3) SOIL: Clumpy micro-cavity relief
        if (soilW > 0.04f)
        {
            float microCav = saturate(-(pC - pAvg) / max(pAvg + 0.10f, 0.10f));
            litColor *= (1.0f - microCav * soilW * (1.0f - wetSoilW) * 0.16f * edgeSafeMask);
        }

        // 4) GRASS / FOLIAGE: Strictly sunlit green chlorophyll transmission (zero glow in dark shadows or on dry ground)
        if (grassW > 0.04f && directMask > 0.08f)
        {
            float invLg = rcp(max(centerLuma + 0.04f, 0.04f));
            float greenChroma = smoothstep(0.01f, 0.12f, (centerColor.g - centerColor.b) * invLg) *
                                smoothstep(-0.04f, 0.06f, (centerColor.g - centerColor.r * 0.82f) * invLg);
            float backLight = pow(saturate(dot(viewDir, -keyDirView)), 2.5f);
            float trans = backLight * grassW * greenChroma * directMask * contactShadow *
                          (effRelight * 0.25f + SssIntensity * 0.25f) * solidMask * (1.0f - silEdge * 0.75f);
            litColor += centerColor * keyTint * float3(1.02f, 1.12f, 0.80f) * trans * 0.65f;
        }
    }

    // Crisp Specular Highlight Clarity (suppressed on fabric, grass, and silhouettes)
    if (SpecularRelief > 1e-4f)
    {
        float microMag = length(microGrad) * meshGuard;
        float grazing = pow(1.0f - safeNdotV, 3.2f) * smoothness;
        float specSignal = 0.50f * grazing + 0.50f * saturate(microMag * 1.15f) * smoothness;
        float matSpecGate = (1.0f - skinW * 0.55f) * (1.0f - fabricW * 0.80f) * (1.0f - grassW * 0.85f);
        float specMod = SpecularRelief * edgeSafeMask * specOcc * specSignal * 0.34f * matSpecGate;
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

        float strength = saturate(NeuralDetail * 0.68f) * detailGuard;
        float3 gain = 1.0f + clamp(bg0.rgb, -0.22f, 0.24f) * strength;
        float shadowLift = clamp(bg0.a, -0.08f, 0.08f) * strength * rcp(1.0f + centerLuma * 2.5f);
        float chromaBoost = clamp(bg2.x, -0.16f, 0.18f) * strength;

        finalColor = litColor * max(gain + shadowLift, 0.62f);
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

    finalColor *= preExp;

    finalColor = max(finalColor, 0.0f);
    DestColor[destCoord] = float4(EncodeColor(finalColor, OutColorEncodeMode), origAlpha);
}
