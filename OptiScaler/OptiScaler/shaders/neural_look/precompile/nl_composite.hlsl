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

    float GiIntensity;
    float AoIntensity;
    float NeuralDetail;
    float SpecularRelief;

    float ToneContrast;
    int InColorEncodeMode;

    int GiEnabled;
    int HasGBufferNormals;
    int HasGBufferAlbedo;
    int HasGBufferRoughness;

    int DebugView;
    int OutColorEncodeMode;
    float SsrIntensity;
    float RelightIntensity;

    float VolumetricGlow;
    float SplitScreenPos;
    int SsrEnabled;
    int _pad0;
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
Texture2D<float4> GiTex : register(t3);

#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
Texture2D<float4> GBufferNormalsTex : register(t4);

#ifdef VK_MODE
[[vk::binding(6, 0)]]
#endif
Texture2D<float4> GBufferAlbedoTex : register(t5);

#ifdef VK_MODE
[[vk::binding(7, 0)]]
#endif
Texture2D<float4> GBufferRoughnessTex : register(t6);

#ifdef VK_MODE
[[vk::binding(8, 0)]]
#endif
Texture2D<float4> SsrTex : register(t7);

#ifdef VK_MODE
[[vk::binding(9, 0)]]
#endif
RWTexture2D<float4> DestColor : register(u0);

static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);

groupshared float3 s_color[18][18];
groupshared float s_depth[18][18];

float Luma(float3 c)
{
    return dot(c, kLumaCoeff);
}

float3 LinearToSrgb(float3 color)
{
    float3 c = saturate(color);
    float3 low = c * 12.92f;
    float3 high = 1.055f * pow(max(c, 0.0031308f), 1.0f / 2.4f) - 0.055f;
    return lerp(high, low, step(c, 0.0031308f));
}

float3 SrgbToLinear(float3 color)
{
    float3 c = saturate(color);
    float3 low = c / 12.92f;
    float3 high = pow(max((c + 0.055f) / 1.055f, 0.04045f), 2.4f);
    return lerp(high, low, step(c, 0.04045f));
}

float3 LinearToPQ(float3 color)
{
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;

    float3 y = saturate(max(color, 0.0f) * (80.0f / 10000.0f));
    float3 ym1 = step(1e-12f, y) * pow(max(y, 1e-12f), m1);
    return pow(max((c1 + c2 * ym1) / (1.0f + c3 * ym1), 1e-12f), m2);
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

float3 EncodeColor(float3 color, int mode)
{
    if (mode == 1)
        return LinearToSrgb(color);
    if (mode == 2)
        return LinearToPQ(color);
    if (mode == 3)
        return SrgbToLinear(color);
    if (mode == 4)
        return PQToLinear(color);
    if (mode == 5)
        return LinearToPQ(SrgbToLinear(color));
    if (mode == 6)
        return LinearToSrgb(PQToLinear(color));
    return color;
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

float LoadDepthAtOutputCoord(int2 outCoord)
{
    int2 dCoord = int2((float2(ClampOutputCoord(outCoord)) + 0.5f) * DepthTextureScale);
    return LinearizeDepth(DepthTex.Load(int3(ClampDepthCoord(dCoord), 0)).r);
}

float4 BilateralSampleHalfRes(Texture2D<float4> halfTex, int2 outCoord, float centerDepth)
{
    float2 giF = (float2(outCoord) + 0.5f) * (float2(GiWidth, GiHeight) / float2(OutputWidth, OutputHeight)) - 0.5f;
    int2 giBase = int2(floor(giF));
    float2 fracUv = frac(giF);

    float invSigma = 1.0f / max(centerDepth * 0.05f, 0.02f);

    float4 accum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float wSum = 0.0f;

    [unroll]
    for (int dy = 0; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = 0; dx <= 1; ++dx)
        {
            int2 tapGi = ClampGiCoord(giBase + int2(dx, dy));
            float4 tap = halfTex.Load(int3(tapGi, 0));

            int2 tapOut = ClampOutputCoord(int2((float2(tapGi) + 0.5f) * (float2(OutputWidth, OutputHeight) / float2(GiWidth, GiHeight))));
            float tapDepth = LoadDepthAtOutputCoord(tapOut);

            float wx = (dx == 0) ? (1.0f - fracUv.x) : fracUv.x;
            float wy = (dy == 0) ? (1.0f - fracUv.y) : fracUv.y;
            float spatialW = wx * wy;
            float depthW = exp(-abs(centerDepth - tapDepth) * invSigma);
            float w = spatialW * depthW + 1e-5f;

            accum += tap * w;
            wSum += w;
        }
    }

    return accum / max(wSum, 1e-5f);
}

float3 SampleVolumetricGlow(int2 outCoord)
{
    int2 giCenter = int2((float2(outCoord) + 0.5f) * (float2(GiWidth, GiHeight) / float2(OutputWidth, OutputHeight)));
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    static const int2 offsets[4] = { int2(-4, 0), int2(4, 0), int2(0, -4), int2(0, 4) };
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float3 g = GiTex.Load(int3(ClampGiCoord(giCenter + offsets[i]), 0)).rgb;
        float3 s = SsrTex.Load(int3(ClampGiCoord(giCenter + offsets[i]), 0)).rgb;
        sum += g * 0.65f + s * 0.35f;
    }
    return sum * 0.25f;
}

float SiLU(float x)
{
    return x / (1.0f + exp(-clamp(x, -12.0f, 12.0f)));
}

float3 EvalNeuralResidualMLP(float f0, float f1, float f2, float f3, float f4, float f5)
{
    static const float W1[8][6] = {
        {  0.22f,  1.45f, -0.55f, -0.20f,  0.30f,  0.15f },
        { -0.18f, -1.25f,  0.60f,  0.15f, -0.25f,  0.20f },
        {  0.35f,  0.25f,  0.85f, -0.35f,  0.40f, -0.30f },
        { -0.42f,  0.40f,  0.50f,  0.10f,  0.55f,  0.45f },
        {  0.28f, -0.35f,  0.70f, -0.45f,  0.20f,  0.50f },
        {  0.15f,  0.90f, -0.40f, -0.25f,  0.65f, -0.15f },
        { -0.30f,  0.15f,  0.65f,  0.30f, -0.40f,  0.60f },
        {  0.48f, -0.20f, -0.30f, -0.15f,  0.35f,  0.35f }
    };
    static const float B1[8] = { 0.02f, -0.02f, 0.05f, -0.04f, 0.01f, 0.03f, -0.03f, 0.02f };

    static const float W2[8][8] = {
        {  0.55f, -0.40f,  0.25f, -0.15f,  0.10f,  0.45f, -0.20f,  0.15f },
        { -0.35f,  0.50f,  0.15f,  0.30f, -0.20f, -0.30f,  0.25f,  0.10f },
        {  0.20f, -0.15f,  0.45f,  0.40f,  0.35f,  0.15f, -0.30f,  0.25f },
        { -0.15f,  0.25f, -0.30f,  0.55f,  0.45f,  0.20f,  0.35f, -0.25f },
        {  0.30f,  0.10f,  0.20f,  0.25f,  0.50f,  0.35f,  0.15f,  0.40f },
        {  0.40f, -0.35f,  0.30f, -0.10f,  0.25f,  0.55f, -0.15f,  0.20f },
        { -0.20f,  0.30f, -0.25f,  0.35f,  0.20f, -0.15f,  0.50f,  0.30f },
        {  0.15f,  0.20f,  0.40f,  0.15f,  0.30f,  0.25f,  0.20f,  0.45f }
    };
    static const float B2[8] = { 0.01f, -0.01f, 0.02f, 0.0f, 0.01f, 0.02f, -0.02f, 0.01f };

    static const float W3[3][8] = {
        {  0.65f, -0.58f,  0.18f, -0.12f,  0.10f,  0.48f, -0.15f,  0.12f },
        { -0.08f,  0.10f,  0.32f,  0.42f,  0.36f, -0.06f,  0.28f,  0.24f },
        {  0.18f, -0.14f,  0.22f,  0.20f,  0.28f,  0.26f,  0.12f,  0.30f }
    };
    static const float B3[3] = { 0.0f, -0.02f, -0.01f };

    float h1[8];
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float s = B1[i] + W1[i][0] * f0 + W1[i][1] * f1 + W1[i][2] * f2 + W1[i][3] * f3 + W1[i][4] * f4 + W1[i][5] * f5;
        h1[i] = SiLU(s);
    }

    float h2[8];
    [unroll]
    for (int j = 0; j < 8; ++j)
    {
        float s = B2[j];
        [unroll]
        for (int k = 0; k < 8; ++k)
            s += W2[j][k] * h1[k];
        h2[j] = SiLU(s);
    }

    float3 outV = float3(B3[0], B3[1], B3[2]);
    [unroll]
    for (int m = 0; m < 8; ++m)
    {
        outV.x += W3[0][m] * h2[m];
        outV.y += W3[1][m] * h2[m];
        outV.z += W3[2][m] * h2[m];
    }

    return clamp(outV, -0.42f, 0.42f);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    int2 groupBase = int2(gid.xy) * 16 - 1;
    uint linearIdx = gtid.y * 16 + gtid.x;

    [unroll]
    for (uint passIdx = 0; passIdx < 2; ++passIdx)
    {
        uint idx = linearIdx + passIdx * 256;
        if (idx < 324)
        {
            uint sx = idx % 18;
            uint sy = idx / 18;
            int2 p = ClampOutputCoord(groupBase + int2(sx, sy));
            s_color[sy][sx] = EncodeColor(SourceColor.Load(int3(p, 0)).rgb, InColorEncodeMode);
            s_depth[sy][sx] = LoadDepthAtOutputCoord(p);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if ((int) dtid.x >= OutputWidth || (int) dtid.y >= OutputHeight)
        return;

    int2 outCoord = int2(dtid.xy);
    int2 ldsCoord = int2(gtid.xy) + 1;

    float4 origRaw = SourceColor.Load(int3(outCoord, 0));

    // Real-time Split-Screen comparison divider
    if (SplitScreenPos > 0.01f && SplitScreenPos < 0.99f)
    {
        int splitX = int(SplitScreenPos * float(OutputWidth));
        if (abs(outCoord.x - splitX) <= 1)
        {
            DestColor[outCoord] = float4(EncodeColor(float3(1.0f, 1.0f, 1.0f), OutColorEncodeMode), origRaw.a);
            return;
        }
        if (outCoord.x < splitX)
        {
            DestColor[outCoord] = origRaw;
            return;
        }
    }

    float3 centerColor = s_color[ldsCoord.y][ldsCoord.x];
    float centerDepth = s_depth[ldsCoord.y][ldsCoord.x];

    float skyMask = 1.0f - smoothstep(SkyDepthThreshold * 0.65f, SkyDepthThreshold, centerDepth);

    // 3x3 neighborhood stats from shared memory
    float3 cL = s_color[ldsCoord.y][ldsCoord.x - 1];
    float3 cR = s_color[ldsCoord.y][ldsCoord.x + 1];
    float3 cU = s_color[ldsCoord.y - 1][ldsCoord.x];
    float3 cD = s_color[ldsCoord.y + 1][ldsCoord.x];
    float3 cUL = s_color[ldsCoord.y - 1][ldsCoord.x - 1];
    float3 cUR = s_color[ldsCoord.y - 1][ldsCoord.x + 1];
    float3 cDL = s_color[ldsCoord.y + 1][ldsCoord.x - 1];
    float3 cDR = s_color[ldsCoord.y + 1][ldsCoord.x + 1];

    float dL = s_depth[ldsCoord.y][ldsCoord.x - 1];
    float dR = s_depth[ldsCoord.y][ldsCoord.x + 1];
    float dU = s_depth[ldsCoord.y - 1][ldsCoord.x];
    float dD = s_depth[ldsCoord.y + 1][ldsCoord.x];

    float gx = abs(dR - centerDepth) < abs(centerDepth - dL) ? (dR - centerDepth) : (centerDepth - dL);
    float gy = abs(dD - centerDepth) < abs(centerDepth - dU) ? (dD - centerDepth) : (centerDepth - dU);
    float maxDepthGrad = max(abs(dR - dL), abs(dD - dU));
    float relDepthJump = maxDepthGrad / max(centerDepth, 0.05f);
    float haloRisk = smoothstep(0.015f, 0.08f, relDepthJump);

    float3 geomNormal = normalize(float3(-gx / max(centerDepth * 0.01f, 1e-3f), gy / max(centerDepth * 0.01f, 1e-3f), -1.0f));
    if (HasGBufferNormals > 0)
    {
        float3 gN = GBufferNormalsTex.Load(int3(outCoord, 0)).xyz;
        if (all(gN >= 0.0f) && all(gN <= 1.0f))
            gN = gN * 2.0f - 1.0f;
        float gLen = dot(gN, gN);
        if (gLen > 0.1f && gLen < 4.0f)
            geomNormal = gN * rsqrt(gLen);
    }

    float centerLuma = Luma(centerColor);
    float lumaL = Luma(cL);
    float lumaR = Luma(cR);
    float lumaU = Luma(cU);
    float lumaD = Luma(cD);
    float lumaAvg = (lumaL + lumaR + lumaU + lumaD + Luma(cUL) + Luma(cUR) + Luma(cDL) + Luma(cDR)) * 0.125f;

    // Micro-bump perturbed surface normal for directional relighting
    float2 microGrad = float2(lumaR - lumaL, lumaD - lumaU);
    float3 microNormal = normalize(geomNormal + float3(-microGrad.x, microGrad.y, 0.0f) * (2.2f * (1.0f - haloRisk) * skyMask));
    float normalFacing = saturate(abs(microNormal.z));

    // Stage A + C: Bilateral Half-Res Temporal SSGI, Contact AO, and SSR
    float3 giBounce = float3(0.0f, 0.0f, 0.0f);
    float contactAo = 1.0f;
    float4 ssrSample = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if (GiEnabled > 0)
    {
        float4 giSample = BilateralSampleHalfRes(GiTex, outCoord, centerDepth);
        giBounce = max(giSample.rgb, 0.0f);
        contactAo = lerp(1.0f, saturate(giSample.a), saturate(AoIntensity) * skyMask);

        if (SsrEnabled > 0 && SsrIntensity > 1e-4f)
            ssrSample = BilateralSampleHalfRes(SsrTex, outCoord, centerDepth);
    }

    float3 litColor = centerColor;

    if (GiEnabled > 0)
    {
        float aoProtect = 1.0f / (1.0f + centerLuma * 0.75f);
        float effectiveAo = lerp(1.0f, contactAo, aoProtect);
        litColor *= effectiveAo;

        if (HasGBufferAlbedo > 0)
        {
            float3 albedo = saturate(GBufferAlbedoTex.Load(int3(outCoord, 0)).rgb);
            litColor += giBounce * albedo * (GiIntensity * 1.65f) * skyMask;
        }
        else
        {
            float3 approxAlbedo = saturate(centerColor / max(centerLuma, 0.06f)) * saturate(centerLuma * 1.5f + 0.15f);
            litColor += giBounce * approxAlbedo * (GiIntensity * 1.25f) * skyMask;
        }
    }

    // Roughness / smoothness factor (PBR G-buffer or perceptual fallback)
    float smoothness = saturate(0.32f + saturate(centerLuma * 0.65f) * 0.35f);
    if (HasGBufferRoughness > 0)
    {
        float rough = saturate(GBufferRoughnessTex.Load(int3(outCoord, 0)).r);
        smoothness = saturate(1.0f - rough);
    }

    // Real-time Glossy Screen-Space Reflections composite
    if (GiEnabled > 0 && SsrEnabled > 0 && SsrIntensity > 1e-4f && ssrSample.a > 1e-3f)
    {
        float fresnelSsr = 0.04f + 0.96f * pow(saturate(1.0f - normalFacing), 3.5f);
        float reflWeight = saturate(ssrSample.a * fresnelSsr * smoothness * SsrIntensity * 0.75f) * skyMask;
        litColor = lerp(litColor, max(litColor, ssrSample.rgb), reflWeight);
    }

    // 3D Directional Key/Rim Relighting using perturbed micro-normals
    if (RelightIntensity > 1e-4f)
    {
        float3 keyDir = normalize(float3(0.35f, 0.75f, -0.56f));
        float keyNdotL = dot(microNormal, keyDir);
        float keyFactor = keyNdotL * 0.24f * RelightIntensity * skyMask * (1.0f - haloRisk * 0.7f);
        float3 keyTint = (keyFactor >= 0.0f) ? float3(1.05f, 1.01f, 0.94f) : float3(0.88f, 0.94f, 1.08f);

        // Foreground Fresnel rim light separation
        float rim = pow(saturate(1.0f - abs(geomNormal.z)), 3.2f) * RelightIntensity * skyMask * 0.28f;
        float rimEnv = saturate(Luma(giBounce) * 1.8f + centerLuma * 0.4f);

        litColor = max(litColor * (1.0f + keyFactor * keyTint) + (giBounce + 0.08f) * rim * rimEnv, 0.0f);
    }

    // Grazing Specular Micro-Relief
    float microMag = length(microGrad);
    float fresnel = pow(saturate(1.0f - normalFacing), 2.5f);
    float grazingBoost = SpecularRelief * skyMask * (1.0f - haloRisk) * smoothness *
                         (0.45f * fresnel + 0.75f * saturate(microMag * 2.8f)) * saturate(centerLuma * 0.95f);
    litColor += litColor * grazingBoost * 0.42f;

    // Volumetric Light Wrap / Atmospheric Halation
    if (GiEnabled > 0 && VolumetricGlow > 1e-4f)
    {
        float3 glow = SampleVolumetricGlow(outCoord);
        float glowGate = saturate(0.25f + (1.0f - skyMask) * 0.75f + saturate(centerLuma * 0.5f));
        litColor += glow * (VolumetricGlow * 0.28f) * glowGate;
    }

    // Stage B: Embedded Tiled Residual Neural Detail & Relighting Network
    float highPass = centerLuma - lumaAvg;
    float variance = (abs(lumaL - lumaAvg) + abs(lumaR - lumaAvg) + abs(lumaU - lumaAvg) + abs(lumaD - lumaAvg)) * 0.25f;

    float f0 = saturate(log2(1.0f + centerLuma) * 0.5f) * 2.0f - 1.0f;
    float f1 = clamp(highPass * 6.5f, -1.0f, 1.0f);
    float f2 = saturate(variance * 8.5f) * 2.0f - 1.0f;
    float f3 = saturate(centerDepth / max(SkyDepthThreshold * 0.5f, 1.0f)) * 2.0f - 1.0f;
    float f4 = normalFacing * 2.0f - 1.0f;
    float f5 = saturate(Luma(giBounce) * 2.5f) * 2.0f - 1.0f;

    float3 neuralOut = EvalNeuralResidualMLP(f0, f1, f2, f3, f4, f5);

    int2 mvCoord = DisplaySizeMV > 0 ? outCoord : int2((float2(outCoord) + 0.5f) * MotionTextureScale);
    float2 mv = MotionTex.Load(int3(ClampMotionCoord(mvCoord), 0)).rg * float2(MvScaleX, MvScaleY);
    float motionDamp = 1.0f / (1.0f + length(mv) * 0.055f);

    float detailGate = skyMask * (1.0f - haloRisk) * motionDamp * NeuralDetail;
    float detailDelta = (neuralOut.x * 0.52f + highPass * 0.48f) * detailGate * 0.55f;
    float lightDelta = neuralOut.y * skyMask * (1.0f - haloRisk) * NeuralDetail * 0.20f;
    float chromaPop = neuralOut.z * skyMask * NeuralDetail * 0.24f;

    float outLuma = max(Luma(litColor), 1e-4f);
    float targetLuma = max(outLuma + detailDelta * max(outLuma, 0.16f) + lightDelta * outLuma, 0.0f);
    float3 finalColor = litColor * (targetLuma / outLuma);

    float3 chroma = finalColor - Luma(finalColor);
    finalColor = Luma(finalColor) + chroma * (1.0f + clamp(chromaPop, -0.15f, 0.35f));

    // HDR-safe filmic micro-contrast & highlight shoulder
    if (ToneContrast > 1e-4f)
    {
        float l = max(Luma(finalColor), 1e-5f);
        float cStr = clamp(ToneContrast, 0.0f, 1.0f);
        float logL = log2(l / 0.18f + 1e-4f);
        float shapedLog = logL * (1.0f + cStr * 0.28f) / (1.0f + abs(logL) * cStr * 0.09f);
        float shapedL = 0.18f * exp2(shapedLog);
        finalColor *= (shapedL / l);
    }

    // Debug views
    if (DebugView == 1)
        finalColor = giBounce * 2.5f;
    else if (DebugView == 2)
        finalColor = contactAo.xxx;
    else if (DebugView == 3)
        finalColor = microNormal * 0.5f + 0.5f;
    else if (DebugView == 4)
        finalColor = saturate(float3(detailDelta * 4.0f + 0.5f, lightDelta * 4.0f + 0.5f, chromaPop * 2.0f + 0.5f));
    else if (DebugView == 5)
        finalColor = ssrSample.rgb * ssrSample.a * 1.5f;

    finalColor = max(finalColor, 0.0f);
    DestColor[outCoord] = float4(EncodeColor(finalColor, OutColorEncodeMode), origRaw.a);
}
