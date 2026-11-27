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
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]]
#endif
Texture2D<float4> ColorPyramidTex : register(t7);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]]
#endif
Texture2D<float4> Material0Tex : register(t8);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]]
#endif
Texture2D<float4> Material1Tex : register(t9);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]]
#endif
Texture3D<float4> BilateralGrid0Tex : register(t10);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(12, 0)]]
#endif
Texture3D<float4> BilateralGrid1Tex : register(t11);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(13, 0)]]
#endif
Texture3D<float4> BilateralGrid2Tex : register(t12);

#ifdef VK_MODE
[[vk::binding(14, 0)]]
#endif
Texture2D<float4> GBufferAlbedoTex : register(t13);

#ifdef VK_MODE
[[vk::binding(15, 0)]]
#endif
Texture2D<float4> GBufferRoughnessTex : register(t14);

#ifdef VK_MODE
[[vk::binding(16, 0)]]
#endif
Texture2D<float4> GBufferMetallicTex : register(t15);

#ifdef VK_MODE
[[vk::binding(17, 0)]]
#endif
Texture2D<float4> GBufferSubsurfaceTex : register(t16);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]]
SamplerState LinearClampPyr : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]]
SamplerState LinearClampMat0 : register(s1);
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]]
SamplerState LinearClampMat1 : register(s2);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]]
SamplerState LinearClampBg0 : register(s3);
[[vk::combinedImageSampler]] [[vk::binding(12, 0)]]
SamplerState LinearClampBg1 : register(s4);
[[vk::combinedImageSampler]] [[vk::binding(13, 0)]]
SamplerState LinearClampBg2 : register(s5);
#else
SamplerState LinearClamp : register(s0);
#define LinearClampPyr LinearClamp
#define LinearClampMat0 LinearClamp
#define LinearClampMat1 LinearClamp
#define LinearClampBg0 LinearClamp
#define LinearClampBg1 LinearClamp
#define LinearClampBg2 LinearClamp
#endif

#ifdef VK_MODE
[[vk::binding(18, 0)]]
#endif
StructuredBuffer<float4> ResolvedSceneLightBuf : register(t17);

#ifdef VK_MODE
[[vk::binding(19, 0)]]
#endif
StructuredBuffer<uint> WorldHashBuf : register(t18);

#ifdef VK_MODE
[[vk::binding(20, 0)]]
#endif
RWTexture2D<float4> DestColor : register(u0);

// 18x18 LDS tile of full-res linear color + render-res linearized depth
groupshared float4 s_color[18][18];
groupshared float s_depth[18][18];

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
    int activeW = (DisplaySizeMV > 0) ? OutputWidth : RenderWidth;
    int activeH = (DisplaySizeMV > 0) ? OutputHeight : RenderHeight;
    int2 minC = int2(MvSubrectBaseX, MvSubrectBaseY);
    int2 maxC = int2(min(MvSubrectBaseX + activeW - 1, MotionWidth - 1),
                     min(MvSubrectBaseY + activeH - 1, MotionHeight - 1));
    return clamp(p, minC, max(minC, maxC));
}

int2 UvToDepthCoord(float2 uv)
{
    float2 jitterUv = uv - float2(JitterOffsetX, JitterOffsetY) / float2(max(RenderWidth, 1), max(RenderHeight, 1));
    int2 c = int2(DepthSubrectBaseX, DepthSubrectBaseY) + int2(saturate(jitterUv) * float2(RenderWidth, RenderHeight));
    return ClampDepthCoord(c);
}

// Native RelaxedPrecision FP16 helpers (both DX12 & Vulkan SPIR-V)
min16float3 MultiBounceAO(min16float visibility, min16float3 albedo)
{
    min16float3 a = min16float(2.0404f) * albedo - min16float(0.3324f);
    min16float3 b = min16float(-4.7951f) * albedo + min16float(0.6417f);
    min16float3 c = min16float(2.7552f) * albedo + min16float(0.6903f);
    min16float v = saturate(visibility);
    return max(v.xxx, ((v * a + b) * v + c) * v);
}

// AgX-inspired highlight-desaturating filmic curve (preserves hue on bright lights instead of neon clipping)
float3 ApplyAgxFilmicContrast(float3 linearCol, float contrastStr)
{
    static const float3x3 agxInset = float3x3(
        0.84247906f, 0.07843360f, 0.07922375f,
        0.04232824f, 0.87846864f, 0.07916613f,
        0.04237565f, 0.07843360f, 0.87914297f
    );
    static const float3x3 agxOutset = float3x3(
        1.19687900f, -0.09802088f, -0.09902974f,
        -0.05289685f, 1.15190313f, -0.09896118f,
        -0.05297163f, -0.09804345f, 1.15107367f
    );

    float3 inset = mul(agxInset, max(linearCol, 0.0f));
    float l = max(Luma(inset), 1e-5f);
    float midGray = 0.18f;
    float logRatio = log2(max(l / midGray, 1e-4f));
    float slope = 1.0f + contrastStr * 0.24f;
    float contrasted = midGray * exp2(clamp(logRatio * slope, -10.0f, 6.0f));

    // Smooth highlight rolloff shoulder preserving HDR headroom
    float shoulder = l / (1.0f + max(l - 0.90f, 0.0f) * contrastStr * 0.18f);
    float targetL = lerp(contrasted, min(contrasted, shoulder), smoothstep(0.45f, 1.6f, l));
    float3 graded = inset * clamp(targetL / l, 0.45f, 1.65f);

    // Subtle AgX highlight purity rolloff above 1.2
    float hiBlend = saturate((targetL - 1.1f) * 0.22f) * contrastStr;
    graded = lerp(graded, targetL.xxx, hiBlend * 0.45f);

    return max(mul(agxOutset, graded), 0.0f);
}

// Joint-bilateral upsample of half-res DenoisedGI, DenoisedSSR, DenoisedMeta, and flicker dilation
void BilateralUpsampleHalfRes(int2 halfCoord, float centerDepth, float centerFlicker, bool sampleSsr,
                              out float4 outGi, out float4 outSsr, out float outContactShadow, out float outFlicker)
{
    float invSigmaLog2 = 1.442695f / max(centerDepth * 0.045f, 0.02f);

    static const int2 offsets[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    static const float weights[5] = { 0.36f, 0.16f, 0.16f, 0.16f, 0.16f };

    float4 giAccum = float4(0, 0, 0, 0);
    float4 ssrAccum = float4(0, 0, 0, 0);
    float csAccum = 0.0f;
    float wSum = 0.0f;
    float maxNFlick = 0.0f;
    float sumNFlick = 0.0f;

    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        int2 tapGi = ClampGiCoord(halfCoord + offsets[i]);
        float4 giTap = DenoisedGiTex.Load(int3(tapGi, 0));
        float csTap = DenoisedMetaTex.Load(int3(tapGi, 0)).r;

        float w = weights[i];
        if (i > 0)
        {
            float4 hNormTap = HalfNormalTex.Load(int3(tapGi, 0));
            w *= exp2(-abs(centerDepth - hNormTap.w) * invSigmaLog2);
            float tf = saturate(hNormTap.z);
            maxNFlick = max(maxNFlick, tf);
            sumNFlick += tf;
        }
        w += 1e-5f;

        giAccum += giTap * w;
        csAccum += csTap * w;
        if (sampleSsr)
            ssrAccum += DenoisedSsrTex.Load(int3(tapGi, 0)) * w;
        wSum += w;
    }

    float invW = rcp(max(wSum, 1e-5f));
    outGi = giAccum * invW;
    outSsr = sampleSsr ? (ssrAccum * invW) : float4(0, 0, 0, 0);
    outContactShadow = saturate(csAccum * invW);
    outFlicker = saturate(max(centerFlicker, maxNFlick * 0.65f + (sumNFlick * 0.25f) * 0.35f));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    int2 groupBase = int2(gid.xy) * 16 - 1;
    uint linearIdx = gtid.y * 16u + gtid.x;
    float2 invOut = 1.0f / float2(max(OutputWidth, 1), max(OutputHeight, 1));
    float2 outToGi = float2(GiWidth, GiHeight) / float2(max(OutputWidth, 1), max(OutputHeight, 1));
    float preExp = max(PreExposure, 1e-4f);
    float invPreExp = rcp(preExp);

    // Populate 18x18 LDS with full-res Color (PreExposure-normalized) + full/render-res Depth
    [unroll]
    for (uint passIdx = 0u; passIdx < 2u; ++passIdx)
    {
        uint idx = linearIdx + passIdx * 256u;
        if (idx < 324u)
        {
            uint sx = idx % 18u;
            uint sy = idx / 18u;
            int2 p = ClampOutputCoord(groupBase + int2(sx, sy));
            float4 raw = SourceColor.Load(int3(p, 0));
            s_color[sy][sx] = float4(EncodeColor(raw.rgb, InColorEncodeMode) * invPreExp, raw.a);

            float2 pUv = (float2(p) + 0.5f) * invOut;
            float rawD = DepthTex.Load(int3(UvToDepthCoord(pUv), 0)).r;
            s_depth[sy][sx] = LinearizeDepthValue(rawD, DepthIsLinear, DepthIsReversed,
                                                  DepthLinearA, DepthLinearB, DepthLinearC, SkyDepthThreshold);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if ((int) dtid.x >= OutputWidth || (int) dtid.y >= OutputHeight)
        return;

    int2 outCoord = int2(dtid.xy);
    int2 ldsCoord = int2(gtid.xy) + 1;
    float2 uv = (float2(outCoord) + 0.5f) * invOut;

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

    float3 cL = s_color[ldsCoord.y][ldsCoord.x - 1].rgb;
    float3 cR = s_color[ldsCoord.y][ldsCoord.x + 1].rgb;
    float3 cU = s_color[ldsCoord.y - 1][ldsCoord.x].rgb;
    float3 cD = s_color[ldsCoord.y + 1][ldsCoord.x].rgb;

    float centerLuma = Luma(centerColor);
    float lumaL = Luma(cL);
    float lumaR = Luma(cR);
    float lumaU = Luma(cU);
    float lumaD = Luma(cD);

    // Multi-scale Volumetric Light Wrap from ColorPyramid mips 2, 3, 4
    float3 multiScaleGlow = float3(0, 0, 0);
    if (VolumetricGlow > 1e-4f)
    {
        float3 g2 = ColorPyramidTex.SampleLevel(LinearClampPyr, uv, 2.0f).rgb;
        float3 g3 = ColorPyramidTex.SampleLevel(LinearClampPyr, uv, 3.0f).rgb;
        float3 g4 = ColorPyramidTex.SampleLevel(LinearClampPyr, uv, 4.0f).rgb;
        float3 wide = g2 * 0.35f + g3 * 0.40f + g4 * 0.25f;
        float wL = Luma(wide);
        float brightGate = max(wL - 0.55f, 0.0f) / max(wL, 1e-4f);
        multiScaleGlow = wide * brightGate;
    }

    // Fast skybox early-out
    if (skyMask <= 1e-4f && DebugView == 0)
    {
        float3 skyOut = centerColor + multiScaleGlow * (VolumetricGlow * 0.14f);
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
        DestColor[outCoord] = float4(EncodeColor(max(skyOut * preExp, 0.0f), OutColorEncodeMode), origAlpha);
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
    float haloRisk = smoothstep(0.015f, 0.08f, maxDepthGrad / max(centerDepth, 0.05f));

    int2 halfCoord = ClampGiCoord(int2((float2(outCoord) + 0.5f) * outToGi));
    float4 hNorm = HalfNormalTex.Load(int3(halfCoord, 0));
    float2 nxy = clamp(hNorm.xy, -1.0f, 1.0f);
    float3 viewNormal = float3(nxy, -sqrt(max(0.0f, 1.0f - dot(nxy, nxy))));
    float centerFlicker = saturate(hNorm.z);

    float3 viewPos = ReconstructViewPos(uv, centerDepth, TanHalfFov, TanHalfFovAspect);
    float3 viewDir = normalize(viewPos);
    float NdotV = saturate(-dot(viewDir, viewNormal));

    float lumaUL = Luma(cUL);
    float lumaUR = Luma(cUR);
    float lumaDL = Luma(cDL);
    float lumaDR = Luma(cDR);

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

    float meshH = abs(2.0f * pC - pL - pR);
    float meshV = abs(2.0f * pC - pU - pD);
    float meshEnergy = max(meshH, meshV) + pVar * 1.5f;
    float meshGuard = rcp(1.0f + meshEnergy * meshEnergy * 32.0f);
    float2 microGrad = clamp(float2(pR - pL, pD - pU) / max(pC + 0.18f, 0.18f), -0.85f, 0.85f);

    // Upsample GI, Contact Shadow, SSR
    float3 giBounce = float3(0, 0, 0);
    float contactAo = 1.0f;
    float contactShadow = 1.0f;
    float4 ssrSample = float4(0, 0, 0, 0);
    float flickerSoft = centerFlicker;

    if (GiEnabled > 0)
    {
        float4 giSample;
        bool needSsr = (SsrEnabled > 0 && SsrIntensity > 1e-4f) || (DebugView == 5);
        BilateralUpsampleHalfRes(halfCoord, centerDepth, centerFlicker, needSsr,
                                 giSample, ssrSample, contactShadow, flickerSoft);
        giBounce = max(giSample.rgb, 0.0f);
        contactAo = lerp(1.0f, saturate(giSample.a), saturate(AoIntensity) * skyMask * (1.0f - flickerSoft));
        contactShadow = lerp(1.0f, contactShadow, saturate(ContactShadowIntensity) * skyMask * (1.0f - flickerSoft));
    }
    else
    {
        static const int2 dOffsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        float maxNF = 0.0f;
        float sumNF = 0.0f;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            float tf = saturate(HalfNormalTex.Load(int3(ClampGiCoord(halfCoord + dOffsets[i]), 0)).z);
            maxNF = max(maxNF, tf);
            sumNF += tf;
        }
        flickerSoft = saturate(max(centerFlicker, maxNF * 0.65f + (sumNF * 0.25f) * 0.35f));
    }

    float solidMask = skyMask * (1.0f - flickerSoft);
    float3 microNormal = normalize(
        viewNormal + float3(-microGrad.x, microGrad.y, 0.0f) * (0.50f * meshGuard * (1.0f - haloRisk) * solidMask));

    // Fetch Trained Neural Material & Intrinsic Priors (override with render-res DLSS-RR G-buffers when present!)
    float4 mat0 = Material0Tex.SampleLevel(LinearClampMat0, uv, 0.0f);
    float4 mat1 = Material1Tex.SampleLevel(LinearClampMat1, uv, 0.0f);

    float3 albedo = saturate(mat0.rgb);
    float directMask = saturate(mat0.a);
    float roughness = saturate(mat1.r);
    float metallic = saturate(mat1.g);
    float skinMask = saturate(mat1.b);
    float foliageMask = saturate(mat1.a);

    int2 gCoord = UvToDepthCoord(uv);
    if (HasGBufferAlbedo > 0)
        albedo = saturate(GBufferAlbedoTex.Load(int3(gCoord, 0)).rgb);
    if (HasGBufferRoughness > 0)
        roughness = saturate(GBufferRoughnessTex.Load(int3(gCoord, 0)).r);
    if (HasGBufferMetallic > 0)
        metallic = saturate(GBufferMetallicTex.Load(int3(gCoord, 0)).r);
    if (HasGBufferSubsurface > 0)
        skinMask = max(skinMask, saturate(GBufferSubsurfaceTex.Load(int3(gCoord, 0)).r));

    float smoothness = saturate(1.0f - roughness);
    float clothMask = saturate((1.0f - metallic) * (1.0f - skinMask) * (1.0f - foliageMask) *
                               smoothstep(0.60f, 0.92f, roughness));

    // Phase 3: Zero-Bandwidth Separable Screen-Space Subsurface Scattering (SSSSS) in LDS for Skin!
    float3 litColor = centerColor;
    if (skinMask > 0.06f && SssIntensity > 1e-4f && solidMask > 0.05f)
    {
        float sssInvSigma = rcp(max(centerDepth * 0.028f, 0.015f));
        float3 sumCol = centerColor * 0.40f;
        float wSssSum = 0.40f;

        static const int2 sssTapOff[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        [unroll]
        for (int st = 0; st < 4; ++st)
        {
            int2 sp = ldsCoord + sssTapOff[st];
            float sd = s_depth[sp.y][sp.x];
            float sw = exp(-abs(centerDepth - sd) * sssInvSigma) * 0.15f;
            sumCol += s_color[sp.y][sp.x].rgb * sw;
            wSssSum += sw;
        }
        float3 blurredSkin = sumCol / max(wSssSum, 1e-4f);
        // Wavelength-dependent diffusion: warm red penetrates deepest, blue stays sharp
        float3 rgbFalloff = float3(0.75f, 0.32f, 0.14f) * saturate(skinMask * SssIntensity * 0.85f) * solidMask * (1.0f - haloRisk);
        litColor = lerp(litColor, blurredSkin, rgbFalloff);
    }

    // Scene-Derived Key Light & World Up (zero camera headlamp!)
    float4 keyDirData = ResolvedSceneLightBuf[4];
    float3 keyDirView = normalize(keyDirData.xyz);
    float keyStrength = saturate(keyDirData.w);
    float3 keyColor = max(ResolvedSceneLightBuf[5].rgb, 0.05f);
    float keyNormLuma = max(Luma(keyColor), 1e-4f);
    float3 keyTint = clamp(keyColor / keyNormLuma, 0.70f, 1.35f);
    float3 worldUpInView = normalize(ResolvedSceneLightBuf[6].xyz);

    // Energy-Conserving GI + Direct-Light-Protected Multi-Bounce AO + Contact Shadows
    if (GiEnabled > 0)
    {
        float indirectWeight = saturate(1.0f - directMask * 0.76f) * (1.0f - skinMask * 0.45f);
        float effectiveAo = lerp(1.0f, contactAo, indirectWeight);
        float3 coloredAo = float3(MultiBounceAO(min16float(effectiveAo), min16float3(albedo)));
        litColor = litColor * coloredAo * lerp(1.0f, contactShadow, 0.75f);

        // Diffuse bounce weighted by (1 - metallic) so pure chrome isn't washed out by diffuse GI
        float shadowFocus = rcp(1.0f + centerLuma * 0.90f);
        litColor += giBounce * albedo * (1.0f - metallic * 0.75f) *
                    (GiIntensity * (0.65f + 0.70f * shadowFocus)) * solidMask;
    }

    // Physically-Based Roughness-Cone SSR with Schlick Fresnel (no plastic glaze on rough/skin!)
    if (GiEnabled > 0 && SsrEnabled > 0 && SsrIntensity > 1e-4f && roughness < 0.85f)
    {
        float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
        f0 = lerp(f0, float3(0.028f, 0.028f, 0.028f), skinMask);
        float fresnel = pow(1.0f - NdotV, 5.0f);
        float3 specF = f0 + (1.0f - f0) * fresnel;
        float glossGate = pow(smoothness, 1.35f) * (1.0f - skinMask * 0.65f);
        float reflWeight = saturate(max(specF.r, max(specF.g, specF.b)) * glossGate * SsrIntensity * 1.15f) *
                           solidMask * (1.0f - haloRisk * 0.85f) * saturate(ssrSample.a);
        float3 reflTint = lerp(float3(1, 1, 1), albedo, metallic * 0.70f);
        litColor = lerp(litColor, ssrSample.rgb * reflTint, reflWeight);
    }

    // Scene-Coherent Directional Relighting + Charlie Cloth Sheen + Foliage Backlit Transmission
    if (RelightIntensity > 1e-4f)
    {
        float deltaBump = (dot(microNormal, keyDirView) - dot(viewNormal, keyDirView)) * 0.60f;
        float macroKey = (dot(viewNormal, keyDirView) - 0.15f) * 0.12f * keyStrength;
        float hemiTerm = dot(viewNormal, worldUpInView) * 0.12f;

        float sculpt = (deltaBump + macroKey + hemiTerm) * RelightIntensity * solidMask * (1.0f - haloRisk * 0.8f);
        float3 sculptTint = (sculpt >= 0.0f) ? keyTint : float3(0.88f, 0.94f, 1.08f);
        litColor *= max(1.0f + sculpt * sculptTint, 0.28f);

        // Rim scatter aligned with scene key & bounce
        float rim = pow(1.0f - NdotV, 3.5f) * RelightIntensity * solidMask * 0.16f * (1.0f - roughness * 0.45f);
        litColor += litColor * rim * keyTint * saturate(Luma(giBounce) * 1.4f + 0.35f);

        // Charlie velvet/cloth sheen at grazing angles
        if (clothMask > 0.05f)
        {
            float sheen = pow(1.0f - NdotV, 4.0f) * clothMask * RelightIntensity * solidMask * 0.14f;
            litColor += (albedo * 0.6f + keyTint * 0.4f) * sheen * max(centerLuma, 0.05f);
        }

        // Backlit foliage subsurface transmission
        if (foliageMask > 0.05f)
        {
            float backLight = pow(saturate(dot(viewDir, -keyDirView)), 3.0f);
            float trans = backLight * foliageMask * (RelightIntensity * 0.45f + SssIntensity * 0.35f) * solidMask;
            litColor += albedo * keyTint * trans * (0.25f + Luma(giBounce));
        }
    }

    // Specular Micro-Relief
    if (SpecularRelief > 1e-4f)
    {
        float microMag = length(microGrad) * meshGuard;
        float grazing = pow(1.0f - NdotV, 3.0f) * smoothness;
        float specSignal = 0.55f * grazing + 0.45f * saturate(microMag * 1.25f) * smoothness;
        float specMod = SpecularRelief * solidMask * (1.0f - haloRisk) * specSignal * 0.28f * (1.0f - skinMask * 0.35f);
        litColor *= (1.0f + specMod);
    }

    if (VolumetricGlow > 1e-4f)
        litColor += multiScaleGlow * (VolumetricGlow * 0.26f * (0.30f + 0.70f * (1.0f - flickerSoft)));

    // 3D Bilateral Grid Trilinear Slicing (Real Trained Neural Look Transform!)
    float3 finalColor = litColor;
    float3 bgDiag = float3(0.5f, 0.5f, 0.5f);

    if (NeuralDetail > 1e-4f || DebugView == 4)
    {
        float lumaZ = saturate(log2(1.0f + max(Luma(litColor), 0.0f)) * 0.25f);
        float3 gridUv = float3(uv, clamp(lumaZ * (7.0f / 8.0f) + (0.5f / 8.0f), 0.0625f, 0.9375f));

        float4 bg0 = BilateralGrid0Tex.SampleLevel(LinearClampBg0, gridUv, 0.0f); // gainDelta.rgb, shadowLift
        float4 bg1 = BilateralGrid1Tex.SampleLevel(LinearClampBg1, gridUv, 0.0f); // offset.rgb, detailGain
        float4 bg2 = BilateralGrid2Tex.SampleLevel(LinearClampBg2, gridUv, 0.0f); // chromaBoost, specBoost, hlRoll, conf

        int2 mvCoord = ClampMotionCoord(int2(MvSubrectBaseX, MvSubrectBaseY) +
                                        int2(uv * float2((DisplaySizeMV > 0) ? OutputWidth : RenderWidth,
                                                         (DisplaySizeMV > 0) ? OutputHeight : RenderHeight)));
        float2 mv = MotionTex.Load(int3(mvCoord, 0)).rg * float2(MvScaleX, MvScaleY);
        float motionDamp = rcp(1.0f + length(mv) * 0.04f);

        float strength = saturate(NeuralDetail * 0.55f) * solidMask * (1.0f - haloRisk * 0.7f);
        float rcasDamp = lerp(1.0f, 0.35f, saturate(FusedRcasSharpness));
        float highPass = (pC - pAvg) / max(pAvg + 0.16f, 0.16f);

        float3 gain = 1.0f + clamp(bg0.rgb, -0.30f, 0.35f) * strength;
        float shadowLift = clamp(bg0.a, -0.15f, 0.25f) * strength * rcp(1.0f + centerLuma * 2.5f);
        float detailBoost = clamp(bg1.a, -0.15f, 0.45f) * clamp(highPass, -0.35f, 0.35f) *
                            strength * motionDamp * meshGuard * rcasDamp * (1.0f - skinMask * 0.45f);
        float chromaBoost = clamp(bg2.x, -0.20f, 0.35f) * strength;

        finalColor = litColor * (gain + detailBoost + shadowLift) + clamp(bg1.rgb, -0.05f, 0.05f) * strength;
        float finalLuma = Luma(finalColor);
        finalColor = finalLuma + (finalColor - finalLuma) * (1.0f + chromaBoost);

        bgDiag = saturate(float3(bg0.r * 2.0f + 0.5f, bg1.a * 2.0f + 0.5f, bg2.x * 2.0f + 0.5f));
    }

    // Exposure-Normalized AgX Filmic Roll-Off
    if (ToneContrast > 1e-4f)
        finalColor = ApplyAgxFilmicContrast(finalColor, clamp(ToneContrast, 0.0f, 1.0f));

    // Fused 5-tap AMD RCAS sharpening using groupshared neighborhood
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
        finalColor *= clamp(rcasLuma / max(centerLuma, 1e-5f), 0.5f, 2.0f);
    }

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
        finalColor = float3(roughness, metallic, skinMask);
    else if (DebugView == 7)
        finalColor = float3(skinMask, contactShadow, foliageMask);
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

    if (DebugView == 0)
        finalColor *= preExp;

    finalColor = max(finalColor, 0.0f);
    DestColor[outCoord] = float4(EncodeColor(finalColor, OutColorEncodeMode), origAlpha);
}
