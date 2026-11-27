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
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]]
#endif
Texture2D<float4> PrevHalfNormalTex : register(t3);

#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
Texture2D<float2> MotionTex : register(t4);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]]
#endif
Texture2D<float4> PrevGiTex : register(t5);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
#endif
Texture2D<float4> PrevSsrTex : register(t6);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]]
#endif
Texture2D<float4> Material1Tex : register(t7);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]]
SamplerState LinearClampPyr : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]]
SamplerState LinearClampNorm : register(s1);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]]
SamplerState LinearClampGi : register(s2);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
SamplerState LinearClampSsr : register(s3);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]]
SamplerState LinearClampMat : register(s4);
#else
SamplerState LinearClamp : register(s0);
#define LinearClampPyr LinearClamp
#define LinearClampNorm LinearClamp
#define LinearClampGi LinearClamp
#define LinearClampSsr LinearClamp
#define LinearClampMat LinearClamp
#endif

#ifdef VK_MODE
[[vk::binding(9, 0)]]
#endif
StructuredBuffer<float4> ResolvedSceneLightBuf : register(t8);

#ifdef VK_MODE
[[vk::binding(10, 0)]]
#endif
StructuredBuffer<uint> WorldHashBuf : register(t9);

#ifdef VK_MODE
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutGiTex : register(u0);

#ifdef VK_MODE
[[vk::binding(12, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutSsrTex : register(u1);

#ifdef VK_MODE
[[vk::binding(13, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutGiMetaTex : register(u2);

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

float3 QueryFallbackRadiance(float3 viewPos, float3 rayDirView, float rayDist)
{
    float3 rayDir = (HasCameraBasis > 0) ? ViewToWorldDir(rayDirView, CameraRight, CameraUp, CameraFwd) : rayDirView;
    if (WorldHashEnabled > 0 && HasCameraBasis > 0)
    {
        float3 probeView = viewPos + rayDirView * rayDist;
        float3 probeWorld = ViewToWorldPos(probeView, CameraPos, CameraRight, CameraUp, CameraFwd);
        float3 hashRad;
        if (LookupWorldHash(WorldHashBuf, probeWorld, -rayDir, max(viewPos.z, 1.0f), FrameIndex, hashRad))
            return hashRad;
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
        OutGiMetaTex[giCoord] = float4(1.0f, 0.0f, 0.0f, 0.0f);
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
        OutGiMetaTex[giCoord] = float4(1.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // Fixed MV coordinate & scaling (handles LowResMV + subrect)
    int activeW = (DisplaySizeMV > 0) ? OutputWidth : RenderWidth;
    int activeH = (DisplaySizeMV > 0) ? OutputHeight : RenderHeight;
    int2 mvCoord = ClampMotionCoord(int2(MvSubrectBaseX, MvSubrectBaseY) + int2(uv * float2(activeW, activeH)));
    float2 mv = MotionTex.Load(int3(mvCoord, 0)).rg * float2(MvScaleX, MvScaleY);
    float2 mvUv = mv / float2(max(activeW, 1), max(activeH, 1));
    float mvMag = length(mvUv * float2(OutputWidth, OutputHeight));
    float2 prevUv = uv + mvUv;

    bool inBounds = (ResetHistory == 0 && FrameIndex > 3 && all(prevUv >= 0.002f) && all(prevUv <= 0.998f));

    // Strict nearest-texel Depth + Normal Disocclusion Rejection (no bilinear silhouette smear, no checkerboard skip)
    bool validHistory = false;
    if (inBounds)
    {
        int2 prevGiCoord = ClampGiCoord(int2(prevUv * float2(GiWidth, GiHeight)));
        float4 prevNormData = PrevHalfNormalTex.Load(int3(prevGiCoord, 0));
        float relDepthErr = abs(centerDepth - prevNormData.w) / max(min(centerDepth, prevNormData.w), 0.05f);
        float2 pnxy = clamp(prevNormData.xy, -1.0f, 1.0f);
        float3 prevNorm = float3(pnxy, -sqrt(max(0.0f, 1.0f - dot(pnxy, pnxy))));
        if (relDepthErr < 0.045f && dot(viewNormal, prevNorm) > 0.72f)
            validHistory = true;
    }

    float3 centerPos = ReconstructViewPos(uv, centerDepth, TanHalfFov, TanHalfFovAspect);
    float3 viewDir = normalize(centerPos);
    float3 toEye = -viewDir;

    float noiseA = InterleavedGradientNoise(
        float2(giCoord) + float2((FrameIndex & 15) * 5.588238f, (FrameIndex & 15) * 3.1732f));
    float noiseB = frac(noiseA * 1.6180339f + 0.371f);

    float maxRadiusPx = max(GiRadius, 24.0f);
    float2 invOutSize = 1.0f / float2(OutputWidth, OutputHeight);

    int slices = clamp(RayCount, 1, 4);
    int steps = clamp(StepCount, 3, 8);
    float tightRange = clamp(centerDepth * 0.08f, 0.25f, 1.35f);
    float broadRange = clamp(centerDepth * 0.24f, 0.90f, 4.5f);
    float aoThickness = clamp(centerDepth * 0.024f, 0.04f, 0.32f);
    float maxFgGap = clamp(centerDepth * 0.055f, 0.14f, 0.85f);
    float maxBroadGap = clamp(centerDepth * 0.10f, 0.30f, 1.60f);
    float giWorldRange = clamp(centerDepth * 0.48f, 2.5f, 13.0f);

    float3 accumGi = float3(0.0f, 0.0f, 0.0f);
    float accumAo = 0.0f;

    // 1. Dual-Scale Multi-Bounce GTAO + Strict Front-Hemisphere Chromatic SSGI
    [loop]
    for (int r = 0; r < slices; ++r)
    {
        float angle = (kPi * (float(r) + noiseA)) / float(slices);
        float2 dirPx = float2(cos(angle), sin(angle));

        // Project normal onto slice plane using exact atan2 signed angles
        float3 sliceDirView = normalize(float3(dirPx.x * TanHalfFovAspect, -dirPx.y * TanHalfFov, 0.0f));
        float3 planeNormal = normalize(cross(sliceDirView, toEye));
        float3 orthoTangent = cross(toEye, planeNormal);
        float3 projNormal = viewNormal - planeNormal * dot(viewNormal, planeNormal);
        float projNLen = length(projNormal);
        float cosN = (projNLen > 1e-4f) ? clamp(dot(projNormal / projNLen, toEye), -1.0f, 1.0f) : 1.0f;
        float sinN = (projNLen > 1e-4f) ? dot(projNormal / projNLen, orthoTangent) : 0.0f;
        float nAngle = (projNLen > 1e-4f) ? atan2(sinN, cosN) : 0.0f;

        uint bitmaskTight = 0u;
        uint bitmaskBroad = 0u;
        float sliceTightOcc = 0.0f;
        float sliceBroadOcc = 0.0f;
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
                    sliceGi += QueryFallbackRadiance(centerPos, exitDir, centerDepth * 0.5f) * 0.08f;
                    break;
                }

                // Crisp half-res depth prevents Hi-Z minDepth 16x16 block outlines
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

                // Band 1: Dual-Scale Visibility-Bitmask AO (tight crevice contact + broad cavity depth)
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
                            sliceTightOcc += (float(countbits(newlyTight)) / 32.0f) * tightFade;
                        }

                        uint newlyBroad = sectorMask & ~bitmaskBroad;
                        bitmaskBroad |= sectorMask;
                        float broadFade = pow(1.0f - saturate(dist / broadRange), 2.1f);
                        sliceBroadOcc += (float(countbits(newlyBroad)) / 32.0f) * broadFade;
                    }
                }

                // Band 2: Strict Front-Hemisphere Chromatic Bounce GI (zero backside bleed onto dark backdrops!)
                float fgStep = max(centerDepth - sampleDepth, 0.0f);
                float fgReject = 1.0f - smoothstep(clamp(centerDepth * 0.12f, 0.22f, 1.2f),
                                                   clamp(centerDepth * 0.30f, 0.55f, 3.0f), fgStep);
                if (fgReject > 0.01f)
                {
                    float nDotL = saturate(dot(viewNormal, frontDir) - 0.02f);
                    float2 sNxy = clamp(HalfNormalTex.Load(int3(sGiCoord, 0)).xy, -1.0f, 1.0f);
                    float3 sNorm = float3(sNxy, -sqrt(max(0.0f, 1.0f - dot(sNxy, sNxy))));
                    float emitterDot = dot(sNorm, -frontDir);
                    float emitterFace = smoothstep(-0.12f, 0.85f, emitterDot);

                    if (nDotL > 0.0f && emitterFace > 0.01f)
                    {
                        float mipLvl = clamp(log2(max(radiusPx * 0.22f, 1.0f)), 0.0f, 4.0f);
                        float3 sampleColor = clamp(ColorPyramidTex.SampleLevel(LinearClampPyr, sampleUv, mipLvl).rgb, 0.0f, 6.0f);
                        float hitLuma = Luma(sampleColor);
                        float hitChroma = max(sampleColor.r, max(sampleColor.g, sampleColor.b)) -
                                          min(sampleColor.r, min(sampleColor.g, sampleColor.b));

                        float lightSignal = smoothstep(0.14f, 0.55f, hitLuma + hitChroma * 0.50f);
                        float emitterBoost = 1.0f + smoothstep(0.45f, 1.60f, hitLuma) * 1.85f + hitChroma * 0.90f;
                        float3 hitCol = max(lerp(hitLuma.xxx, sampleColor, 1.30f), 0.0f);

                        float normD = dist / max(giWorldRange * 0.38f, 0.5f);
                        float giAtten = rcp(1.0f + normD * normD) * pow(1.0f - saturate(dist / giWorldRange), 1.25f);

                        sliceGi += hitCol * nDotL * emitterFace * lightSignal * emitterBoost * giAtten * fgReject *
                                   (2.60f / float(steps));
                    }
                }
            }
        }

        float sliceTightAo = saturate(1.0f - sliceTightOcc);
        float sliceBroadAo = saturate(1.0f - sliceBroadOcc * 0.72f);
        accumAo += sliceTightAo * sliceBroadAo;
        accumGi += sliceGi;
    }

    float3 currentGi = max(accumGi / float(slices), 0.0f);
    float distAoFade = rcp(1.0f + centerDepth * 0.010f);
    float currentAo = lerp(1.0f, saturate(accumAo / float(slices)), distAoFade);

    // 2. Long-Range Soft Ray-Marched Contact Shadows toward Dominant Key Light (0.08m -> 1.80m)
    float contactShadow = 1.0f;
    float4 keyDirData = ResolvedSceneLightBuf[4];
    float3 keyDirView = normalize(keyDirData.xyz);
    float keyStrength = saturate(keyDirData.w);
    float nDotKey = dot(viewNormal, keyDirView);
    if (nDotKey > 0.02f && keyStrength > 0.05f)
    {
        int cSteps = clamp(int(ContactShadowSteps), 2, 8);
        float maxShadowDist = clamp(centerDepth * 0.065f, 0.08f, 1.80f);
        float occ = 0.0f;
        float invTanV = rcp(max(TanHalfFov, 1e-4f));
        float invTanH = rcp(max(TanHalfFovAspect, 1e-4f));

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
            float maxThick = clamp(centerDepth * 0.028f, 0.03f, 0.38f);
            if (diff > centerDepth * 0.002f && diff < maxThick)
            {
                occ = max(occ, 1.0f - fracS * 0.55f);
            }
        }
        float csDistFade = rcp(1.0f + centerDepth * 0.022f);
        contactShadow = saturate(1.0f - occ * saturate(nDotKey * 3.0f) * saturate(keyStrength * 1.4f) * csDistFade * 0.82f);
    }

    // 3. Hi-Z Roughness-Cone SSR (skip diffuse surfaces roughness > 0.85) + Virtual Hit Distance
    float4 mat1 = Material1Tex.SampleLevel(LinearClampMat, uv, 0.0f);
    float roughness = saturate(mat1.r);
    float4 currentSsr = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float ssrHitDist = centerDepth * 0.5f;
    float2 ssrHitUv = uv;

    if (SsrEnabled > 0 && roughness < 0.85f)
    {
        float3 reflDir = normalize(reflect(viewDir, viewNormal));
        if (reflDir.z > -0.65f)
        {
            float ssrMaxDist = max(centerDepth * 3.5f, 0.2f);
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

                [loop]
                for (int k = 1; k <= ssrSteps; ++k)
                {
                    float kt = (float(k) + noiseB * 0.35f) * invSteps;
                    float kt2 = kt * kt;
                    float2 rayUv = lerp(uv, clippedEndUv, kt2);
                    float rayZ = rcp(lerp(invZ0, invZ1, kt2));

                    // Sample crisp half-res depth to avoid 1/4-res min-depth silhouette bloat on reflections
                    float rayDepth = HalfColorDepthTex.Load(int3(ClampGiCoord(int2(rayUv * float2(GiWidth, GiHeight))), 0)).a;

                    if (rayDepth < 64000.0f)
                    {
                        float diff = rayZ - rayDepth;
                        float stepThickness = max(rayDepth * 0.12f, abs(rayZ - prevRayZ) * 1.35f);
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

                    float hitPxDist = length((hitUv - uv) * float2(OutputWidth, OutputHeight));
                    float coneMip = clamp(log2(max(hitPxDist * roughness * 0.40f, 1.0f)), 0.0f, 4.0f);
                    float3 hitCol = (coneMip < 0.35f)
                                        ? HalfColorDepthTex.Load(int3(ClampGiCoord(int2(hitUv * float2(GiWidth, GiHeight))), 0)).rgb
                                        : ColorPyramidTex.SampleLevel(LinearClampPyr, hitUv, coneMip).rgb;

                    float2 edgeDist = min(hitUv, 1.0f - hitUv);
                    float edgeFade = saturate(min(edgeDist.x, edgeDist.y) * 9.0f);
                    float distFade = 1.0f - saturate(hiKt * maxT * 0.70f);
                    ssrHitDist = max(rayLen * maxT * hiKt, 0.05f);
                    ssrHitUv = hitUv;
                    currentSsr = float4(max(hitCol, 0.0f), edgeFade * lerp(0.55f, 1.0f, distFade));
                }
            }
        }

        // High-impact off-screen reflection fallback: directional pyramid + World Hash / Scene SH
        if (currentSsr.a < 0.95f)
        {
            float3 envRefl = QueryFallbackRadiance(centerPos, reflDir, clamp(centerDepth * 1.2f, 1.5f, 24.0f));
            if (reflDir.z > -0.60f)
            {
                float2 dirSampleUv = saturate(uv + reflDir.xy * 0.25f);
                float3 pyrRefl = max(ColorPyramidTex.SampleLevel(LinearClampPyr, dirSampleUv, 3.0f).rgb, 0.0f);
                envRefl = lerp(envRefl, pyrRefl, 0.55f);
            }
            currentSsr.rgb = lerp(envRefl, currentSsr.rgb, currentSsr.a);
            currentSsr.a = max(currentSsr.a, 0.24f * (1.0f - roughness));
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

    if (validHistory)
    {
        float baseAlpha = lerp(0.12f, 0.42f, saturate(mvMag / 16.0f));
        float4 prevGi = PrevGiTex.SampleLevel(LinearClampGi, prevUv, 0.0f);

        float aoDiff = abs(prevGi.a - currentAo);
        float3 giDiff = abs(prevGi.rgb - currentGi);
        float giMaxDiff = max(giDiff.r, max(giDiff.g, giDiff.b));
        float giAlpha = max(baseAlpha, min(0.65f, max(saturate(aoDiff * 1.15f), saturate(giMaxDiff * 0.50f))));
        giResult = lerp(prevGi, giResult, saturate(giAlpha));

        float prevLuma = Luma(prevGi.rgb);
        variance = clamp(abs(curLuma - prevLuma) / max(curLuma + prevLuma + 0.05f, 0.05f), 0.02f, 1.0f);
        histLen = 8.0f;

        if (SsrEnabled > 0)
        {
            // Virtual-hit MV blending for SSR: blend surface MV with reflected-hit pixel MV to reduce floor smear
            float2 ssrPrevUv = prevUv;
            if (currentSsr.a > 0.25f)
            {
                float2 hitApproxUv = saturate(ssrHitUv);
                int2 hitMvCoord = ClampMotionCoord(int2(MvSubrectBaseX, MvSubrectBaseY) + int2(hitApproxUv * float2(activeW, activeH)));
                float2 hitMv = MotionTex.Load(int3(hitMvCoord, 0)).rg * float2(MvScaleX, MvScaleY);
                float2 hitPrevUv = uv + hitMv / float2(max(activeW, 1), max(activeH, 1));
                float virtBlend = saturate(ssrHitDist / max(centerDepth * 1.5f, 0.5f)) * (1.0f - roughness) * 0.55f;
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
}
