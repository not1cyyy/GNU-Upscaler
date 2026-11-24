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

    int DepthWidth;
    int DepthHeight;
    int MotionWidth;
    int MotionHeight;

    int DepthIsLinear;
    int DepthIsReversed;
    int DisplaySizeMV;
    int ColorSpaceMode;

    float DepthLinearA;
    float DepthLinearB;
    float DepthLinearC;
    float SkyDepthThreshold;

    float MvScaleX;
    float MvScaleY;
    float MotionTextureScale;
    float DepthTextureScale;

    float CameraVFov;
    float AspectRatio;
    float GiRadius;
    float GiBias;

    int RayCount;
    int StepCount;
    int FrameIndex;
    int ResetHistory;

    int HasGBufferNormals;
    int InColorEncodeMode;
    int SsrEnabled;
    int SsrSteps;
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
Texture2D<float4> PrevGiTex : register(t3);

#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
Texture2D<float4> GBufferNormalsTex : register(t4);

#ifdef VK_MODE
[[vk::binding(6, 0)]]
#endif
Texture2D<float4> PrevSsrTex : register(t5);

#ifdef VK_MODE
[[vk::binding(7, 0)]]
#endif
RWTexture2D<float4> OutGiTex : register(u0);

#ifdef VK_MODE
[[vk::binding(8, 0)]]
#endif
RWTexture2D<float4> OutSsrTex : register(u1);

static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);
static const float kPi = 3.14159265f;

float Luma(float3 c)
{
    return dot(c, kLumaCoeff);
}

float3 SrgbToLinear(float3 color)
{
    float3 c = saturate(color);
    float3 low = c / 12.92f;
    float3 high = pow(max((c + 0.055f) / 1.055f, 0.04045f), 2.4f);
    return lerp(high, low, step(c, 0.04045f));
}

float3 PQToLinear(float3 pq)
{
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;

    float3 p = saturate(pq);
    float3 pm2 = step(1e-12f, p) * pow(max(p, 1e-12f), 1.0f / m2);
    float3 ratio = max(pm2 - c1, 0.0f) / max(c2 - c3 * pm2, 1e-6f);
    float3 y = step(1e-12f, ratio) * pow(max(ratio, 1e-12f), 1.0f / m1);
    return y * (10000.0f / 80.0f);
}

float3 ToLinearColor(float3 color)
{
    if (InColorEncodeMode == 3)
        return SrgbToLinear(color);
    if (InColorEncodeMode == 4)
        return PQToLinear(color);
    return max(color, 0.0f);
}

int2 ClampOutputCoord(int2 p)
{
    return int2(clamp(p.x, 0, OutputWidth - 1), clamp(p.y, 0, OutputHeight - 1));
}

int2 ClampDepthCoord(int2 p)
{
    return int2(clamp(p.x, 0, DepthWidth - 1), clamp(p.y, 0, DepthHeight - 1));
}

int2 ClampMotionCoord(int2 p)
{
    return int2(clamp(p.x, 0, MotionWidth - 1), clamp(p.y, 0, MotionHeight - 1));
}

int2 ClampGiCoord(int2 p)
{
    return int2(clamp(p.x, 0, GiWidth - 1), clamp(p.y, 0, GiHeight - 1));
}

float LoadRawDepthAtUv(float2 uv)
{
    int2 dCoord = ClampDepthCoord(int2(uv * float2(DepthWidth, DepthHeight)));
    return DepthTex.Load(int3(dCoord, 0)).r;
}

float LinearizeDepth(float rawDepth)
{
    if (DepthIsLinear > 0)
    {
        float z = (DepthIsReversed > 0) ? (1.0f - rawDepth) : rawDepth;
        return (z <= 1.0001f) ? max(z, 0.0f) * max(DepthLinearB, 100.0f) : max(z, 0.0f);
    }

    if (DepthIsReversed > 0)
    {
        float revOffset = DepthLinearB - DepthLinearC;
        return DepthLinearA / max(revOffset + rawDepth * DepthLinearC, 1e-5f);
    }

    return DepthLinearA / max(DepthLinearB - rawDepth * DepthLinearC, 1e-5f);
}

float3 ReconstructViewPos(float2 uv, float linearDepth)
{
    float tanHalfFov = tan(clamp(CameraVFov, 0.25f, 2.4f) * 0.5f);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    return float3(ndc.x * tanHalfFov * AspectRatio * linearDepth, ndc.y * tanHalfFov * linearDepth, linearDepth);
}

float2 ProjectViewPosToUv(float3 p)
{
    float tanHalfFov = tan(clamp(CameraVFov, 0.25f, 2.4f) * 0.5f);
    float invZ = 1.0f / max(p.z, 1e-4f);
    float ndcX = p.x * invZ / (tanHalfFov * AspectRatio);
    float ndcY = p.y * invZ / tanHalfFov;
    return float2(ndcX * 0.5f + 0.5f, 0.5f - ndcY * 0.5f);
}

// Always step in exact depth-texel coordinates so upscaled render-res depth produces crisp 3D normals
float3 ReconstructViewNormalFromDepthTexel(float2 uv)
{
    int2 dCoord = ClampDepthCoord(int2(uv * float2(DepthWidth, DepthHeight)));
    float2 dUv = (float2(dCoord) + 0.5f) / float2(DepthWidth, DepthHeight);
    float2 invDepth = 1.0f / float2(DepthWidth, DepthHeight);

    float dC = LinearizeDepth(DepthTex.Load(int3(dCoord, 0)).r);
    float dL = LinearizeDepth(DepthTex.Load(int3(ClampDepthCoord(dCoord + int2(-1, 0)), 0)).r);
    float dR = LinearizeDepth(DepthTex.Load(int3(ClampDepthCoord(dCoord + int2(1, 0)), 0)).r);
    float dU = LinearizeDepth(DepthTex.Load(int3(ClampDepthCoord(dCoord + int2(0, -1)), 0)).r);
    float dD = LinearizeDepth(DepthTex.Load(int3(ClampDepthCoord(dCoord + int2(0, 1)), 0)).r);

    float3 pC = ReconstructViewPos(dUv, dC);
    float3 pL = ReconstructViewPos(dUv + float2(-invDepth.x, 0.0f), dL);
    float3 pR = ReconstructViewPos(dUv + float2(invDepth.x, 0.0f), dR);
    float3 pU = ReconstructViewPos(dUv + float2(0.0f, -invDepth.y), dU);
    float3 pD = ReconstructViewPos(dUv + float2(0.0f, invDepth.y), dD);

    float3 dx = (abs(dR - dC) < abs(dC - dL)) ? (pR - pC) : (pC - pL);
    float3 dy = (abs(dD - dC) < abs(dC - dU)) ? (pC - pD) : (pU - pC);

    float3 n = cross(dx, dy);
    float lenSq = dot(n, n);
    if (lenSq < 1e-12f)
        return float3(0.0f, 0.0f, -1.0f);

    n *= rsqrt(lenSq);
    if (n.z > 0.0f)
        n = -n;
    return n;
}

float InterleavedGradientNoise(float2 p)
{
    float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(p, magic.xy)));
}

float4 SamplePrevBilinear(Texture2D<float4> tex, float2 prevUv)
{
    float2 fCoord = prevUv * float2(GiWidth, GiHeight) - 0.5f;
    int2 iCoord = int2(floor(fCoord));
    float2 f = frac(fCoord);

    float4 c00 = tex.Load(int3(ClampGiCoord(iCoord + int2(0, 0)), 0));
    float4 c10 = tex.Load(int3(ClampGiCoord(iCoord + int2(1, 0)), 0));
    float4 c01 = tex.Load(int3(ClampGiCoord(iCoord + int2(0, 1)), 0));
    float4 c11 = tex.Load(int3(ClampGiCoord(iCoord + int2(1, 1)), 0));

    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if ((int) dtid.x >= GiWidth || (int) dtid.y >= GiHeight)
        return;

    int2 giCoord = int2(dtid.xy);
    float2 uv = (float2(giCoord) + 0.5f) / float2(GiWidth, GiHeight);
    int2 outCoord = ClampOutputCoord(int2(uv * float2(OutputWidth, OutputHeight)));

    float rawDepth = LoadRawDepthAtUv(uv);
    float centerDepth = LinearizeDepth(rawDepth);

    bool isSky = (centerDepth >= SkyDepthThreshold) || (DepthIsReversed > 0 && rawDepth <= 1e-6f) ||
                 (DepthIsReversed == 0 && rawDepth >= 0.99999f);
    if (isSky)
    {
        OutGiTex[giCoord] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        OutSsrTex[giCoord] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 centerPos = ReconstructViewPos(uv, centerDepth);
    float3 viewNormal = ReconstructViewNormalFromDepthTexel(uv);

    if (HasGBufferNormals > 0)
    {
        float3 gN = GBufferNormalsTex.Load(int3(outCoord, 0)).xyz;
        if (all(gN >= 0.0f) && all(gN <= 1.0f))
            gN = gN * 2.0f - 1.0f;
        float gLen = dot(gN, gN);
        if (gLen > 0.1f && gLen < 4.0f)
        {
            float3 gNorm = gN * rsqrt(gLen);
            if (dot(gNorm, viewNormal) > 0.15f)
                viewNormal = normalize(lerp(viewNormal, gNorm, 0.45f));
        }
    }

    float noiseA = InterleavedGradientNoise(
        float2(giCoord) + float2((FrameIndex & 15) * 5.588238f, (FrameIndex & 15) * 3.1732f));
    float noiseB = frac(noiseA * 1.6180339f + 0.371f);

    float maxRadiusPx = max(GiRadius, 24.0f);
    float2 invOutSize = 1.0f / float2(OutputWidth, OutputHeight);

    float3 accumGi = float3(0.0f, 0.0f, 0.0f);
    float accumAo = 0.0f;

    int rays = clamp(RayCount, 1, 8);
    int steps = clamp(StepCount, 2, 8);
    float worldRange = clamp(centerDepth * 0.32f, 1.5f, 26.0f);
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

            float sampleRawDepth = LoadRawDepthAtUv(sampleUv);
            float sampleDepth = LinearizeDepth(sampleRawDepth);

            if (sampleDepth >= SkyDepthThreshold)
                continue;

            float3 samplePos = ReconstructViewPos(sampleUv, sampleDepth);
            float3 delta = samplePos - centerPos;
            float dist = length(delta);

            if (dist < 1e-3f || dist > worldRange)
                continue;

            float3 dir3D = delta / dist;
            float nDotD = dot(viewNormal, dir3D);

            // Thickness rejection
            float depthDelta = centerDepth - sampleDepth;
            if (depthDelta > thicknessLimit)
                continue;

            float rangeFade = 1.0f - saturate(dist / worldRange);
            float elev = nDotD - GiBias;

            // Strong close-range contact shadow term for character/object grounding
            if (s <= 2 && elev > 0.02f)
            {
                float contactRange = max(centerDepth * 0.075f, 0.45f);
                microContactOcc = max(microContactOcc, saturate(elev * 1.65f) * (1.0f - saturate(dist / contactRange)));
            }

            if (elev > maxHorizonSin)
            {
                float aoDelta = saturate(elev) * rangeFade;
                maxHorizonSin = max(maxHorizonSin, elev);

                int2 sampleOutCoord = ClampOutputCoord(int2(sampleUv * float2(OutputWidth, OutputHeight)));
                float3 hitColor = ToLinearColor(SourceColor.Load(int3(sampleOutCoord, 0)).rgb);
                float hitLuma = Luma(hitColor);
                float fireflyScale = min(hitLuma, 5.0f) / max(hitLuma, 1e-4f);
                hitColor *= fireflyScale;

                // Boost chromatic saturation of indirect bounce slightly so colored bounce stands out
                hitColor = max(lerp(hitLuma.xxx, hitColor, 1.35f), 0.0f);

                float bounceCos = saturate(nDotD);
                rayGi += hitColor * bounceCos * rangeFade * aoDelta;
                rayWeightSum += rangeFade;
            }
        }

        float rayAo = saturate(1.0f - max(maxHorizonSin * 1.05f, microContactOcc * 0.92f));
        accumAo += rayAo;
        if (rayWeightSum > 1e-4f)
            accumGi += rayGi / max(rayWeightSum, 0.70f);
    }

    float3 currentGi = max(accumGi / float(rays), 0.0f);
    float currentAo = saturate(accumAo / float(rays));

    // Real-time Half-Res Screen-Space Reflections (SSR) using true 3D viewNormal
    float4 currentSsr = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (SsrEnabled > 0)
    {
        float3 viewDir = normalize(centerPos);
        float3 reflDir = normalize(reflect(viewDir, viewNormal));

        if (reflDir.z > -0.45f)
        {
            int ssrSteps = clamp(SsrSteps, 4, 12);
            float ssrMaxDist = clamp(centerDepth * 1.15f, 3.0f, 55.0f);
            float ssrThickness = clamp(centerDepth * 0.085f, 0.16f, 3.8f);

            float prevDist = 0.0f;
            float hitDist = 0.0f;
            float2 hitUv = uv;
            bool hit = false;

            [loop]
            for (int k = 1; k <= ssrSteps; ++k)
            {
                float kt = (float(k) + noiseB * 0.40f) / float(ssrSteps);
                float marchDist = lerp(max(centerDepth * 0.015f, 0.06f), ssrMaxDist, kt * kt);
                float3 rayPos = centerPos + reflDir * marchDist;

                if (rayPos.z <= 0.05f || rayPos.z >= SkyDepthThreshold)
                    break;

                float2 rayUv = ProjectViewPosToUv(rayPos);
                if (any(rayUv < 0.004f) || any(rayUv > 0.996f))
                    break;

                float rayDepth = LinearizeDepth(LoadRawDepthAtUv(rayUv));
                float diff = rayPos.z - rayDepth;
                if (diff > 0.0f && diff < ssrThickness)
                {
                    hit = true;
                    hitDist = marchDist;
                    hitUv = rayUv;
                    break;
                }

                prevDist = marchDist;
            }

            if (hit)
            {
                // 2 binary midpoint refinement steps for crisp reflection edges
                float lo = prevDist;
                float hi = hitDist;
                [unroll]
                for (int b = 0; b < 2; ++b)
                {
                    float mid = 0.5f * (lo + hi);
                    float3 midPos = centerPos + reflDir * mid;
                    float2 midUv = ProjectViewPosToUv(midPos);
                    if (midPos.z > 0.05f && all(midUv >= 0.002f) && all(midUv <= 0.998f))
                    {
                        float midDepth = LinearizeDepth(LoadRawDepthAtUv(midUv));
                        float midDiff = midPos.z - midDepth;
                        if (midDiff > 0.0f && midDiff < ssrThickness)
                        {
                            hi = mid;
                            hitUv = midUv;
                        }
                        else
                        {
                            lo = mid;
                        }
                    }
                }

                int2 hitCoord = ClampOutputCoord(int2(hitUv * float2(OutputWidth, OutputHeight)));
                float3 hitCol = ToLinearColor(SourceColor.Load(int3(hitCoord, 0)).rgb);
                float hitLuma = Luma(hitCol);
                hitCol *= min(hitLuma, 6.0f) / max(hitLuma, 1e-4f);

                float2 edgeDist = min(hitUv, 1.0f - hitUv);
                float edgeFade = saturate(min(edgeDist.x, edgeDist.y) * 14.0f);
                float distFade = 1.0f - saturate(hi / ssrMaxDist);
                float conf = edgeFade * lerp(0.50f, 1.0f, distFade);

                currentSsr = float4(hitCol, conf);
            }
        }
    }

    // Temporal reprojection using game motion vectors
    int2 mvCoord = DisplaySizeMV > 0 ? outCoord : int2((float2(outCoord) + 0.5f) * MotionTextureScale);
    float2 mv = MotionTex.Load(int3(ClampMotionCoord(mvCoord), 0)).rg * float2(MvScaleX, MvScaleY);
    float2 prevUv = uv + mv * invOutSize;

    float4 giResult = float4(currentGi, currentAo);
    float4 ssrResult = currentSsr;

    if (ResetHistory == 0 && FrameIndex > 2 && all(prevUv >= 0.002f) && all(prevUv <= 0.998f))
    {
        float mvMag = length(mv);
        float baseAlpha = lerp(0.14f, 0.52f, saturate(mvMag / 16.0f));

        float4 prevGi = SamplePrevBilinear(PrevGiTex, prevUv);
        float aoDiff = abs(prevGi.a - currentAo);
        float3 giDiff = abs(prevGi.rgb - currentGi);
        float giMaxDiff = max(giDiff.r, max(giDiff.g, giDiff.b));
        float giAlpha = max(baseAlpha, max(saturate(aoDiff * 1.4f), saturate(giMaxDiff * 0.65f)));
        giResult = lerp(prevGi, giResult, saturate(giAlpha));

        if (SsrEnabled > 0)
        {
            float4 prevSsr = SamplePrevBilinear(PrevSsrTex, prevUv);
            float3 ssrDiff = abs(prevSsr.rgb - currentSsr.rgb);
            float ssrMaxDiff = max(ssrDiff.r, max(ssrDiff.g, ssrDiff.b));
            float ssrAlpha = max(lerp(0.18f, 0.65f, saturate(mvMag / 12.0f)), saturate(ssrMaxDiff * 0.55f));
            ssrResult = lerp(prevSsr, currentSsr, saturate(ssrAlpha));
        }
    }

    OutGiTex[giCoord] = float4(max(giResult.rgb, 0.0f), saturate(giResult.a));
    OutSsrTex[giCoord] = float4(max(ssrResult.rgb, 0.0f), saturate(ssrResult.a));
}
