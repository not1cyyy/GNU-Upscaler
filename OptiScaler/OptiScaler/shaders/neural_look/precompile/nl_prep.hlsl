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
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutHalfColorDepth : register(u0);

#ifdef VK_MODE
[[vk::binding(5, 0)]] [[vk::image_format("rg16f")]]
#endif
RWTexture2D<float2> OutHalfNormal : register(u1);

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
        OutHalfNormal[halfCoord] = float2(0.0f, 0.0f);
        return;
    }

    // Continuous collinear-triple spike detector across 3x3 depth neighborhood
    float spH     = SignedSpike(dC, dL,  dR);
    float spV     = SignedSpike(dC, dU,  dD);
    float spD1    = SignedSpike(dC, dUL, dDR);
    float spD2    = SignedSpike(dC, dUR, dDL);
    float spTop   = SignedSpike(dU, dUL, dUR);
    float spBot   = SignedSpike(dD, dDL, dDR);
    float spLeft  = SignedSpike(dL, dUL, dDL);
    float spRight = SignedSpike(dR, dUR, dDR);

    float maxSpike = max(max(max(spH, spV), max(spD1, spD2)), max(max(spTop, spBot), max(spLeft, spRight)));
    float relSpike = maxSpike / max(minD, 0.05f);
    float ditherMask = smoothstep(0.004f, 0.018f, relSpike);

    float safeDC = (dC >= 64000.0f && ditherMask > 0.15f) ? minD : dC;
    float outDepth = lerp(safeDC, minD, smoothstep(0.15f, 0.55f, ditherMask));
    OutHalfColorDepth[halfCoord] = float4(linColor, min(outDepth, 65000.0f));

    if (outDepth >= 64000.0f)
    {
        OutHalfNormal[halfCoord] = float2(0.0f, 0.0f);
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

    if (ditherMask > 0.0f)
        viewNormal = normalize(lerp(viewNormal, float3(0.0f, 0.0f, -1.0f), ditherMask * 0.85f));

    // Losslessly pack 4-bit quantized ditherMask into OutHalfNormal.x (read only via .Load())
    float dQ = round(saturate(ditherMask) * 15.0f);
    OutHalfNormal[halfCoord] = float2(viewNormal.x + dQ * 4.0f, viewNormal.y);
}
