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
    int MotionWidth;
    int MotionHeight;

    int MvSubrectBaseX;
    int MvSubrectBaseY;
    int DisplaySizeMV;
    int LowResMV;

    int ResetHistory;
    int FrameIndex;
    int RayCount;
    int StepCount;

    int SsrEnabled;
    int SsrSteps;
    int HasCameraBasis;
    int WorldHashEnabled;

    float SkyDepthThreshold;
    float MvScaleX;
    float MvScaleY;
    float TanHalfFov;

    float TanHalfFovAspect;
    float GiRadius;
    float GiBias;
    float ContactShadowSteps;

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
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]]
#endif
Texture2D<float4> ColorPyramidTex : register(t0);

#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float4> HalfColorDepthTex : register(t1);

#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
Texture2D<float4> HalfNormalTex : register(t2);

#ifdef VK_MODE
[[vk::binding(4, 0)]]
#endif
Texture2D<float4> PrevHalfNormalTex : register(t3);

#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
Texture2D<float2> MotionTex : register(t4);

#ifdef VK_MODE
[[vk::binding(6, 0)]]
#endif
Texture2D<float4> PrevGiTex : register(t5);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
#endif
Texture2D<float4> PrevSsrTex : register(t6);

#ifdef VK_MODE
[[vk::binding(8, 0)]]
#endif
Texture2D<float4> PrevGiMetaTex : register(t7);

#ifdef VK_MODE
[[vk::binding(9, 0)]]
#endif
Texture2D<float4> PrevGiDirTex : register(t8);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]]
#endif
Texture2D<float4> Material1Tex : register(t9);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]]
#endif
Texture2D<float4> Material2Tex : register(t10);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]]
SamplerState LinearClampPyr : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
SamplerState LinearClampSsr : register(s1);
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]]
SamplerState LinearClampMat1 : register(s2);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]]
SamplerState LinearClampMat2 : register(s3);
#else
SamplerState LinearClamp : register(s0);
#define LinearClampPyr LinearClamp
#define LinearClampSsr LinearClamp
#define LinearClampMat1 LinearClamp
#define LinearClampMat2 LinearClamp
#endif

#ifdef VK_MODE
[[vk::binding(12, 0)]]
#endif
StructuredBuffer<float4> ResolvedSceneLightBuf : register(t11);

#ifdef VK_MODE
[[vk::binding(13, 0)]]
#endif
StructuredBuffer<uint> WorldHashBuf : register(t12);

#ifdef VK_MODE
[[vk::binding(14, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutGiTex : register(u0);

#ifdef VK_MODE
[[vk::binding(15, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutSsrTex : register(u1);

#ifdef VK_MODE
[[vk::binding(16, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutGiMetaTex : register(u2);

#ifdef VK_MODE
[[vk::binding(17, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutGiDirTex : register(u3);

int2 ClampGiCoord(int2 p)
{
    return int2(clamp(p.x, 0, GiWidth - 1), clamp(p.y, 0, GiHeight - 1));
}

int2 ClampMotionCoord(int2 p)
{
    int activeW = (DisplaySizeMV > 0) ? OutputWidth : RenderWidth;
    int activeH = (DisplaySizeMV > 0) ? OutputHeight : RenderHeight;
    int2 minC = int2(MvSubrectBaseX, MvSubrectBaseY);
    int2 maxC = int2(min(MvSubrectBaseX + activeW - 1, MotionWidth - 1),
                     min(MvSubrectBaseY + activeH - 1, MotionHeight - 1));
    return clamp(p, minC, max(minC, maxC));
}

float InterleavedGradientNoise(float2 p)
{
    float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(p, magic.xy)));
}

float3 EvalSceneSH(float3 dir)
{
    float3 sh0 = ResolvedSceneLightBuf[0].rgb;
    float3 shX = ResolvedSceneLightBuf[1].rgb;
    float3 shY = ResolvedSceneLightBuf[2].rgb;
    float3 shZ = ResolvedSceneLightBuf[3].rgb;
    return max(sh0 + shX * dir.x + shY * dir.y + shZ * dir.z, 0.0f) * 0.22f;
}

float3 QueryFallbackRadiance(float3 viewPos, float3 rayDirView, float rayDist, float2 jitter2)
{
    float3 rayDir = (HasCameraBasis > 0) ? ViewToWorldDir(rayDirView, CameraRight, CameraUp, CameraFwd) : rayDirView;
    if (WorldHashEnabled > 0 && HasCameraBasis > 0)
    {
        float3 worldOrigin = ViewToWorldPos(viewPos, CameraPos, CameraRight, CameraUp, CameraFwd);
        float3 hitWorld = worldOrigin + rayDir * rayDist;
        float3 rad;
        if (LookupWorldHash(WorldHashBuf, hitWorld, -rayDir, rayDist, FrameIndex, rad))
            return rad;
    }
    return EvalSceneSH(rayDir);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if ((int) dtid.x >= GiWidth || (int) dtid.y >= GiHeight)
        return;

    int2 giCoord = int2(dtid.xy);
    float2 uv = (float2(giCoord) + 0.5f) / float2(GiWidth, GiHeight);

    float4 centerCd = HalfColorDepthTex.Load(int3(giCoord, 0));
    float centerDepth = centerCd.a;

    if (centerDepth >= 64000.0f || centerDepth >= SkyDepthThreshold)
    {
        OutGiTex[giCoord] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        OutSsrTex[giCoord] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        OutGiMetaTex[giCoord] = float4(1.0f, 0.0f, 0.0f, 1.0f);
        OutGiDirTex[giCoord] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float4 hNorm = HalfNormalTex.Load(int3(giCoord, 0));
    float2 nxy = clamp(hNorm.xy, -1.0f, 1.0f);
    float3 viewNormal = float3(nxy, -sqrt(max(0.0f, 1.0f - dot(nxy, nxy))));
    float flickerMask = saturate(hNorm.z);

    if (flickerMask > 0.85f)
    {
        OutGiTex[giCoord] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        OutSsrTex[giCoord] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        OutGiMetaTex[giCoord] = float4(1.0f, 0.0f, 0.0f, 1.0f);
        OutGiDirTex[giCoord] = float4(viewNormal.xy, OctEncode(viewNormal));
        return;
    }

    float4 mat1 = Material1Tex.SampleLevel(LinearClampMat1, uv, 0.0f);
    float4 mat2 = Material2Tex.SampleLevel(LinearClampMat2, uv, 0.0f);
    float roughness = saturate(mat1.r);
    float wetness = saturate(mat1.b);
    float fabricProb = saturate(mat2.x);
    float soilProb = saturate(mat2.z);
    float grassProb = saturate(mat2.w);

    // Motion vector lookup aligned 1:1 with active image space (pre-upscale = RenderWidth/Height)
    int activeW = (DisplaySizeMV > 0) ? OutputWidth : RenderWidth;
    int activeH = (DisplaySizeMV > 0) ? OutputHeight : RenderHeight;
    int2 mvCoord = ClampMotionCoord(int2(MvSubrectBaseX, MvSubrectBaseY) + int2(uv * float2(activeW, activeH)));
    float2 mv = MotionTex.Load(int3(mvCoord, 0)).rg * float2(MvScaleX, MvScaleY);
    float2 mvUv = mv / float2(max(activeW, 1), max(activeH, 1));
    float mvMag = length(mvUv * float2(OutputWidth, OutputHeight));
    float2 prevUv = uv + mvUv;

    bool inBounds = (ResetHistory == 0 && FrameIndex > 3 && all(prevUv >= 0.002f) && all(prevUv <= 0.998f));

    // 2x2 Depth + Normal Bilateral Temporal Reprojection
    bool validHistory = false;
    float4 prevGi = float4(0.0f, 0.0f, 0.0f, 1.0f);
    float4 prevMeta = float4(1.0f, 0.25f, 0.0f, 1.0f);
    float4 prevDir = float4(viewNormal.xy, OctEncode(viewNormal));
    if (inBounds)
    {
        float2 fCoord = prevUv * float2(GiWidth, GiHeight) - 0.5f;
        int2 baseC = int2(floor(fCoord));
        float2 f = frac(fCoord);
        float wBilin[4] = {
            (1.0f - f.x) * (1.0f - f.y),
            f.x * (1.0f - f.y),
            (1.0f - f.x) * f.y,
            f.x * f.y
        };
        int2 off[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

        float4 giAcc = float4(0, 0, 0, 0);
        float4 metaAcc = float4(0, 0, 0, 0);
        float4 dirAcc = float4(0, 0, 0, 0);
        float wSum = 0.0f;

        [unroll]
        for (int ti = 0; ti < 4; ++ti)
        {
            int2 pc = ClampGiCoord(baseC + off[ti]);
            float4 pNormData = PrevHalfNormalTex.Load(int3(pc, 0));
            float relDepthErr = abs(centerDepth - pNormData.w) / max(min(centerDepth, pNormData.w), 0.05f);
            float2 pnxy = clamp(pNormData.xy, -1.0f, 1.0f);
            float3 pN = float3(pnxy, -sqrt(max(0.0f, 1.0f - dot(pnxy, pnxy))));
            if (relDepthErr < 0.040f && dot(viewNormal, pN) > 0.78f)
            {
                float w = wBilin[ti] + 1e-4f;
                giAcc += PrevGiTex.Load(int3(pc, 0)) * w;
                metaAcc += PrevGiMetaTex.Load(int3(pc, 0)) * w;
                dirAcc += PrevGiDirTex.Load(int3(pc, 0)) * w;
                wSum += w;
            }
        }

        if (wSum > 1e-4f)
        {
            float invW = rcp(wSum);
            prevGi = giAcc * invW;
            prevMeta = metaAcc * invW;
            prevDir = dirAcc * invW;
            validHistory = true;
        }
    }

    float3 centerPos = ReconstructViewPos(uv, centerDepth, TanHalfFov, TanHalfFovAspect);
    float3 viewDir = normalize(centerPos);
    float3 toEye = -viewDir;

    // R2 quasirandom spatiotemporal blue-noise sequence
    float2 r2 = R2Sequence(uint(FrameIndex));
    float noiseA = frac(InterleavedGradientNoise(float2(giCoord)) + r2.x);
    float noiseB = frac(InterleavedGradientNoise(float2(giCoord) * 1.37f + float2(13.0f, 29.0f)) + r2.y);

    float maxRadiusPx = max(GiRadius, 24.0f);
    float2 invOutSize = 1.0f / float2(OutputWidth, OutputHeight);

    int slices = clamp(RayCount, 1, 2);
    int steps = clamp(StepCount, 3, 6);
    float tightRange = clamp(centerDepth * 0.08f, 0.25f, 1.35f);
    float broadRange = clamp(centerDepth * 0.24f, 0.90f, 4.5f);
    float aoThickness = clamp(centerDepth * 0.024f, 0.04f, 0.32f);
    float maxFgGap = clamp(centerDepth * 0.055f, 0.14f, 0.85f);
    float maxBroadGap = clamp(centerDepth * 0.10f, 0.30f, 1.60f);
    float giWorldRange = clamp(centerDepth * 0.45f, 2.5f, 12.0f);

    float3 accumGi = float3(0.0f, 0.0f, 0.0f);
    float accumAo = 0.0f;
    float3 bentNormalAccum = float3(0.0f, 0.0f, 0.0f);
    float3 giDirAccum = float3(0.0f, 0.0f, 0.0f);

    // 1. Dual-Scale Multi-Bounce GTAO + Bent Normal + Natural-Chroma SSGI
    [loop]
    for (int r = 0; r < slices; ++r)
    {
        float angle = (kPi * (float(r) + noiseA)) / float(slices);
        float2 dirPx = float2(cos(angle), sin(angle));

        float3 sliceDirView = normalize(float3(dirPx.x * TanHalfFovAspect, -dirPx.y * TanHalfFov, 0.0f));
        float3 planeNormal = normalize(cross(sliceDirView, toEye));
        float3 orthoTangent = cross(toEye, planeNormal);
        float3 projNormal = viewNormal - planeNormal * dot(viewNormal, planeNormal);
        float projNLen = length(projNormal);
        float3 projNDir = (projNLen > 1e-4f) ? (projNormal / projNLen) : toEye;
        float cosN = (projNLen > 1e-4f) ? clamp(dot(projNDir, toEye), -1.0f, 1.0f) : 1.0f;
        float sinN = (projNLen > 1e-4f) ? dot(projNDir, orthoTangent) : 0.0f;
        float nAngle = (projNLen > 1e-4f) ? atan2(sinN, cosN) : 0.0f;

        uint bitmaskTight = 0u;
        uint bitmaskBroad = 0u;
        float sliceTightOcc = 0.0f;
        float sliceBroadOcc = 0.0f;
        float occNeg = 0.0f;
        float occPos = 0.0f;
        float3 sliceGi = float3(0.0f, 0.0f, 0.0f);

        [loop]
        for (int dirSign = -1; dirSign <= 1; dirSign += 2)
        {
            float2 marchDirPx = dirPx * float(dirSign);

            [loop]
            for (int s = 1; s <= steps; ++s)
            {
                float t = (float(s) - 0.30f + noiseB * 0.45f) / float(steps);
                float radiusPx = lerp(2.5f, maxRadiusPx, t * t);
                float2 sampleUv = uv + marchDirPx * radiusPx * invOutSize;

                if (any(sampleUv < 0.002f) || any(sampleUv > 0.998f))
                {
                    float3 exitDir = normalize(lerp(viewNormal, float3(marchDirPx.x, -marchDirPx.y, -0.3f), 0.55f));
                    float3 fb = QueryFallbackRadiance(centerPos, exitDir, centerDepth * 0.5f, float2(noiseA, noiseB)) * 0.06f;
                    sliceGi += fb;
                    giDirAccum += exitDir * Luma(fb);
                    break;
                }

                int2 sGiCoord = ClampGiCoord(int2(sampleUv * float2(GiWidth, GiHeight)));
                float sampleDepth = HalfColorDepthTex.Load(int3(sGiCoord, 0)).a;
                if (sampleDepth >= 64000.0f || sampleDepth >= SkyDepthThreshold)
                    continue;

                float3 samplePos = ReconstructViewPos(sampleUv, sampleDepth, TanHalfFov, TanHalfFovAspect);
                float3 deltaFront = samplePos - centerPos;
                float distSq = dot(deltaFront, deltaFront);
                if (distSq < 1e-5f || distSq > giWorldRange * giWorldRange)
                    continue;

                float dist = sqrt(distSq);
                float3 frontDir = deltaFront / dist;

                // Dual-Scale Visibility-Bitmask AO + Directional Occlusion for Bent Normals
                float fgGap = centerDepth - sampleDepth;
                if (fgGap <= maxBroadGap && dist < broadRange)
                {
                    float3 backDir = normalize(deltaFront - toEye * aoThickness);
                    float fhX = dot(frontDir, toEye);
                    float fhY = dot(frontDir, orthoTangent);
                    float hFront = (abs(fhX) + abs(fhY) > 1e-5f) ? atan2(fhY, fhX) : 0.0f;
                    float bhX = dot(backDir, toEye);
                    float bhY = dot(backDir, orthoTangent);
                    float hBack = (abs(bhX) + abs(bhY) > 1e-5f) ? atan2(bhY, bhX) : hFront;

                    float hF = hFront - nAngle;
                    if (hF > kPi) hF -= 2.0f * kPi;
                    else if (hF < -kPi) hF += 2.0f * kPi;

                    float hB = hBack - nAngle;
                    if (hB > kPi) hB -= 2.0f * kPi;
                    else if (hB < -kPi) hB += 2.0f * kPi;

                    float a0 = clamp((min(hF, hB) + kPi * 0.5f) / kPi, 0.0f, 1.0f);
                    float a1 = clamp((max(hF, hB) + kPi * 0.5f) / kPi, 0.0f, 1.0f);

                    uint startBit = min(uint(floor(a0 * 32.0f)), 31u);
                    uint endBit = min(uint(ceil(a1 * 32.0f)), 32u);
                    uint bitCount = (endBit > startBit) ? (endBit - startBit) : 0u;
                    if (bitCount > 0u)
                    {
                        uint sectorMask = (bitCount >= 32u) ? (0xFFFFFFFFu << startBit)
                                                            : (((1u << bitCount) - 1u) << startBit);

                        if (fgGap <= maxFgGap && dist < tightRange)
                        {
                            uint newlyTight = sectorMask & ~bitmaskTight;
                            bitmaskTight |= sectorMask;
                            float tightFade = pow(1.0f - saturate(dist / tightRange), 1.35f);
                            float tOcc = (float(countbits(newlyTight)) / 32.0f) * tightFade;
                            sliceTightOcc += tOcc;
                            if (dirSign < 0) occNeg += tOcc * 0.65f;
                            else occPos += tOcc * 0.65f;
                        }

                        uint newlyBroad = sectorMask & ~bitmaskBroad;
                        bitmaskBroad |= sectorMask;
                        float broadFade = pow(1.0f - saturate(dist / broadRange), 2.1f);
                        float bOcc = (float(countbits(newlyBroad)) / 32.0f) * broadFade;
                        sliceBroadOcc += bOcc;
                        if (dirSign < 0) occNeg += bOcc * 0.55f;
                        else occPos += bOcc * 0.55f;
                    }
                }

                // Natural-Chroma Front-Hemisphere SSGI (restored saturation balance for bright sand & foliage)
                float fgStep = max(centerDepth - sampleDepth, 0.0f);
                float fgReject = 1.0f - smoothstep(clamp(centerDepth * 0.12f, 0.22f, 1.2f),
                                                   clamp(centerDepth * 0.30f, 0.55f, 3.0f), fgStep);
                if (fgReject > 0.01f)
                {
                    float nDotL = saturate(dot(viewNormal, frontDir) - 0.02f);
                    float2 sNxy = clamp(HalfNormalTex.Load(int3(sGiCoord, 0)).xy, -1.0f, 1.0f);
                    float3 sNorm = float3(sNxy, -sqrt(max(0.0f, 1.0f - dot(sNxy, sNxy))));
                    float emitterFace = smoothstep(-0.12f, 0.85f, dot(sNorm, -frontDir));

                    if (nDotL > 0.0f && emitterFace > 0.01f)
                    {
                        float mipLvl = clamp(log2(max(radiusPx * 0.22f, 1.0f)), 0.0f, 4.0f);
                        float4 pyrSample = ColorPyramidTex.SampleLevel(LinearClampPyr, sampleUv, mipLvl);
                        float3 sampleColor = (abs(pyrSample.a - sampleDepth) <= max(sampleDepth * 0.20f, 0.10f))
                                                 ? pyrSample.rgb
                                                 : HalfColorDepthTex.Load(int3(sGiCoord, 0)).rgb;
                        sampleColor = clamp(sampleColor, 0.0f, 4.5f);

                        float hitLuma = Luma(sampleColor);
                        float lightSignal = smoothstep(0.12f, 0.58f, hitLuma);
                        float emitterBoost = 1.0f + smoothstep(0.55f, 1.80f, hitLuma) * 1.15f;
                        // Slightly restrain bounce chroma (0.88) instead of amplifying it -> zero neon sand/grass
                        float3 hitCol = max(lerp(hitLuma.xxx, sampleColor, 0.88f), 0.0f);

                        float normD = dist / max(giWorldRange * 0.38f, 0.5f);
                        float giAtten = rcp(1.0f + normD * normD) * pow(1.0f - saturate(dist / giWorldRange), 1.25f);

                        float3 hitContrib = hitCol * nDotL * emitterFace * lightSignal * emitterBoost * giAtten *
                                            fgReject * (2.10f / float(steps));
                        sliceGi += hitContrib;
                        giDirAccum += frontDir * Luma(hitContrib);
                    }
                }
            }
        }

        float sliceTightAo = saturate(1.0f - sliceTightOcc);
        float sliceBroadAo = saturate(1.0f - sliceBroadOcc * 0.72f);
        float sliceVis = sliceTightAo * sliceBroadAo;
        accumAo += sliceVis;
        accumGi += sliceGi;

        float bentShift = clamp(occNeg - occPos, -0.90f, 0.90f);
        float3 sliceBent = normalize(projNDir + orthoTangent * bentShift);
        bentNormalAccum += sliceBent * (0.25f + 0.75f * sliceVis);
    }

    float3 currentGi = max(accumGi / float(slices), 0.0f);
    float distAoFade = rcp(1.0f + centerDepth * 0.010f);
    float currentAo = lerp(1.0f, saturate(accumAo / float(slices)), distAoFade);

    float3 bentNormal = (dot(bentNormalAccum, bentNormalAccum) > 1e-6f)
                            ? normalize(lerp(viewNormal, normalize(bentNormalAccum), 0.75f))
                            : viewNormal;
    bentNormal.z = min(bentNormal.z, -0.05f);
    bentNormal = normalize(bentNormal);

    float3 giDir = (dot(giDirAccum, giDirAccum) > 1e-6f) ? normalize(giDirAccum) : bentNormal;

    // 2. Soft-Penumbra Ray-Marched Contact Shadows toward Dominant Key Light
    float contactShadow = 1.0f;
    float4 keyDirData = ResolvedSceneLightBuf[4];
    float3 keyDirView = normalize(keyDirData.xyz);
    float keyStrength = saturate(keyDirData.w);
    float nDotKey = dot(viewNormal, keyDirView);
    if (nDotKey > 0.02f && keyStrength > 0.05f)
    {
        int cSteps = clamp(int(ContactShadowSteps), 2, 6);
        float maxShadowDist = clamp(centerDepth * 0.065f, 0.08f, 1.80f);
        float occ = 0.0f;
        float invTanV = rcp(max(TanHalfFov, 1e-4f));
        float invTanH = rcp(max(TanHalfFovAspect, 1e-4f));
        float maxThick = clamp(centerDepth * 0.032f, 0.035f, 0.42f);
        float minThick = centerDepth * 0.0025f;

        [loop]
        for (int cs = 1; cs <= cSteps; ++cs)
        {
            float fracS = (float(cs) - 0.25f + noiseB * 0.5f) / float(cSteps);
            float3 rayPos = centerPos + keyDirView * (maxShadowDist * fracS);
            if (rayPos.z <= 1e-3f)
                break;

            float2 rUv = ProjectViewPosToUv(rayPos, invTanV, invTanH);
            if (any(rUv < 0.002f) || any(rUv > 0.998f))
                break;

            float sampleD = HalfColorDepthTex.Load(int3(ClampGiCoord(int2(rUv * float2(GiWidth, GiHeight))), 0)).a;
            float diff = rayPos.z - sampleD;
            if (diff > minThick && diff < maxThick)
            {
                float win = smoothstep(minThick, minThick * 2.5f, diff) *
                            (1.0f - smoothstep(maxThick * 0.65f, maxThick, diff));
                occ = max(occ, win * (1.0f - fracS * 0.45f));
            }
        }
        float csDistFade = rcp(1.0f + centerDepth * 0.022f);
        float grassDamp = 1.0f - grassProb * 0.65f;
        contactShadow = saturate(1.0f - occ * saturate(nDotKey * 3.0f) * saturate(keyStrength * 1.4f) * csDistFade * grassDamp * 0.80f);
    }

    // 3. Warp-Coherent Fixed-Stride SSR + 2-Step Binary Refinement (zero divergent Hi-Z branching)
    float effRoughness = lerp(roughness, 0.14f, soilProb * wetness);
    float4 currentSsr = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float ssrHitDist = centerDepth * 0.5f;
    float2 ssrHitUv = uv;

    if (SsrEnabled > 0 && effRoughness < 0.80f && !(fabricProb > 0.65f && effRoughness > 0.65f))
    {
        float3 tUp = (abs(viewNormal.y) < 0.9f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        float3 tU = normalize(cross(tUp, viewNormal));
        float3 tV = cross(viewNormal, tU);
        float phi = noiseA * 2.0f * kPi;
        float ggxSpread = effRoughness * effRoughness * 0.32f * (1.0f - 0.50f * saturate(1.0f - dot(toEye, viewNormal)));
        float3 microNormal = normalize(viewNormal + (tU * cos(phi) + tV * sin(phi)) * ggxSpread);
        if (dot(microNormal, toEye) < 0.05f)
            microNormal = viewNormal;

        float3 reflDir = normalize(reflect(viewDir, microNormal));
        if (reflDir.z > -0.65f)
        {
            float ssrMaxDist = max(centerDepth * 3.0f, 0.2f);
            float minZClip = max(centerPos.z * 0.05f, 1e-3f);
            float rayLen = ssrMaxDist;
            if (reflDir.z < 0.0f)
                rayLen = min(rayLen, max((minZClip - centerPos.z) / reflDir.z, centerPos.z * 0.01f));

            float3 endPos = centerPos + reflDir * rayLen;
            float invTanV = rcp(max(TanHalfFov, 1e-4f));
            float invTanH = rcp(max(TanHalfFovAspect, 1e-4f));
            float2 endUv = ProjectViewPosToUv(endPos, invTanV, invTanH);

            float2 deltaUv = endUv - uv;
            float maxT = 1.0f;
            if (abs(deltaUv.x) > 1e-5f)
                maxT = min(maxT, ((deltaUv.x > 0.0f ? 0.996f : 0.004f) - uv.x) / deltaUv.x);
            if (abs(deltaUv.y) > 1e-5f)
                maxT = min(maxT, ((deltaUv.y > 0.0f ? 0.996f : 0.004f) - uv.y) / deltaUv.y);
            maxT = saturate(maxT);

            if (maxT > 0.02f && dot(deltaUv, deltaUv) > 1e-6f)
            {
                float2 clippedEndUv = uv + deltaUv * maxT;
                float invZ0 = rcp(centerPos.z);
                float endZ = rcp(lerp(invZ0, rcp(max(endPos.z, minZClip)), maxT));
                float invZ1 = rcp(endZ);

                int ssrSteps = clamp(SsrSteps, 4, 12);
                float invSteps = rcp(float(ssrSteps));
                float prevKt = 0.0f;
                float prevRayZ = centerPos.z;
                float hitKt = 0.0f;
                float2 hitUv = uv;
                float hitThickness = 0.0f;
                bool hit = false;
                float mipBias = (effRoughness < 0.22f) ? 0.0f : 1.0f;

                [loop]
                for (int k = 1; k <= ssrSteps; ++k)
                {
                    float kt = (float(k) - 0.55f + noiseB * 0.45f) * invSteps;
                    float kt2 = kt * kt;
                    float2 rayUv = lerp(uv, clippedEndUv, kt2);
                    float rayZ = rcp(lerp(invZ0, invZ1, kt2));
                    float rayDepth = ColorPyramidTex.SampleLevel(LinearClampPyr, rayUv, mipBias).a;

                    if (rayDepth < 64000.0f)
                    {
                        float diff = rayZ - rayDepth;
                        float stepThickness = max(rayDepth * 0.14f, abs(rayZ - prevRayZ) * 1.45f);
                        if (diff > rayZ * 0.002f && diff < stepThickness)
                        {
                            hit = true;
                            hitKt = kt2;
                            hitUv = rayUv;
                            hitThickness = stepThickness;
                            break;
                        }
                    }

                    prevKt = kt2;
                    prevRayZ = rayZ;
                }

                if (hit)
                {
                    float loKt = prevKt;
                    float hiKt = hitKt;
                    [unroll]
                    for (int b = 0; b < 2; ++b)
                    {
                        float midKt = 0.5f * (loKt + hiKt);
                        float2 midUv = lerp(uv, clippedEndUv, midKt);
                        float midZ = rcp(lerp(invZ0, invZ1, midKt));
                        float midDepth = HalfColorDepthTex.Load(int3(ClampGiCoord(int2(midUv * float2(GiWidth, GiHeight))), 0)).a;
                        float midDiff = midZ - midDepth;
                        if (midDepth < 64000.0f && midDiff > midZ * 0.002f && midDiff < hitThickness)
                        {
                            hiKt = midKt;
                            hitUv = midUv;
                        }
                        else
                        {
                            loKt = midKt;
                        }
                    }

                    int2 hitCoord = ClampGiCoord(int2(hitUv * float2(GiWidth, GiHeight)));
                    float4 hitCd = HalfColorDepthTex.Load(int3(hitCoord, 0));
                    float hitPxDist = length((hitUv - uv) * float2(OutputWidth, OutputHeight));
                    float coneMip = clamp(log2(max(hitPxDist * effRoughness * 0.40f, 1.0f)), 0.0f, 4.0f);
                    float3 hitCol = hitCd.rgb;
                    if (coneMip >= 0.35f)
                    {
                        float4 pyrHit = ColorPyramidTex.SampleLevel(LinearClampPyr, hitUv, coneMip);
                        if (abs(pyrHit.a - hitCd.a) <= max(hitCd.a * 0.18f, 0.08f))
                            hitCol = pyrHit.rgb;
                    }

                    float2 edgeDist = min(hitUv, 1.0f - hitUv);
                    float edgeFade = saturate(min(edgeDist.x, edgeDist.y) * 9.0f);
                    float distFade = 1.0f - saturate(hiKt * maxT * 0.70f);
                    ssrHitDist = max(rayLen * maxT * hiKt, 0.05f);
                    ssrHitUv = hitUv;
                    currentSsr = float4(max(hitCol, 0.0f), edgeFade * lerp(0.55f, 1.0f, distFade));
                }
            }
        }

        if (currentSsr.a < 0.95f)
        {
            float3 envRefl = QueryFallbackRadiance(centerPos, reflDir, clamp(centerDepth * 1.2f, 1.5f, 24.0f), float2(noiseA, noiseB));
            if (reflDir.z > -0.60f)
            {
                float2 dirSampleUv = saturate(uv + reflDir.xy * 0.25f);
                float3 pyrRefl = max(ColorPyramidTex.SampleLevel(LinearClampPyr, dirSampleUv, 3.0f).rgb, 0.0f);
                envRefl = lerp(envRefl, pyrRefl, 0.55f);
            }
            currentSsr.rgb = lerp(envRefl, currentSsr.rgb, currentSsr.a);
            currentSsr.a = max(currentSsr.a, 0.22f * (1.0f - effRoughness));
        }
    }

    float solidTrust = 1.0f - flickerMask;
    currentGi *= solidTrust;
    currentAo = lerp(1.0f, currentAo, solidTrust);
    contactShadow = lerp(1.0f, contactShadow, solidTrust);
    currentSsr.a *= solidTrust;

    float4 giResult = float4(currentGi, currentAo);
    float4 ssrResult = currentSsr;
    float curLuma = Luma(currentGi);
    float variance = 0.25f;
    float histLen = 1.0f;
    float3 outBent = bentNormal;
    float3 outGiDir = giDir;

    if (validHistory)
    {
        histLen = min(max(prevMeta.a, 1.0f) + 1.0f, 32.0f);
        float baseAlpha = max(rcp(histLen), lerp(0.08f, 0.35f, saturate(mvMag / 16.0f)));

        float aoDiff = abs(prevGi.a - currentAo);
        float3 giDiff = abs(prevGi.rgb - currentGi);
        float giMaxDiff = max(giDiff.r, max(giDiff.g, giDiff.b));
        float giAlpha = max(baseAlpha, min(0.65f, max(saturate(aoDiff * 1.15f), saturate(giMaxDiff * 0.50f))));
        giResult = lerp(prevGi, giResult, saturate(giAlpha));

        contactShadow = lerp(prevMeta.r, contactShadow, max(baseAlpha, 0.35f));

        float prevLuma = Luma(prevGi.rgb);
        float instVar = abs(curLuma - prevLuma) / max(curLuma + prevLuma + 0.05f, 0.05f);
        variance = clamp(lerp(prevMeta.g, instVar, 0.35f), 0.02f, 1.0f);

        float2 pbxy = clamp(prevDir.xy, -1.0f, 1.0f);
        float3 prevBent = float3(pbxy, -sqrt(max(0.0f, 1.0f - dot(pbxy, pbxy))));
        outBent = normalize(lerp(prevBent, bentNormal, saturate(giAlpha)));
        float3 prevGiD = OctDecode(prevDir.zw);
        outGiDir = normalize(lerp(prevGiD, giDir, saturate(giAlpha)));

        if (SsrEnabled > 0)
        {
            float2 ssrPrevUv = prevUv;
            if (currentSsr.a > 0.25f)
            {
                float2 hitApproxUv = saturate(ssrHitUv);
                int2 hitMvCoord = ClampMotionCoord(int2(MvSubrectBaseX, MvSubrectBaseY) + int2(hitApproxUv * float2(activeW, activeH)));
                float2 hitMv = MotionTex.Load(int3(hitMvCoord, 0)).rg * float2(MvScaleX, MvScaleY);
                float2 hitPrevUv = uv + hitMv / float2(max(activeW, 1), max(activeH, 1));
                float virtBlend = saturate(ssrHitDist / max(centerDepth * 1.5f, 0.5f)) * (1.0f - effRoughness) * 0.55f;
                ssrPrevUv = lerp(prevUv, hitPrevUv, virtBlend);
            }
            ssrPrevUv = clamp(ssrPrevUv, 0.002f, 0.998f);
            float4 prevSsr = PrevSsrTex.SampleLevel(LinearClampSsr, ssrPrevUv, 0.0f);
            float3 ssrDiff = abs(prevSsr.rgb - currentSsr.rgb);
            float ssrMaxDiff = max(ssrDiff.r, max(ssrDiff.g, ssrDiff.b));
            float ssrAlpha = max(lerp(0.16f, 0.55f, saturate(mvMag / 12.0f)), min(0.70f, saturate(ssrMaxDiff * 0.45f)));
            ssrResult = lerp(prevSsr, currentSsr, saturate(ssrAlpha));
        }
    }

    OutGiTex[giCoord] = float4(max(giResult.rgb, 0.0f), saturate(giResult.a));
    OutSsrTex[giCoord] = float4(max(ssrResult.rgb, 0.0f), saturate(ssrResult.a));
    OutGiMetaTex[giCoord] = float4(contactShadow, variance, ssrHitDist, histLen);
    OutGiDirTex[giCoord] = float4(outBent.xy, OctEncode(outGiDir));
}
