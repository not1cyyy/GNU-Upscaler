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
    float TanHalfFov;

    float TanHalfFovAspect;
    int FrameIndex;
    float FusedRcasSharpness;
    int _pad2;
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> SourceColor : register(t0);

#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float4> HalfColorDepthTex : register(t1);

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
Texture2D<float4> HalfNormalTex : register(t4);

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

groupshared float4 s_color[18][18];
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

int2 ClampMotionCoord(int2 p)
{
    return int2(clamp(p.x, 0, MotionWidth - 1), clamp(p.y, 0, MotionHeight - 1));
}

int2 ClampGiCoord(int2 p)
{
    return int2(clamp(p.x, 0, GiWidth - 1), clamp(p.y, 0, GiHeight - 1));
}

float3 ReconstructViewPos(float2 uv, float linearDepth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    return float3(ndc.x * TanHalfFovAspect * linearDepth, ndc.y * TanHalfFov * linearDepth, linearDepth);
}

// Fused 5-tap depth-guided bilateral filter reading pre-linearized HalfColorDepthTex.a & Temporal G-Proxy flicker EMA
void FusedDenoiseHalfRes(int2 halfCoord, float centerDepth, float centerFlicker, bool sampleSsr, bool sampleGlow,
                         out float4 outGi, out float4 outSsr, out float3 outGlow, out float outFlicker)
{
    float invSigmaLog2 = 1.442695f / max(centerDepth * 0.045f, 0.02f);

    static const int2 offsets[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    static const float weights[5] = { 0.32f, 0.17f, 0.17f, 0.17f, 0.17f };

    float4 giAccum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 ssrAccum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 glowAccum = float3(0.0f, 0.0f, 0.0f);
    float wSum = 0.0f;
    float maxNeighborFlicker = 0.0f;
    float sumNeighborFlicker = 0.0f;

    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        int2 tapGi = ClampGiCoord(halfCoord + offsets[i]);
        float4 giTap = GiTex.Load(int3(tapGi, 0));

        float w = weights[i];
        if (i > 0)
        {
            float tapDepth = HalfColorDepthTex.Load(int3(tapGi, 0)).a;
            w *= exp2(-abs(centerDepth - tapDepth) * invSigmaLog2);

            float tapFlicker = saturate(HalfNormalTex.Load(int3(tapGi, 0)).z);
            maxNeighborFlicker = max(maxNeighborFlicker, tapFlicker);
            sumNeighborFlicker += tapFlicker;
        }
        w += 1e-5f;

        giAccum += giTap * w;
        wSum += w;

        float4 ssrTap = float4(0.0f, 0.0f, 0.0f, 0.0f);
        if (sampleSsr)
        {
            ssrTap = SsrTex.Load(int3(tapGi, 0));
            ssrAccum += ssrTap * w;
        }

        if (sampleGlow && i > 0)
        {
            float3 c = giTap.rgb * 0.6f + ssrTap.rgb * 0.4f;
            float l = Luma(c);
            float brightMask = max(l - 0.65f, 0.0f) / max(l, 1e-4f);
            glowAccum += c * brightMask;
        }
    }

    float invWSum = rcp(max(wSum, 1e-5f));
    outGi = giAccum * invWSum;
    outSsr = sampleSsr ? (ssrAccum * invWSum) : float4(0.0f, 0.0f, 0.0f, 0.0f);
    outGlow = sampleGlow ? (glowAccum * 0.25f) : float3(0.0f, 0.0f, 0.0f);
    outFlicker = saturate(max(centerFlicker, maxNeighborFlicker * 0.65f + (sumNeighborFlicker * 0.25f) * 0.35f));
}

#ifdef VK_MODE
#define half_f float
#define half_f3 float3
#else
#define half_f min16float
#define half_f3 min16float3
#endif

half_f3 MultiBounceAO(half_f visibility, half_f3 albedo)
{
    half_f3 a = half_f(2.0404f) * albedo - half_f(0.3324f);
    half_f3 b = half_f(-4.7951f) * albedo + half_f(0.6417f);
    half_f3 c = half_f(2.7552f) * albedo + half_f(0.6903f);
    half_f v = saturate(visibility);
    return max(v.xxx, ((v * a + b) * v + c) * v);
}

half_f FastSiLU(half_f x)
{
    return x * (half_f(0.5f) + half_f(0.5f) * x * rsqrt(half_f(1.0f) + x * x));
}

half_f3 EvalNeuralResidualMLP(half_f f0, half_f f1, half_f f2, half_f f3, half_f f4, half_f f5)
{
    static const half_f W1[8][6] = {
        {  0.22f,  1.45f, -0.55f, -0.20f,  0.30f,  0.15f },
        { -0.18f, -1.25f,  0.60f,  0.15f, -0.25f,  0.20f },
        {  0.35f,  0.25f,  0.85f, -0.35f,  0.40f, -0.30f },
        { -0.42f,  0.40f,  0.50f,  0.10f,  0.55f,  0.45f },
        {  0.28f, -0.35f,  0.70f, -0.45f,  0.20f,  0.50f },
        {  0.15f,  0.90f, -0.40f, -0.25f,  0.65f, -0.15f },
        { -0.30f,  0.15f,  0.65f,  0.30f, -0.40f,  0.60f },
        {  0.48f, -0.20f, -0.30f, -0.15f,  0.35f,  0.35f }
    };

    static const half_f W2[8][8] = {
        {  0.55f, -0.40f,  0.25f, -0.15f,  0.10f,  0.45f, -0.20f,  0.15f },
        { -0.35f,  0.50f,  0.15f,  0.30f, -0.20f, -0.30f,  0.25f,  0.10f },
        {  0.20f, -0.15f,  0.45f,  0.40f,  0.35f,  0.15f, -0.30f,  0.25f },
        { -0.15f,  0.25f, -0.30f,  0.55f,  0.45f,  0.20f,  0.35f, -0.25f },
        {  0.30f,  0.10f,  0.20f,  0.25f,  0.50f,  0.35f,  0.15f,  0.40f },
        {  0.40f, -0.35f,  0.30f, -0.10f,  0.25f,  0.55f, -0.15f,  0.20f },
        { -0.20f,  0.30f, -0.25f,  0.35f,  0.20f, -0.15f,  0.50f,  0.30f },
        {  0.15f,  0.20f,  0.40f,  0.15f,  0.30f,  0.25f,  0.20f,  0.45f }
    };

    static const half_f W3[3][8] = {
        {  0.65f, -0.58f,  0.18f, -0.12f,  0.10f,  0.48f, -0.15f,  0.12f },
        { -0.14f,  0.14f, -0.18f,  0.22f,  0.16f, -0.12f, -0.16f,  0.08f },
        {  0.18f, -0.14f,  0.22f,  0.18f,  0.24f,  0.22f,  0.10f,  0.24f }
    };

    half_f h1[8];
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        half_f s = W1[i][0] * f0 + W1[i][1] * f1 + W1[i][2] * f2 + W1[i][3] * f3 + W1[i][4] * f4 + W1[i][5] * f5;
        h1[i] = FastSiLU(s);
    }

    half_f h2[8];
    [unroll]
    for (int j = 0; j < 8; ++j)
    {
        half_f s = half_f(0.0f);
        [unroll]
        for (int k = 0; k < 8; ++k)
            s += W2[j][k] * h1[k];
        h2[j] = FastSiLU(s);
    }

    half_f3 outV = half_f3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (int m = 0; m < 8; ++m)
    {
        outV.x += W3[0][m] * h2[m];
        outV.y += W3[1][m] * h2[m];
        outV.z += W3[2][m] * h2[m];
    }

    return clamp(outV, half_f3(-0.35f, -0.35f, -0.35f), half_f3(0.35f, 0.35f, 0.35f));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    int2 groupBase = int2(gid.xy) * 16 - 1;
    uint linearIdx = gtid.y * 16 + gtid.x;
    float2 outToGi = float2(GiWidth, GiHeight) / float2(OutputWidth, OutputHeight);

    [unroll]
    for (uint passIdx = 0; passIdx < 2; ++passIdx)
    {
        uint idx = linearIdx + passIdx * 256;
        if (idx < 324)
        {
            uint sx = idx % 18;
            uint sy = idx / 18;
            int2 p = ClampOutputCoord(groupBase + int2(sx, sy));
            float4 raw = SourceColor.Load(int3(p, 0));
            s_color[sy][sx] = float4(EncodeColor(raw.rgb, InColorEncodeMode), raw.a);

            int2 pGi = ClampGiCoord(int2((float2(p) + 0.5f) * outToGi));
            s_depth[sy][sx] = HalfColorDepthTex.Load(int3(pGi, 0)).a;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if ((int) dtid.x >= OutputWidth || (int) dtid.y >= OutputHeight)
        return;

    int2 outCoord = int2(dtid.xy);
    int2 ldsCoord = int2(gtid.xy) + 1;
    float2 uv = (float2(outCoord) + 0.5f) / float2(OutputWidth, OutputHeight);

    float4 centerPacked = s_color[ldsCoord.y][ldsCoord.x];
    float3 centerColor = centerPacked.rgb;
    float origAlpha = centerPacked.a;

    if (SplitScreenPos > 0.01f && SplitScreenPos < 0.99f)
    {
        int splitX = int(SplitScreenPos * float(OutputWidth));
        if (abs(outCoord.x - splitX) <= 1)
        {
            DestColor[outCoord] = float4(EncodeColor(float3(1.0f, 1.0f, 1.0f), OutColorEncodeMode), origAlpha);
            return;
        }
        if (outCoord.x < splitX)
        {
            DestColor[outCoord] = SourceColor.Load(int3(outCoord, 0));
            return;
        }
    }

    float centerDepth = s_depth[ldsCoord.y][ldsCoord.x];
    float skyMask = 1.0f - smoothstep(SkyDepthThreshold * 0.65f, SkyDepthThreshold, centerDepth);
    if (centerDepth >= 64000.0f)
        skyMask = 0.0f;

    // 3x3 neighborhood from shared memory (reused by both Neural Look and fused RCAS!)
    float3 cL = s_color[ldsCoord.y][ldsCoord.x - 1].rgb;
    float3 cR = s_color[ldsCoord.y][ldsCoord.x + 1].rgb;
    float3 cU = s_color[ldsCoord.y - 1][ldsCoord.x].rgb;
    float3 cD = s_color[ldsCoord.y + 1][ldsCoord.x].rgb;

    float centerLuma = Luma(centerColor);
    float lumaL = Luma(cL);
    float lumaR = Luma(cR);
    float lumaU = Luma(cU);
    float lumaD = Luma(cD);

    // Fast skybox early-out (still applies fused RCAS if requested, skips all 3D GI/SSR/AO/MLP)
    if (skyMask <= 1e-4f && DebugView == 0)
    {
        float3 skyOut = centerColor;
        if (FusedRcasSharpness > 1e-4f)
        {
            float minL = min(min(lumaU, lumaL), min(lumaR, lumaD));
            float maxL = max(max(lumaU, lumaL), max(lumaR, lumaD));
            minL = min(minL, centerLuma);
            maxL = max(maxL, centerLuma);
            float hitMin = minL / max(4.0f * maxL, 1e-5f);
            float hitMax = (1.0f - saturate(maxL)) / min(4.0f * minL - 4.0f, -1e-5f);
            float w = clamp(max(-0.1875f, min(hitMin, hitMax)) * saturate(FusedRcasSharpness), -0.1875f, 0.0f);
            float rcasLuma = (centerLuma + w * (lumaU + lumaL + lumaR + lumaD)) * rcp(1.0f + 4.0f * w);
            skyOut *= clamp(rcasLuma / max(centerLuma, 1e-5f), 0.5f, 2.0f);
        }
        DestColor[outCoord] = float4(EncodeColor(max(skyOut, 0.0f), OutColorEncodeMode), origAlpha);
        return;
    }

    float3 cUL = s_color[ldsCoord.y - 1][ldsCoord.x - 1].rgb;
    float3 cUR = s_color[ldsCoord.y - 1][ldsCoord.x + 1].rgb;
    float3 cDL = s_color[ldsCoord.y + 1][ldsCoord.x - 1].rgb;
    float3 cDR = s_color[ldsCoord.y + 1][ldsCoord.x + 1].rgb;

    float dL = s_depth[ldsCoord.y][ldsCoord.x - 1];
    float dR = s_depth[ldsCoord.y][ldsCoord.x + 1];
    float dU = s_depth[ldsCoord.y - 1][ldsCoord.x];
    float dD = s_depth[ldsCoord.y + 1][ldsCoord.x];

    float maxDepthGrad = max(abs(dR - dL), abs(dD - dU));
    float relDepthJump = maxDepthGrad / max(centerDepth, 0.05f);
    float haloRisk = smoothstep(0.015f, 0.08f, relDepthJump);

    int2 halfCoord = ClampGiCoord(int2((float2(outCoord) + 0.5f) * outToGi));

    // Single-fetch temporally stabilized view normal & depth-flicker EMA from Temporal G-Proxy
    float4 hNorm = HalfNormalTex.Load(int3(halfCoord, 0));
    float2 nxy = clamp(hNorm.xy, -1.0f, 1.0f);
    float3 viewNormal = float3(nxy, -sqrt(max(0.0f, 1.0f - dot(nxy, nxy))));
    float centerFlicker = saturate(hNorm.z);

    float3 viewPos = ReconstructViewPos(uv, centerDepth);
    float3 viewDir = normalize(viewPos);
    float NdotV = saturate(-dot(viewDir, viewNormal));

    float lumaUL = Luma(cUL);
    float lumaUR = Luma(cUR);
    float lumaDL = Luma(cDL);
    float lumaDR = Luma(cDR);
    float lumaAvg = (lumaL + lumaR + lumaU + lumaD + lumaUL + lumaUR + lumaDL + lumaDR) * 0.125f;

    // Perceptual sqrt(luma) space stabilizes dark fabrics identically across Linear HDR, sRGB, and PQ
    float pC  = sqrt(max(centerLuma, 0.0f));
    float pL  = sqrt(max(lumaL, 0.0f));
    float pR  = sqrt(max(lumaR, 0.0f));
    float pU  = sqrt(max(lumaU, 0.0f));
    float pD  = sqrt(max(lumaD, 0.0f));
    float pUL = sqrt(max(lumaUL, 0.0f));
    float pUR = sqrt(max(lumaUR, 0.0f));
    float pDL = sqrt(max(lumaDL, 0.0f));
    float pDR = sqrt(max(lumaDR, 0.0f));
    float pAvg = (pL + pR + pU + pD + pUL + pUR + pDL + pDR) * 0.125f;
    float pVar = (abs(pL - pAvg) + abs(pR - pAvg) + abs(pU - pAvg) + abs(pD - pAvg)) * 0.25f;

    // Detect 1px high-frequency mesh / lace / hair grids (alternating center vs both opposite neighbors)
    float meshH = abs(2.0f * pC - pL - pR);
    float meshV = abs(2.0f * pC - pU - pD);
    float meshEnergy = max(meshH, meshV) + pVar * 1.5f;
    float meshGuard = rcp(1.0f + meshEnergy * meshEnergy * 32.0f);

    float2 microGrad = clamp(float2(pR - pL, pD - pU) / max(pC + 0.18f, 0.18f), -0.85f, 0.85f);

    // Fused 5-tap denoised GI, SSR, Volumetric Glow, and tight cross flicker dilation
    float3 giBounce = float3(0.0f, 0.0f, 0.0f);
    float contactAo = 1.0f;
    float4 ssrSample = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 brightGlow = float3(0.0f, 0.0f, 0.0f);
    float flickerSoft = centerFlicker;

    if (GiEnabled > 0)
    {
        float4 giSample;
        bool needSsr = (SsrEnabled > 0 && SsrIntensity > 1e-4f) || (DebugView == 5);
        bool needGlow = (VolumetricGlow > 1e-4f);
        FusedDenoiseHalfRes(halfCoord, centerDepth, centerFlicker, needSsr, needGlow, giSample, ssrSample, brightGlow,
                            flickerSoft);
        giBounce = max(giSample.rgb, 0.0f);
        contactAo = lerp(1.0f, saturate(giSample.a), saturate(AoIntensity) * skyMask * (1.0f - flickerSoft));
    }
    else
    {
        static const int2 dOffsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        float maxNeighborFlicker = 0.0f;
        float sumNeighborFlicker = 0.0f;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            float tapF = saturate(HalfNormalTex.Load(int3(ClampGiCoord(halfCoord + dOffsets[i]), 0)).z);
            maxNeighborFlicker = max(maxNeighborFlicker, tapF);
            sumNeighborFlicker += tapF;
        }
        flickerSoft = saturate(max(centerFlicker, maxNeighborFlicker * 0.65f + (sumNeighborFlicker * 0.25f) * 0.35f));
    }

    float solidMask = skyMask * (1.0f - flickerSoft);
    float3 microNormal = normalize(
        viewNormal + float3(-microGrad.x, microGrad.y, 0.0f) * (0.55f * meshGuard * (1.0f - haloRisk) * solidMask));

    float3 albedoApprox = saturate(centerColor / max(centerLuma, 0.06f)) * saturate(centerLuma * 1.4f);
    if (HasGBufferAlbedo > 0)
        albedoApprox = saturate(GBufferAlbedoTex.Load(int3(outCoord, 0)).rgb);

    float3 litColor = centerColor;

    if (GiEnabled > 0)
    {
        float ambientDelight = lerp(1.0f, 0.86f, saturate(GiIntensity * 0.45f) * skyMask);
        float aoProtect = rcp(1.0f + centerLuma * 0.85f);
        float3 coloredAo = lerp(float3(1.0f, 1.0f, 1.0f),
                                float3(MultiBounceAO(half_f(contactAo), half_f3(albedoApprox))), aoProtect);

        litColor = litColor * coloredAo * ambientDelight;

        float shadowFocus = rcp(1.0f + centerLuma * 1.35f);
        litColor += giBounce * albedoApprox * (GiIntensity * (0.65f + 0.75f * shadowFocus)) * solidMask;
    }

    float smoothness;
    if (HasGBufferRoughness > 0)
    {
        float rough = saturate(GBufferRoughnessTex.Load(int3(outCoord, 0)).r);
        smoothness = saturate(1.0f - rough);
    }
    else
    {
        // Balanced surface sheen: preserves rich floor/metal reflections while slightly softer on coarse detail
        float texRough = saturate(pVar * 4.5f);
        smoothness = saturate((0.46f + saturate(centerLuma * 0.35f) * 0.24f) * (1.0f - texRough * 0.30f));
    }

    if (GiEnabled > 0 && SsrEnabled > 0 && SsrIntensity > 1e-4f)
    {
        float fresnelSsr = lerp(0.44f, 0.78f, pow(1.0f - NdotV, 2.0f));
        float baseReflWeight =
            saturate(fresnelSsr * smoothness * SsrIntensity * 0.68f) * solidMask * (1.0f - haloRisk * 0.85f);

        // Keep floor sheen consistent from 0 deg horizontal to 60 deg pitched down; never darken vertical walls
        float floorMask = smoothstep(0.20f, 0.50f, viewNormal.y);
        float3 envFallback = giBounce * 1.15f + litColor * 0.32f;
        float hitAlpha = saturate(ssrSample.a);
        float3 reflColor = lerp(envFallback, ssrSample.rgb, hitAlpha);
        float reflWeight = baseReflWeight * lerp(0.82f * floorMask, 1.0f, hitAlpha);
        litColor = lerp(litColor, reflColor, reflWeight);
    }

    if (RelightIntensity > 1e-4f)
    {
        float3 keyDir = float3(0.45f, 0.78f, -0.43f);
        float deltaBump = (dot(microNormal, keyDir) - dot(viewNormal, keyDir)) * 0.65f;
        float macroKey = (dot(viewNormal, keyDir) - 0.18f) * 0.11f;
        float hemiTerm = viewNormal.y * 0.14f;

        float sculpt = (deltaBump + macroKey + hemiTerm) * RelightIntensity * solidMask * (1.0f - haloRisk * 0.8f);
        float3 sculptTint = (sculpt >= 0.0f) ? float3(1.04f, 1.01f, 0.96f) : float3(0.90f, 0.95f, 1.06f);
        litColor *= max(1.0f + sculpt * sculptTint, 0.25f);

        float verticalMask = smoothstep(0.08f, 0.45f, 1.0f - abs(viewNormal.y));
        float rim = pow(1.0f - NdotV, 3.5f) * verticalMask * RelightIntensity * solidMask * 0.18f;
        litColor += litColor * rim * saturate(Luma(giBounce) * 1.5f + 0.35f);
    }

    if (SpecularRelief > 1e-4f)
    {
        float microMag = length(microGrad) * meshGuard;
        float grazing = pow(1.0f - NdotV, 3.0f) * smoothness;
        float specSignal = 0.55f * grazing + 0.45f * saturate(microMag * 1.25f) * smoothness;
        float specMod = SpecularRelief * solidMask * (1.0f - haloRisk) * specSignal * 0.28f;
        litColor *= (1.0f + specMod);
    }

    if (GiEnabled > 0 && VolumetricGlow > 1e-4f)
        litColor += brightGlow * (VolumetricGlow * 0.30f * (0.25f + 0.75f * (1.0f - flickerSoft)));

    float detailDelta = 0.0f;
    float lightDelta = 0.0f;
    float chromaPop = 0.0f;
    float3 finalColor = litColor;

    if (NeuralDetail > 1e-4f || DebugView == 4)
    {
        float highPass = (pC - pAvg) / max(pAvg + 0.16f, 0.16f);

        half_f f0 = half_f(saturate(log2(1.0f + centerLuma) * 0.45f) * 2.0f - 1.0f);
        half_f f1 = half_f(clamp(highPass * 2.8f, -1.0f, 1.0f));
        half_f f2 = half_f(saturate((pVar / max(pAvg + 0.14f, 0.14f)) * 4.0f) * 2.0f - 1.0f);
        half_f f3 = half_f(saturate(log2(1.0f + centerDepth) * 0.125f) * 2.0f - 1.0f);
        half_f f4 = half_f(saturate(abs(microNormal.z)) * 2.0f - 1.0f);
        half_f f5 = half_f(saturate(Luma(giBounce) * 2.2f) * 2.0f - 1.0f);

        float3 neuralOut = float3(EvalNeuralResidualMLP(f0, f1, f2, f3, f4, f5));

        int2 mvCoord = DisplaySizeMV > 0 ? outCoord : int2((float2(outCoord) + 0.5f) * MotionTextureScale);
        float2 mv = MotionTex.Load(int3(ClampMotionCoord(mvCoord), 0)).rg * float2(MvScaleX, MvScaleY);
        float motionDamp = rcp(1.0f + length(mv) * 0.06f);

        float rcasDamp = lerp(1.0f, 0.35f, saturate(FusedRcasSharpness));
        float detailGate = solidMask * (1.0f - haloRisk) * motionDamp * meshGuard * NeuralDetail;
        detailDelta = (neuralOut.x * 0.55f + clamp(highPass, -0.35f, 0.35f) * 0.45f * rcasDamp) * detailGate * 0.30f;
        lightDelta = neuralOut.y * solidMask * (1.0f - haloRisk) * NeuralDetail * 0.12f;
        chromaPop = neuralOut.z * solidMask * NeuralDetail * 0.24f;

        float lumaScale = max(1.0f + detailDelta + lightDelta, 0.35f);
        finalColor = litColor * lumaScale;

        float finalLuma = Luma(finalColor);
        float3 chroma = finalColor - finalLuma;
        finalColor = finalLuma + chroma * (1.0f + clamp(chromaPop, -0.10f, 0.32f));
    }

    if (ToneContrast > 1e-4f)
    {
        float l = max(Luma(finalColor), 1e-5f);
        float cStr = clamp(ToneContrast, 0.0f, 1.0f);
        float midGray = 0.18f;
        float logRatio = log2(max(l / midGray, 1e-4f));
        float slope = 1.0f + cStr * 0.24f;
        float contrasted = midGray * exp2(clamp(logRatio * slope, -10.0f, 6.0f));
        float shoulder = l / (1.0f + max(l - 0.85f, 0.0f) * cStr * 0.20f);
        float targetL = lerp(contrasted, min(contrasted, shoulder), smoothstep(0.45f, 1.5f, l));
        finalColor *= clamp(targetL / l, 0.45f, 1.65f);
    }

    // Fused 5-tap AMD RCAS sharpening using groupshared neighborhood (eliminates separate RCAS pass)
    if (FusedRcasSharpness > 1e-4f && DebugView == 0)
    {
        float minL = min(min(lumaU, lumaL), min(lumaR, lumaD));
        float maxL = max(max(lumaU, lumaL), max(lumaR, lumaD));
        minL = min(minL, centerLuma);
        maxL = max(maxL, centerLuma);
        float hitMin = minL / max(4.0f * maxL, 1e-5f);
        float hitMax = (1.0f - saturate(maxL)) / min(4.0f * minL - 4.0f, -1e-5f);
        float rcasStrength = saturate(FusedRcasSharpness) * lerp(0.45f, 1.0f, meshGuard) * (1.0f - flickerSoft);
        float w = clamp(max(-0.1875f, min(hitMin, hitMax)) * rcasStrength, -0.1875f, 0.0f);
        float rcasLuma = (centerLuma + w * (lumaU + lumaL + lumaR + lumaD)) * rcp(1.0f + 4.0f * w);
        float rcasRatio = clamp(rcasLuma / max(centerLuma, 1e-5f), 0.5f, 2.0f);
        finalColor *= rcasRatio;
    }

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
    DestColor[outCoord] = float4(EncodeColor(finalColor, OutColorEncodeMode), origAlpha);
}
