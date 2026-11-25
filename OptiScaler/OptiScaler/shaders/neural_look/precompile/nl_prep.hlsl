#ifdef VK_MODE
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    int OutputWidth;
    int OutputHeight;
    int HalfWidth;
    int HalfHeight;

    int DepthWidth;
    int DepthHeight;
    int DepthIsLinear;
    int DepthIsReversed;

    float DepthLinearA;
    float DepthLinearB;
    float DepthLinearC;
    float SkyDepthThreshold;

    float TanHalfFov;
    float TanHalfFovAspect;
    int HasGBufferNormals;
    int InColorEncodeMode;

    int MotionWidth;
    int MotionHeight;
    int DisplaySizeMV;
    int ResetHistory;

    float MvScaleX;
    float MvScaleY;
    float MotionTextureScale;
    int FrameIndex;
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
Texture2D<float4> GBufferNormalsTex : register(t2);

#ifdef VK_MODE
[[vk::binding(4, 0)]]
#endif
Texture2D<float2> MotionTex : register(t3);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]]
#endif
Texture2D<float4> PrevHalfNormalTex : register(t4);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]]
SamplerState LinearClamp : register(s0);
#else
SamplerState LinearClamp : register(s0);
#endif

#ifdef VK_MODE
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutHalfColorDepth : register(u0);

#ifdef VK_MODE
[[vk::binding(7, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutHalfNormal : register(u1);

static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);

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

int2 ClampHalfCoord(int2 p)
{
    return int2(clamp(p.x, 0, HalfWidth - 1), clamp(p.y, 0, HalfHeight - 1));
}

int2 ClampMotionCoord(int2 p)
{
    return int2(clamp(p.x, 0, max(MotionWidth - 1, 0)), clamp(p.y, 0, max(MotionHeight - 1, 0)));
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
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    return float3(ndc.x * TanHalfFovAspect * linearDepth, ndc.y * TanHalfFov * linearDepth, linearDepth);
}

float LoadLinearDepth(int2 coord)
{
    float rawDepth = DepthTex.Load(int3(ClampDepthCoord(coord), 0)).r;
    bool rawSky = (DepthIsReversed > 0 && rawDepth <= 1e-6f) || (DepthIsReversed == 0 && rawDepth >= 0.99999f);
    float linDepth = rawSky ? 65000.0f : LinearizeDepth(rawDepth);
    return (linDepth >= SkyDepthThreshold) ? 65000.0f : linDepth;
}

float3 ReconstructViewNormalFromDepths(int2 dCoord, float dC, float dL, float dR, float dU, float dD)
{
    float2 dUv = (float2(dCoord) + 0.5f) / float2(DepthWidth, DepthHeight);
    float2 invDepth = 1.0f / float2(DepthWidth, DepthHeight);

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

// Mathematically 0.0 on any flat plane at any angle and on any clean straight silhouette edge;
// fires continuously whenever midpoint c sticks out from BOTH collinear neighbors a and b.
float SignedSpike(float c, float a, float b)
{
    float da = a - c;
    float db = b - c;
    return (da * db > 0.0f) ? min(abs(da), abs(db)) : 0.0f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if ((int) dtid.x >= HalfWidth || (int) dtid.y >= HalfHeight)
        return;

    int2 halfCoord = int2(dtid.xy);
    float2 uv = (float2(halfCoord) + 0.5f) / float2(HalfWidth, HalfHeight);
    int2 outCoord = ClampOutputCoord(int2(uv * float2(OutputWidth, OutputHeight)));

    int2 baseOut = halfCoord * 2;
    float3 c00 = ToLinearColor(SourceColor.Load(int3(ClampOutputCoord(baseOut), 0)).rgb);
    float3 c10 = ToLinearColor(SourceColor.Load(int3(ClampOutputCoord(baseOut + int2(1, 0)), 0)).rgb);
    float3 c01 = ToLinearColor(SourceColor.Load(int3(ClampOutputCoord(baseOut + int2(0, 1)), 0)).rgb);
    float3 c11 = ToLinearColor(SourceColor.Load(int3(ClampOutputCoord(baseOut + int2(1, 1)), 0)).rgb);
    float3 linColor = (c00 + c10 + c01 + c11) * 0.25f;
    float luma = Luma(linColor);
    linColor *= min(luma, 6.0f) / max(luma, 1e-4f);

    int2 dCoord = ClampDepthCoord(int2(uv * float2(DepthWidth, DepthHeight)));
    float dC  = LoadLinearDepth(dCoord);
    float dL  = LoadLinearDepth(dCoord + int2(-1, 0));
    float dR  = LoadLinearDepth(dCoord + int2(1, 0));
    float dU  = LoadLinearDepth(dCoord + int2(0, -1));
    float dD  = LoadLinearDepth(dCoord + int2(0, 1));
    float dUL = LoadLinearDepth(dCoord + int2(-1, -1));
    float dUR = LoadLinearDepth(dCoord + int2(1, -1));
    float dDL = LoadLinearDepth(dCoord + int2(-1, 1));
    float dDR = LoadLinearDepth(dCoord + int2(1, 1));

    float minD = min(dC, min(min(min(dL, dR), min(dU, dD)), min(min(dUL, dUR), min(dDL, dDR))));
    float maxD = max(dC, max(max(max(dL, dR), max(dU, dD)), max(max(dUL, dUR), max(dDL, dDR))));

    if (minD >= 64000.0f)
    {
        OutHalfColorDepth[halfCoord] = float4(linColor, 65000.0f);
        OutHalfNormal[halfCoord] = float4(0.0f, 0.0f, 0.0f, 65000.0f);
        return;
    }

    // Strict 2D collinear-triple stipple seed (0.0 on any 1D pipe/beam/floor/straight silhouette)
    float spH     = SignedSpike(dC, dL,  dR);
    float spV     = SignedSpike(dC, dU,  dD);
    float spD1    = SignedSpike(dC, dUL, dDR);
    float spD2    = SignedSpike(dC, dUR, dDL);
    float spTop   = SignedSpike(dU, dUL, dUR);
    float spBot   = SignedSpike(dD, dDL, dDR);
    float spLeft  = SignedSpike(dL, dUL, dDL);
    float spRight = SignedSpike(dR, dUR, dDR);

    float orthoSpike = max(min(spH, spV), min(spD1, spD2));
    float ringSpike = min(min(spTop, spBot), min(spLeft, spRight));
    float relSpike = max(orthoSpike, ringSpike) / max(minD, 0.05f);
    float spatialDither = smoothstep(0.005f, 0.020f, relSpike);

    // Temporal depth-flip & normal stabilization via reprojected RGBA16F G-Proxy history
    int2 mvCoord = DisplaySizeMV > 0 ? outCoord : int2((float2(outCoord) + 0.5f) * MotionTextureScale);
    float2 mv = MotionTex.Load(int3(ClampMotionCoord(mvCoord), 0)).rg * float2(MvScaleX, MvScaleY);
    float mvMag = length(mv);
    float2 prevUv = uv + mv / float2(OutputWidth, OutputHeight);
    bool validHistory = (ResetHistory == 0 && FrameIndex > 2 && all(prevUv >= 0.002f) && all(prevUv <= 0.998f));

    float4 prevSample =
        validHistory ? PrevHalfNormalTex.SampleLevel(LinearClamp, prevUv, 0.0f) : float4(0.0f, 0.0f, 0.0f, dC);
    float prevRawD =
        validHistory
            ? PrevHalfNormalTex.Load(int3(ClampHalfCoord(int2(prevUv * float2(HalfWidth, HalfHeight))), 0)).w
            : dC;

    float depthDiff = min(abs(dC - prevRawD), abs(dC - prevSample.w));
    float relTempDiff = depthDiff / max(min(dC, min(prevRawD, prevSample.w)), 0.05f);
    float motionTrust = rcp(1.0f + mvMag * 0.20f);
    float temporalFlip = validHistory ? (smoothstep(0.005f, 0.024f, relTempDiff) * motionTrust) : 0.0f;

    float instantFlicker = max(spatialDither, temporalFlip);
    float flickerEma = instantFlicker;
    if (validHistory)
    {
        float prevEma = saturate(prevSample.z);
        float decayAlpha = lerp(0.08f, 0.25f, saturate(mvMag / 8.0f));
        float alpha = (instantFlicker > prevEma) ? 0.50f : decayAlpha;
        flickerEma = lerp(prevEma, instantFlicker, alpha);
    }

    float safeDC = (dC >= 64000.0f && flickerEma > 0.15f) ? minD : dC;
    float outDepth = lerp(safeDC, minD, smoothstep(0.15f, 0.55f, flickerEma));
    OutHalfColorDepth[halfCoord] = float4(linColor, min(outDepth, 65000.0f));

    if (outDepth >= 64000.0f)
    {
        OutHalfNormal[halfCoord] = float4(0.0f, 0.0f, saturate(flickerEma), min(dC, 65000.0f));
        return;
    }

    float3 viewNormal = ReconstructViewNormalFromDepths(dCoord, dC, dL, dR, dU, dD);
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

    if (validHistory && relTempDiff < 0.025f && flickerEma < 0.35f)
    {
        float2 pnxy = clamp(prevSample.xy, -1.0f, 1.0f);
        float3 prevNormal = float3(pnxy, -sqrt(max(0.0f, 1.0f - dot(pnxy, pnxy))));
        if (dot(prevNormal, viewNormal) > 0.35f)
        {
            float normAlpha = lerp(0.25f, 0.75f, saturate(mvMag / 8.0f));
            viewNormal = normalize(lerp(prevNormal, viewNormal, normAlpha));
        }
    }

    if (flickerEma > 0.0f)
        viewNormal = normalize(lerp(viewNormal, float3(0.0f, 0.0f, -1.0f), saturate(flickerEma) * 0.80f));

    OutHalfNormal[halfCoord] = float4(viewNormal.xy, saturate(flickerEma), min(dC, 65000.0f));
}
