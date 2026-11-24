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
    float CameraVFov;

    float AspectRatio;
    int _pad0;
    int _pad1;
    int _pad2;
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

float3 ReconstructViewPos(float2 uv, float linearDepth)
{
    float tanHalfFov = tan(clamp(CameraVFov, 0.25f, 2.4f) * 0.5f);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    return float3(ndc.x * tanHalfFov * AspectRatio * linearDepth, ndc.y * tanHalfFov * linearDepth, linearDepth);
}

// Exact depth-texel 3D normal reconstruction (crisp at all upscaling ratios)
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

// 9-tap depth-guided Atrous bilateral denoise over half-res GI/SSR for ray-traced smoothness without edge bleed
float4 DenoiseSampleHalfRes(Texture2D<float4> halfTex, int2 outCoord, float centerDepth)
{
    int2 giCenter = int2((float2(outCoord) + 0.5f) * (float2(GiWidth, GiHeight) / float2(OutputWidth, OutputHeight)));
    float invSigma = 1.0f / max(centerDepth * 0.045f, 0.02f);

    static const float kernel[3] = { 0.25f, 0.5f, 0.25f };
    float4 accum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float wSum = 0.0f;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 tapGi = ClampGiCoord(giCenter + int2(dx * 2, dy * 2));
            float4 tap = halfTex.Load(int3(tapGi, 0));

            int2 tapOut = ClampOutputCoord(int2((float2(tapGi) + 0.5f) * (float2(OutputWidth, OutputHeight) / float2(GiWidth, GiHeight))));
            float tapDepth = LoadDepthAtOutputCoord(tapOut);

            float spatialW = kernel[dx + 1] * kernel[dy + 1];
            float depthW = exp(-abs(centerDepth - tapDepth) * invSigma);
            float w = spatialW * depthW + 1e-5f;

            accum += tap * w;
            wSum += w;
        }
    }

    return accum / max(wSum, 1e-5f);
}

// Jimenez et al. 2016 Multi-Bounce Colored AO approximation
float3 MultiBounceAO(float visibility, float3 albedo)
{
    float3 a = 2.0404f * albedo - 0.3324f;
    float3 b = -4.7951f * albedo + 0.6417f;
    float3 c = 2.7552f * albedo + 0.6903f;
    float v = saturate(visibility);
    return max(v.xxx, ((v * a + b) * v + c) * v);
}

float3 SampleBrightVolumetricGlow(int2 outCoord)
{
    int2 giCenter = int2((float2(outCoord) + 0.5f) * (float2(GiWidth, GiHeight) / float2(OutputWidth, OutputHeight)));
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    static const int2 offsets[4] = { int2(-4, 0), int2(4, 0), int2(0, -4), int2(0, 4) };
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float3 g = GiTex.Load(int3(ClampGiCoord(giCenter + offsets[i]), 0)).rgb;
        float3 s = SsrTex.Load(int3(ClampGiCoord(giCenter + offsets[i]), 0)).rgb;
        float3 c = g * 0.6f + s * 0.4f;
        // Extract only genuinely bright emissive/specular energy (> 0.65)
        float l = Luma(c);
        float brightMask = max(l - 0.65f, 0.0f) / max(l, 1e-4f);
        sum += c * brightMask;
    }
    return sum * 0.25f;
}

float SiLU(float x)
{
    return x / (1.0f + exp(-clamp(x, -12.0f, 12.0f)));
}

// Embedded-weight 3-layer residual MLP (6 -> 8 -> 8 -> 3)
// Returns zero-centered [deltaDetail, deltaMicroLight, deltaChromaPop]
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
    static const float B1[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

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

    static const float W3[3][8] = {
        {  0.65f, -0.58f,  0.18f, -0.12f,  0.10f,  0.48f, -0.15f,  0.12f },
        { -0.14f,  0.14f, -0.18f,  0.22f,  0.16f, -0.12f, -0.16f,  0.08f },
        {  0.18f, -0.14f,  0.22f,  0.18f,  0.24f,  0.22f,  0.10f,  0.24f }
    };

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
        float s = 0.0f;
        [unroll]
        for (int k = 0; k < 8; ++k)
            s += W2[j][k] * h1[k];
        h2[j] = SiLU(s);
    }

    float3 outV = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int m = 0; m < 8; ++m)
    {
        outV.x += W3[0][m] * h2[m];
        outV.y += W3[1][m] * h2[m];
        outV.z += W3[2][m] * h2[m];
    }

    return clamp(outV, -0.35f, 0.35f);
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
    float2 uv = (float2(outCoord) + 0.5f) / float2(OutputWidth, OutputHeight);

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

    // 3x3 color & depth stats from shared memory
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

    float maxDepthGrad = max(abs(dR - dL), abs(dD - dU));
    float relDepthJump = maxDepthGrad / max(centerDepth, 0.05f);
    float haloRisk = smoothstep(0.015f, 0.08f, relDepthJump);

    // True 3D depth-texel view normal (+ optional G-buffer normal)
    float3 viewNormal = ReconstructViewNormalFromDepthTexel(uv);
    if (HasGBufferNormals > 0)
    {
        float3 gN = GBufferNormalsTex.Load(int3(outCoord, 0)).xyz;
        if (all(gN >= 0.0f) && all(gN <= 1.0f))
            gN = gN * 2.0f - 1.0f;
        float gLen = dot(gN, gN);
        if (gLen > 0.1f && gLen < 4.0f)
            viewNormal = gN * rsqrt(gLen);
    }

    float3 viewPos = ReconstructViewPos(uv, centerDepth);
    float3 viewDir = normalize(viewPos);
    float NdotV = saturate(-dot(viewDir, viewNormal));

    float centerLuma = Luma(centerColor);
    float lumaL = Luma(cL);
    float lumaR = Luma(cR);
    float lumaU = Luma(cU);
    float lumaD = Luma(cD);
    float lumaAvg = (lumaL + lumaR + lumaU + lumaD + Luma(cUL) + Luma(cUR) + Luma(cDL) + Luma(cDR)) * 0.125f;

    // Micro-bump surface normal perturbation (screen-space relief)
    float2 microGrad = float2(lumaR - lumaL, lumaD - lumaU) / max(centerLuma + 0.12f, 0.12f);
    float3 microNormal = normalize(viewNormal + float3(-microGrad.x, microGrad.y, 0.0f) * (1.85f * (1.0f - haloRisk) * skyMask));

    // Stage A + C: 9-tap Denoised Half-Res Temporal SSGI, Multi-Bounce AO, and SSR
    float3 giBounce = float3(0.0f, 0.0f, 0.0f);
    float contactAo = 1.0f;
    float4 ssrSample = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if (GiEnabled > 0)
    {
        float4 giSample = DenoiseSampleHalfRes(GiTex, outCoord, centerDepth);
        giBounce = max(giSample.rgb, 0.0f);
        contactAo = lerp(1.0f, saturate(giSample.a), saturate(AoIntensity) * skyMask);

        if (SsrEnabled > 0 && SsrIntensity > 1e-4f)
            ssrSample = DenoiseSampleHalfRes(SsrTex, outCoord, centerDepth);
    }

    float3 albedoApprox = saturate(centerColor / max(centerLuma, 0.06f)) * saturate(centerLuma * 1.4f);
    if (HasGBufferAlbedo > 0)
        albedoApprox = saturate(GBufferAlbedoTex.Load(int3(outCoord, 0)).rgb);

    float3 litColor = centerColor;

    // 1. Energy-Conserving Multi-Bounce AO + Ambient De-Lighted Indirect GI Bounce
    if (GiEnabled > 0)
    {
        // De-light flat ambient slightly so colored bounce & AO sculpt depth instead of lifting exposure
        float ambientDelight = lerp(1.0f, 0.84f, saturate(GiIntensity * 0.45f) * skyMask);
        float aoProtect = 1.0f / (1.0f + centerLuma * 0.85f);
        float3 coloredAo = lerp(float3(1.0f, 1.0f, 1.0f), MultiBounceAO(contactAo, albedoApprox), aoProtect);

        litColor = litColor * coloredAo * ambientDelight;

        // Inject bounce light strongest in shadowed/shaded crevices (never blows out sunlit areas)
        float shadowFocus = 1.0f / (1.0f + centerLuma * 1.35f);
        litColor += giBounce * albedoApprox * (GiIntensity * (0.65f + 0.75f * shadowFocus)) * skyMask;
    }

    // Roughness / smoothness (PBR G-buffer or perceptual reflectance fallback)
    float smoothness = saturate(0.28f + saturate(centerLuma * 0.55f) * 0.35f);
    if (HasGBufferRoughness > 0)
    {
        float rough = saturate(GBufferRoughnessTex.Load(int3(outCoord, 0)).r);
        smoothness = saturate(1.0f - rough);
    }

    // 2. Energy-Conserving Fresnel Screen-Space Reflections (pure lerp, never additive!)
    if (GiEnabled > 0 && SsrEnabled > 0 && SsrIntensity > 1e-4f && ssrSample.a > 1e-3f)
    {
        float fresnelSsr = 0.04f + 0.96f * pow(1.0f - NdotV, 4.0f);
        float reflWeight = saturate(ssrSample.a * fresnelSsr * smoothness * SsrIntensity * 0.85f) * skyMask;
        litColor = lerp(litColor, ssrSample.rgb, reflWeight);
    }

    // 3. Zero-Mean 3D Micro-Normal Sculpting & Directional Hemisphere Relighting
    if (RelightIntensity > 1e-4f)
    {
        float3 keyDir = normalize(float3(0.45f, 0.78f, -0.43f));
        // Purely relative micro-bump delta (zero spatial mean -> carves shadows AND pops highlights)
        float deltaBump = (dot(microNormal, keyDir) - dot(viewNormal, keyDir)) * 0.95f;
        // Hemisphere vertical sculpting (upward faces catch warm sky, downward faces deepen cool shadow)
        float hemiTerm = viewNormal.y * 0.16f;

        float sculpt = (deltaBump + hemiTerm) * RelightIntensity * skyMask * (1.0f - haloRisk * 0.8f);
        float3 sculptTint = (sculpt >= 0.0f) ? float3(1.04f, 1.01f, 0.96f) : float3(0.90f, 0.95f, 1.06f);
        litColor *= max(1.0f + sculpt * sculptTint, 0.25f);

        // Strictly luminance-proportional grazing rim (zero linear floor -> black stays black)
        float rim = pow(1.0f - NdotV, 3.5f) * RelightIntensity * skyMask * 0.22f;
        litColor += litColor * rim * saturate(Luma(giBounce) * 1.5f + 0.35f);
    }

    // 4. Zero-Mean Micro-Specular Relief
    float microMag = length(microGrad);
    float grazing = pow(1.0f - NdotV, 2.5f);
    float specSignal = (0.35f * grazing + 0.65f * saturate(microMag * 1.8f)) - 0.12f;
    float specMod = SpecularRelief * skyMask * (1.0f - haloRisk) * smoothness * specSignal * 0.32f;
    litColor *= max(1.0f + specMod, 0.65f);

    // 5. Bright-Only Volumetric Halation
    if (GiEnabled > 0 && VolumetricGlow > 1e-4f)
    {
        float3 glow = SampleBrightVolumetricGlow(outCoord);
        litColor += glow * (VolumetricGlow * 0.30f);
    }

    // Stage B: Mean-Preserving Tiled Residual Neural Detail & Relighting
    float highPass = (centerLuma - lumaAvg) / max(lumaAvg + 0.10f, 0.10f);
    float variance = (abs(lumaL - lumaAvg) + abs(lumaR - lumaAvg) + abs(lumaU - lumaAvg) + abs(lumaD - lumaAvg)) * 0.25f;

    float f0 = saturate(log2(1.0f + centerLuma) * 0.45f) * 2.0f - 1.0f;
    float f1 = clamp(highPass * 3.5f, -1.0f, 1.0f);
    float f2 = saturate((variance / max(lumaAvg + 0.08f, 0.08f)) * 4.5f) * 2.0f - 1.0f;
    float f3 = saturate(log2(1.0f + centerDepth) / 8.0f) * 2.0f - 1.0f;
    float f4 = saturate(abs(microNormal.z)) * 2.0f - 1.0f;
    float f5 = saturate(Luma(giBounce) * 2.2f) * 2.0f - 1.0f;

    float3 neuralOut = EvalNeuralResidualMLP(f0, f1, f2, f3, f4, f5);

    int2 mvCoord = DisplaySizeMV > 0 ? outCoord : int2((float2(outCoord) + 0.5f) * MotionTextureScale);
    float2 mv = MotionTex.Load(int3(ClampMotionCoord(mvCoord), 0)).rg * float2(MvScaleX, MvScaleY);
    float motionDamp = 1.0f / (1.0f + length(mv) * 0.06f);

    float detailGate = skyMask * (1.0f - haloRisk) * motionDamp * NeuralDetail;
    // Strictly zero-mean detail & light modulation (pivoted around local contrast, no global DC shift)
    float detailDelta = (neuralOut.x * 0.45f + clamp(highPass, -0.5f, 0.5f) * 0.55f) * detailGate * 0.48f;
    float lightDelta = neuralOut.y * skyMask * (1.0f - haloRisk) * NeuralDetail * 0.14f;
    float chromaPop = neuralOut.z * skyMask * NeuralDetail * 0.26f;

    float outLuma = max(Luma(litColor), 1e-5f);
    float lumaScale = max(1.0f + detailDelta + lightDelta, 0.35f);
    float3 finalColor = litColor * lumaScale;

    float finalLuma = Luma(finalColor);
    float3 chroma = finalColor - finalLuma;
    finalColor = finalLuma + chroma * (1.0f + clamp(chromaPop, -0.10f, 0.32f));

    // Local-Mean-Preserving Filmic Contrast (deepens blacks/shadows without lifting midtones)
    if (ToneContrast > 1e-4f)
    {
        float l = max(Luma(finalColor), 1e-5f);
        float cStr = clamp(ToneContrast, 0.0f, 1.0f);
        float pivot = max(lumaAvg, 0.08f);
        float ratio = l / pivot;
        float contrasted = pow(max(ratio, 1e-4f), 1.0f + cStr * 0.26f) * pivot;
        // Soft highlight protection above 1.0 so HDR/bright whites never clip
        float shoulder = l / (1.0f + max(l - 1.0f, 0.0f) * cStr * 0.18f);
        float targetL = lerp(contrasted, min(contrasted, shoulder), saturate(l * 0.5f));
        finalColor *= (targetL / l);
    }

    // Debug views
    if (DebugView == 1)
        finalColor = giBounce * 2.5f;
    else if (DebugView == 2)
        finalColor = contactAo.xxx;
    else if (DebugView == 3)
        finalColor = viewNormal * 0.5f + 0.5f;
    else if (DebugView == 4)
        finalColor = saturate(float3(detailDelta * 4.0f + 0.5f, lightDelta * 4.0f + 0.5f, chromaPop * 2.0f + 0.5f));
    else if (DebugView == 5)
        finalColor = ssrSample.rgb * ssrSample.a * 1.5f;

    finalColor = max(finalColor, 0.0f);
    DestColor[outCoord] = float4(EncodeColor(finalColor, OutColorEncodeMode), origRaw.a);
}
