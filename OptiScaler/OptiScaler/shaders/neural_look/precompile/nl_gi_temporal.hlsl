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

    int MotionWidth;
    int MotionHeight;
    int DisplaySizeMV;
    int ResetHistory;

    float SkyDepthThreshold;
    float MvScaleX;
    float MvScaleY;
    float MotionTextureScale;

    float TanHalfFov;
    float TanHalfFovAspect;
    float GiRadius;
    float GiBias;

    int RayCount;
    int StepCount;
    int FrameIndex;
    int SsrEnabled;

    int SsrSteps;
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
Texture2D<float2> MotionTex : register(t2);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]]
#endif
Texture2D<float4> PrevGiTex : register(t3);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]]
#endif
Texture2D<float4> PrevSsrTex : register(t4);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]]
SamplerState LinearClampGi : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]]
SamplerState LinearClampSsr : register(s1);
#else
SamplerState LinearClamp : register(s0);
#define LinearClampGi LinearClamp
#define LinearClampSsr LinearClamp
#endif

#ifdef VK_MODE
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutGiTex : register(u0);

#ifdef VK_MODE
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutSsrTex : register(u1);

static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);
static const float kPi = 3.14159265f;

float Luma(float3 c)
{
    return dot(c, kLumaCoeff);
}

int2 ClampGiCoord(int2 p)
{
    return int2(clamp(p.x, 0, GiWidth - 1), clamp(p.y, 0, GiHeight - 1));
}

int2 ClampMotionCoord(int2 p)
{
    return int2(clamp(p.x, 0, MotionWidth - 1), clamp(p.y, 0, MotionHeight - 1));
}

float3 ReconstructViewPos(float2 uv, float linearDepth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    return float3(ndc.x * TanHalfFovAspect * linearDepth, ndc.y * TanHalfFov * linearDepth, linearDepth);
}

float2 ProjectViewPosToUv(float3 p, float invTanHalfFov, float invTanHalfFovAspect)
{
    float invZ = rcp(max(p.z, 1e-4f));
    float ndcX = p.x * invZ * invTanHalfFovAspect;
    float ndcY = p.y * invZ * invTanHalfFov;
    return float2(ndcX * 0.5f + 0.5f, 0.5f - ndcY * 0.5f);
}

float InterleavedGradientNoise(float2 p)
{
    float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(p, magic.xy)));
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
        return;
    }

    int2 outCoord = int2(uv * float2(OutputWidth, OutputHeight));
    int2 mvCoord = DisplaySizeMV > 0 ? outCoord : int2((float2(outCoord) + 0.5f) * MotionTextureScale);
    float2 mv = MotionTex.Load(int3(ClampMotionCoord(mvCoord), 0)).rg * float2(MvScaleX, MvScaleY);
    float mvMag = length(mv);
    float2 prevUv = uv + mv / float2(OutputWidth, OutputHeight);
    bool validHistory = (ResetHistory == 0 && FrameIndex > 3 && all(prevUv >= 0.002f) && all(prevUv <= 0.998f));

    // Checkerboard temporal raymarching: trace 50% of half-res pixels per frame, reuse reprojected history on the rest
    bool traceThisFrame = (((giCoord.x ^ giCoord.y) & 1) == (FrameIndex & 1)) || !validHistory || (mvMag > 6.0f);
    if (!traceThisFrame)
    {
        OutGiTex[giCoord] = PrevGiTex.SampleLevel(LinearClampGi, prevUv, 0.0f);
        OutSsrTex[giCoord] = (SsrEnabled > 0) ? PrevSsrTex.SampleLevel(LinearClampSsr, prevUv, 0.0f)
                                              : float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 centerPos = ReconstructViewPos(uv, centerDepth);

    float noiseA = InterleavedGradientNoise(
        float2(giCoord) + float2((FrameIndex & 15) * 5.588238f, (FrameIndex & 15) * 3.1732f));
    float noiseB = frac(noiseA * 1.6180339f + 0.371f);

    float maxRadiusPx = max(GiRadius, 24.0f);
    float2 invOutSize = 1.0f / float2(OutputWidth, OutputHeight);
    float2 halfSize = float2(GiWidth, GiHeight);

    float3 accumGi = float3(0.0f, 0.0f, 0.0f);
    float accumAo = 0.0f;

    int rays = clamp(RayCount, 1, 8);
    int steps = clamp(StepCount, 2, 8);
    float worldRange = clamp(centerDepth * 0.32f, 1.5f, 26.0f);
    float worldRangeSq = worldRange * worldRange;
    float invWorldRange = rcp(worldRange);
    float invContactRange = rcp(max(centerDepth * 0.075f, 0.45f));
    float thicknessLimit = clamp(centerDepth * 0.16f, 0.25f, 7.0f);

    [loop]
    for (int r = 0; r < rays; ++r)
    {
        float angle = (2.0f * kPi * (float(r) + noiseA)) / float(rays);
        float2 dirPx = float2(cos(angle), sin(angle));

        float maxHorizonSin = 0.0f;
        float microContactOcc = 0.0f;
        float3 rayGi = float3(0.0f, 0.0f, 0.0f);
        float rayWeightSum = 0.0f;

        [loop]
        for (int s = 1; s <= steps; ++s)
        {
            float t = (float(s) - 0.35f + noiseB * 0.5f) / float(steps);
            float radiusPx = lerp(2.0f, maxRadiusPx, t * t);
            float2 sampleUv = uv + dirPx * radiusPx * invOutSize;

            if (any(sampleUv < 0.001f) || any(sampleUv > 0.999f))
                break;

            int2 sampleGi = ClampGiCoord(int2(sampleUv * halfSize));
            float4 sampleCd = HalfColorDepthTex.Load(int3(sampleGi, 0));
            float sampleDepth = sampleCd.a;
            if (sampleDepth >= 64000.0f)
                continue;

            float3 samplePos = ReconstructViewPos(sampleUv, sampleDepth);
            float3 delta = samplePos - centerPos;
            float distSq = dot(delta, delta);

            if (distSq < 1e-6f || distSq > worldRangeSq)
                continue;

            float depthDelta = centerDepth - sampleDepth;
            if (depthDelta > thicknessLimit)
                continue;

            float invDist = rsqrt(distSq);
            float dist = distSq * invDist;
            float3 dir3D = delta * invDist;
            float nDotD = dot(viewNormal, dir3D);

            float rangeFade = 1.0f - saturate(dist * invWorldRange);
            float elev = nDotD - GiBias;

            if (s <= 2 && elev > 0.02f)
                microContactOcc = max(microContactOcc, saturate(elev * 1.65f) * (1.0f - saturate(dist * invContactRange)));

            if (elev > maxHorizonSin)
            {
                float aoDelta = saturate(elev) * rangeFade;
                maxHorizonSin = elev;

                float3 hitColor = sampleCd.rgb;
                float hitLuma = Luma(hitColor);
                hitColor = max(lerp(hitLuma.xxx, hitColor, 1.35f), 0.0f);

                rayGi += hitColor * saturate(nDotD) * rangeFade * aoDelta;
                rayWeightSum += rangeFade;
            }
        }

        accumAo += saturate(1.0f - max(maxHorizonSin * 1.05f, microContactOcc * 0.92f));
        if (rayWeightSum > 1e-4f)
            accumGi += rayGi / max(rayWeightSum, 0.70f);
    }

    float3 currentGi = max(accumGi / float(rays), 0.0f);
    float currentAo = saturate(accumAo / float(rays));

    // Screen-space clipped linear SSR raymarching using L2-cached half-res color/depth
    float4 currentSsr = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (SsrEnabled > 0)
    {
        float3 viewDir = normalize(centerPos);
        float3 reflDir = normalize(reflect(viewDir, viewNormal));

        if (reflDir.z > -0.60f)
        {
            float ssrMaxDist = clamp(centerDepth * 2.4f, 8.0f, 75.0f);
            float ssrThickness = clamp(centerDepth * 0.11f, 0.22f, 4.5f);

            float rayLen = ssrMaxDist;
            if (reflDir.z < 0.0f)
                rayLen = min(rayLen, max((0.1f - centerPos.z) / reflDir.z, 0.05f));

            float3 endPos = centerPos + reflDir * rayLen;
            float invTanHalfFov = rcp(max(TanHalfFov, 1e-4f));
            float invTanHalfFovAspect = rcp(max(TanHalfFovAspect, 1e-4f));
            float2 endUv = ProjectViewPosToUv(endPos, invTanHalfFov, invTanHalfFovAspect);

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
                float endZ = rcp(lerp(invZ0, rcp(max(endPos.z, 0.1f)), maxT));
                float invZ1 = rcp(endZ);

                int ssrSteps = clamp(SsrSteps, 4, 12);
                float invSsrSteps = rcp(float(ssrSteps));
                float prevKt = 0.0f;
                float hitKt = 0.0f;
                float2 hitUv = uv;
                bool hit = false;

                [loop]
                for (int k = 1; k <= ssrSteps; ++k)
                {
                    float kt = (float(k) + noiseB * 0.40f) * invSsrSteps;
                    float kt2 = kt * kt;
                    float2 rayUv = lerp(uv, clippedEndUv, kt2);
                    float rayZ = rcp(lerp(invZ0, invZ1, kt2));

                    float rayDepth = HalfColorDepthTex.Load(int3(ClampGiCoord(int2(rayUv * halfSize)), 0)).a;
                    float diff = rayZ - rayDepth;
                    if (diff > 0.0f && diff < ssrThickness)
                    {
                        hit = true;
                        hitKt = kt2;
                        hitUv = rayUv;
                        break;
                    }

                    prevKt = kt2;
                }

                if (hit)
                {
                    float midKt = 0.5f * (prevKt + hitKt);
                    float2 midUv = lerp(uv, clippedEndUv, midKt);
                    float midZ = rcp(lerp(invZ0, invZ1, midKt));
                    float4 midCd = HalfColorDepthTex.Load(int3(ClampGiCoord(int2(midUv * halfSize)), 0));
                    float midDiff = midZ - midCd.a;
                    float3 hitCol = (midDiff > 0.0f && midDiff < ssrThickness)
                                        ? midCd.rgb
                                        : HalfColorDepthTex.Load(int3(ClampGiCoord(int2(hitUv * halfSize)), 0)).rgb;

                    float2 edgeDist = min(hitUv, 1.0f - hitUv);
                    float edgeFade = saturate(min(edgeDist.x, edgeDist.y) * 9.0f);
                    float distFade = 1.0f - saturate(hitKt * maxT * 0.75f);
                    currentSsr = float4(hitCol, edgeFade * lerp(0.50f, 1.0f, distFade));
                }
            }
        }
    }

    float solidTrust = 1.0f - flickerMask;
    currentGi *= solidTrust;
    currentAo = lerp(1.0f, currentAo, solidTrust);
    currentSsr.a *= solidTrust;

    float4 giResult = float4(currentGi, currentAo);
    float4 ssrResult = currentSsr;

    if (validHistory)
    {
        float baseAlpha = lerp(0.14f, 0.45f, saturate(mvMag / 16.0f));

        float4 prevGi = PrevGiTex.SampleLevel(LinearClampGi, prevUv, 0.0f);
        float aoDiff = abs(prevGi.a - currentAo);
        float3 giDiff = abs(prevGi.rgb - currentGi);
        float giMaxDiff = max(giDiff.r, max(giDiff.g, giDiff.b));
        float giAlpha = max(baseAlpha, min(0.65f, max(saturate(aoDiff * 1.15f), saturate(giMaxDiff * 0.50f))));
        giResult = lerp(prevGi, giResult, saturate(giAlpha));

        if (SsrEnabled > 0)
        {
            float4 prevSsr = PrevSsrTex.SampleLevel(LinearClampSsr, prevUv, 0.0f);
            float3 ssrDiff = abs(prevSsr.rgb - currentSsr.rgb);
            float ssrMaxDiff = max(ssrDiff.r, max(ssrDiff.g, ssrDiff.b));
            float ssrAlpha = max(lerp(0.18f, 0.55f, saturate(mvMag / 12.0f)), min(0.70f, saturate(ssrMaxDiff * 0.45f)));
            ssrResult = lerp(prevSsr, currentSsr, saturate(ssrAlpha));
        }
    }

    OutGiTex[giCoord] = float4(max(giResult.rgb, 0.0f), saturate(giResult.a));
    OutSsrTex[giCoord] = float4(max(ssrResult.rgb, 0.0f), saturate(ssrResult.a));
}
