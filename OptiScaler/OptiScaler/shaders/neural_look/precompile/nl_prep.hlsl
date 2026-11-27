#include "nl_common.hlsli"

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
    int HasGBufferNormals;
    int InColorEncodeMode;

    int HasReactiveMask;
    int HasTransparencyMask;
    int HasExposureTex;
    int HasCameraBasis;

    int WorldHashEnabled;
    int ResetHistory;
    int FrameIndex;
    int _pad0;

    float DepthLinearA;
    float DepthLinearB;
    float DepthLinearC;
    float SkyDepthThreshold;

    float TanHalfFov;
    float TanHalfFovAspect;
    float MvScaleX;
    float MvScaleY;

    float MotionTextureScale;
    float JitterOffsetX;
    float JitterOffsetY;
    float PreExposure;

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
[[vk::binding(6, 0)]]
#endif
Texture2D<float4> ReactiveTex : register(t5);

#ifdef VK_MODE
[[vk::binding(7, 0)]]
#endif
Texture2D<float4> TransparencyTex : register(t6);

#ifdef VK_MODE
[[vk::binding(8, 0)]]
#endif
Texture2D<float4> ExposureTex : register(t7);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]]
#endif
Texture2D<float4> PrevGiTex : register(t8);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(9, 0)]]
SamplerState LinearClampGi : register(s1);
#else
#define LinearClampGi LinearClamp
#endif

#ifdef VK_MODE
[[vk::binding(10, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutHalfColorDepth : register(u0);

#ifdef VK_MODE
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutHalfNormal : register(u1);

#ifdef VK_MODE
[[vk::binding(12, 0)]]
#endif
RWStructuredBuffer<uint> SceneLightBuf : register(u2);

#ifdef VK_MODE
[[vk::binding(13, 0)]]
#endif
RWStructuredBuffer<uint> WorldHashBuf : register(u3);

groupshared float s_depth[10][10];

int2 ClampOutputCoord(int2 p)
{
    return int2(clamp(p.x, 0, OutputWidth - 1), clamp(p.y, 0, OutputHeight - 1));
}

int2 ClampHalfCoord(int2 p)
{
    return int2(clamp(p.x, 0, HalfWidth - 1), clamp(p.y, 0, HalfHeight - 1));
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

int2 UvToMotionCoord(float2 uv)
{
    int activeW = (DisplaySizeMV > 0) ? OutputWidth : RenderWidth;
    int activeH = (DisplaySizeMV > 0) ? OutputHeight : RenderHeight;
    int2 c = int2(MvSubrectBaseX, MvSubrectBaseY) + int2(uv * float2(activeW, activeH));
    return ClampMotionCoord(c);
}

float LoadLinearDepth(int2 coord)
{
    float rawDepth = DepthTex.Load(int3(ClampDepthCoord(coord), 0)).r;
    return LinearizeDepthValue(rawDepth, DepthIsLinear, DepthIsReversed,
                               DepthLinearA, DepthLinearB, DepthLinearC, SkyDepthThreshold);
}

float3 ReconstructViewNormalFromHalfDepths(float2 uv, float dC, float dL, float dR, float dU, float dD)
{
    float2 invHalf = 1.0f / float2(max(HalfWidth, 1), max(HalfHeight, 1));

    float3 pC = ReconstructViewPos(uv, dC, TanHalfFov, TanHalfFovAspect);
    float3 pL = ReconstructViewPos(uv + float2(-invHalf.x, 0.0f), dL, TanHalfFov, TanHalfFovAspect);
    float3 pR = ReconstructViewPos(uv + float2(invHalf.x, 0.0f), dR, TanHalfFov, TanHalfFovAspect);
    float3 pU = ReconstructViewPos(uv + float2(0.0f, -invHalf.y), dU, TanHalfFov, TanHalfFovAspect);
    float3 pD = ReconstructViewPos(uv + float2(0.0f, invHalf.y), dD, TanHalfFov, TanHalfFovAspect);

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

float SignedSpike(float c, float a, float b)
{
    float da = a - c;
    float db = b - c;
    return (da * db > 0.0f) ? min(abs(da), abs(db)) : 0.0f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID,
            uint tid : SV_GroupIndex)
{
    // Clear SceneLightBuf (32 uints) for Pass 1 atomic reduction
    if (dtid.x == 0 && dtid.y == 0)
    {
        [unroll]
        for (uint si = 0u; si < 32u; ++si)
            SceneLightBuf[si] = 0u;
    }

    // Populate 10x10 LDS half-res depth tile (100 loads across 64 threads vs 9/pixel)
    int2 tileOrigin = int2(gid.xy) * 8 - 1;
    float2 invHalf = 1.0f / float2(max(HalfWidth, 1), max(HalfHeight, 1));
    [unroll]
    for (uint passIdx = 0u; passIdx < 2u; ++passIdx)
    {
        uint idx = tid + passIdx * 64u;
        if (idx < 100u)
        {
            int ly = int(idx / 10u);
            int lx = int(idx % 10u);
            int2 hc = ClampHalfCoord(tileOrigin + int2(lx, ly));
            float2 hUv = (float2(hc) + 0.5f) * invHalf;
            s_depth[ly][lx] = LoadLinearDepth(UvToDepthCoord(hUv));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if ((int) dtid.x >= HalfWidth || (int) dtid.y >= HalfHeight)
        return;

    // Clear WorldHashBuf on history reset
    if (ResetHistory > 0)
    {
        uint flatIdx = dtid.y * uint(HalfWidth) + dtid.x;
        if (flatIdx < 131072u)
        {
            WorldHashBuf[flatIdx * 2u] = 0u;
            WorldHashBuf[flatIdx * 2u + 1u] = 0u;
        }
    }

    int2 halfCoord = int2(dtid.xy);
    int lx = int(gtid.x) + 1;
    int ly = int(gtid.y) + 1;
    float2 uv = (float2(halfCoord) + 0.5f) * invHalf;
    float exposureNorm = rcp(max(PreExposure, 1e-4f));

    int2 dCoord = UvToDepthCoord(uv);
    float dC  = s_depth[ly][lx];
    float dL  = s_depth[ly][lx - 1];
    float dR  = s_depth[ly][lx + 1];
    float dU  = s_depth[ly - 1][lx];
    float dD  = s_depth[ly + 1][lx];
    float dUL = s_depth[ly - 1][lx - 1];
    float dUR = s_depth[ly - 1][lx + 1];
    float dDL = s_depth[ly + 1][lx - 1];
    float dDR = s_depth[ly + 1][lx + 1];

    float minD = min(dC, min(min(min(dL, dR), min(dU, dD)), min(min(dUL, dUR), min(dDL, dDR))));

    // Depth-guided 2x2 color downsample (prevents sky/background bleeding into foreground edges)
    int2 baseOut = halfCoord * 2;
    float2 invOut = 1.0f / float2(max(OutputWidth, 1), max(OutputHeight, 1));
    float3 linColor = float3(0.0f, 0.0f, 0.0f);
    float wColSum = 0.0f;

    [unroll]
    for (int oy = 0; oy < 2; ++oy)
    {
        [unroll]
        for (int ox = 0; ox < 2; ++ox)
        {
            int2 pOut = ClampOutputCoord(baseOut + int2(ox, oy));
            float3 cTap = ToLinearColor(SourceColor.Load(int3(pOut, 0)).rgb, InColorEncodeMode) * exposureNorm;
            float2 pUv = (float2(pOut) + 0.5f) * invOut;
            float pDepth = LoadLinearDepth(UvToDepthCoord(pUv));
            float w = exp2(-abs(pDepth - dC) / max(dC * 0.06f, 0.02f)) + 1e-3f;
            linColor += cTap * w;
            wColSum += w;
        }
    }
    linColor /= max(wColSum, 1e-4f);
    float luma = Luma(linColor);
    if (luma > 12.0f)
        linColor *= (12.0f + log2(1.0f + (luma - 12.0f))) / max(luma, 1e-4f);

    if (minD >= 64000.0f)
    {
        OutHalfColorDepth[halfCoord] = float4(linColor, 65000.0f);
        OutHalfNormal[halfCoord] = float4(0.0f, 0.0f, 0.0f, 65000.0f);
        return;
    }

    // Strict 2D collinear-triple stipple/dither detector (threshold tuned so dense grass is not killed)
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
    float spatialDither = smoothstep(0.018f, 0.065f, relSpike);

    int2 mvCoord = UvToMotionCoord(uv);
    float2 mv = MotionTex.Load(int3(mvCoord, 0)).rg * float2(MvScaleX, MvScaleY);
    float2 mvRefSize = (DisplaySizeMV > 0) ? float2(OutputWidth, OutputHeight)
                                           : float2(max(RenderWidth, 1), max(RenderHeight, 1));
    float2 mvUv = mv / mvRefSize;
    float mvMagPx = length(mvUv * float2(OutputWidth, OutputHeight));
    float2 prevUv = uv + mvUv;
    bool validHistory = (ResetHistory == 0 && FrameIndex > 2 && all(prevUv >= 0.002f) && all(prevUv <= 0.998f));

    float4 prevSample =
        validHistory ? PrevHalfNormalTex.SampleLevel(LinearClamp, prevUv, 0.0f) : float4(0.0f, 0.0f, 0.0f, dC);
    float prevRawD =
        validHistory
            ? PrevHalfNormalTex.Load(int3(ClampHalfCoord(int2(prevUv * float2(HalfWidth, HalfHeight))), 0)).w
            : dC;

    float depthDiff = min(abs(dC - prevRawD), abs(dC - prevSample.w));
    float relTempDiff = depthDiff / max(min(dC, min(prevRawD, prevSample.w)), 0.05f);
    float motionTrust = rcp(1.0f + mvMagPx * 0.15f);
    float temporalFlip = validHistory ? (smoothstep(0.012f, 0.050f, relTempDiff) * motionTrust) : 0.0f;

    float reactiveMask = 0.0f;
    if (HasReactiveMask > 0)
        reactiveMask = max(reactiveMask, saturate(ReactiveTex.Load(int3(dCoord, 0)).r));
    if (HasTransparencyMask > 0)
        reactiveMask = max(reactiveMask, saturate(TransparencyTex.Load(int3(dCoord, 0)).r) * 0.75f);

    float instantFlicker = max(max(spatialDither, temporalFlip), reactiveMask * 0.65f);
    float flickerEma = instantFlicker;
    if (validHistory)
    {
        float prevEma = saturate(prevSample.z);
        float decayAlpha = lerp(0.08f, 0.25f, saturate(mvMagPx / 8.0f));
        float alpha = (instantFlicker > prevEma) ? 0.50f : decayAlpha;
        flickerEma = lerp(prevEma, instantFlicker, alpha);
    }

    float safeDC = (dC >= 64000.0f && flickerEma > 0.25f) ? minD : dC;
    float outDepth = lerp(safeDC, minD, smoothstep(0.25f, 0.65f, flickerEma));
    OutHalfColorDepth[halfCoord] = float4(linColor, min(outDepth, 65000.0f));

    if (outDepth >= 64000.0f)
    {
        OutHalfNormal[halfCoord] = float4(0.0f, 0.0f, saturate(flickerEma), min(dC, 65000.0f));
        return;
    }

    float3 viewNormal = ReconstructViewNormalFromHalfDepths(uv, dC, dL, dR, dU, dD);
    if (HasGBufferNormals > 0)
    {
        float3 gN = GBufferNormalsTex.Load(int3(dCoord, 0)).xyz;
        if (all(gN >= 0.0f) && all(gN <= 1.0f))
            gN = gN * 2.0f - 1.0f;
        float gLen = dot(gN, gN);
        if (gLen > 0.1f && gLen < 4.0f)
        {
            float3 gNorm = gN * rsqrt(gLen);
            if (HasCameraBasis > 0)
            {
                float3 gView = WorldToViewDir(gNorm, CameraRight, CameraUp, CameraFwd);
                if (dot(gView, viewNormal) > 0.10f)
                    gNorm = gView;
            }
            if (dot(gNorm, viewNormal) > 0.15f)
                viewNormal = normalize(lerp(viewNormal, gNorm, 0.90f));
        }
    }

    if (validHistory && relTempDiff < 0.025f && flickerEma < 0.35f)
    {
        float2 pnxy = clamp(prevSample.xy, -1.0f, 1.0f);
        float3 prevNormal = float3(pnxy, -sqrt(max(0.0f, 1.0f - dot(pnxy, pnxy))));
        if (dot(prevNormal, viewNormal) > 0.45f)
        {
            float normAlpha = lerp(0.25f, 0.75f, saturate(mvMagPx / 8.0f));
            viewNormal = normalize(lerp(prevNormal, viewNormal, normAlpha));
        }
    }

    if (flickerEma > 0.0f)
        viewNormal = normalize(lerp(viewNormal, float3(0.0f, 0.0f, -1.0f), saturate(flickerEma) * 0.75f));

    OutHalfNormal[halfCoord] = float4(viewNormal.xy, saturate(flickerEma), min(dC, 65000.0f));

    // Phase 4: World-Space Spatial Hash Radiance Cache injection (4-slot probing, 1-in-4 staggered pixels)
    if (WorldHashEnabled > 0 && HasCameraBasis > 0 && ResetHistory == 0 && flickerEma < 0.45f &&
        (((halfCoord.x & 1) + (halfCoord.y & 1) * 2) == (FrameIndex & 3)))
    {
        float3 viewPos = ReconstructViewPos(uv, outDepth, TanHalfFov, TanHalfFovAspect);
        float3 worldPos = ViewToWorldPos(viewPos, CameraPos, CameraRight, CameraUp, CameraFwd);
        float3 worldNorm = ViewToWorldDir(viewNormal, CameraRight, CameraUp, CameraFwd);

        float3 prevGi = validHistory ? max(PrevGiTex.SampleLevel(LinearClampGi, prevUv, 0.0f).rgb, 0.0f)
                                     : float3(0.0f, 0.0f, 0.0f);
        float3 totalRad = linColor + prevGi * 0.55f;

        InjectWorldHash(WorldHashBuf, worldPos, worldNorm, outDepth, FrameIndex, totalRad);
    }
}
