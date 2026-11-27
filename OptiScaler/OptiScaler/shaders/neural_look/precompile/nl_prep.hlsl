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

    int ColorSubrectBaseX;
    int ColorSubrectBaseY;
    int PreUpscale;
    int _padPre;

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

int2 GetMotionActiveSize()
{
    if (DisplaySizeMV > 0)
    {
        return (PreUpscale == 0)
                   ? int2(max(OutputWidth, 1), max(OutputHeight, 1))
                   : int2(max(MotionWidth - MvSubrectBaseX, 1), max(MotionHeight - MvSubrectBaseY, 1));
    }
    return int2(max(RenderWidth, 1), max(RenderHeight, 1));
}

int2 ClampMotionCoord(int2 p)
{
    int2 activeMv = GetMotionActiveSize();
    int2 minC = int2(MvSubrectBaseX, MvSubrectBaseY);
    int2 maxC = int2(min(MvSubrectBaseX + activeMv.x - 1, MotionWidth - 1),
                     min(MvSubrectBaseY + activeMv.y - 1, MotionHeight - 1));
    return clamp(p, minC, max(minC, maxC));
}

int2 UvToDepthCoord(float2 uv)
{
    float2 jitterUv = (PreUpscale > 0)
                          ? uv
                          : (uv - float2(JitterOffsetX, JitterOffsetY) / float2(max(RenderWidth, 1), max(RenderHeight, 1)));
    int2 c = int2(DepthSubrectBaseX, DepthSubrectBaseY) + int2(saturate(jitterUv) * float2(RenderWidth, RenderHeight));
    return ClampDepthCoord(c);
}

int2 HalfCoordToDepthCoord(int2 hc)
{
    if (PreUpscale > 0 && abs(RenderWidth - HalfWidth * 2) <= 1 && abs(RenderHeight - HalfHeight * 2) <= 1)
    {
        return ClampDepthCoord(int2(DepthSubrectBaseX, DepthSubrectBaseY) + hc * 2);
    }
    float2 invHalf = 1.0f / float2(max(HalfWidth, 1), max(HalfHeight, 1));
    return UvToDepthCoord((float2(hc) + 0.5f) * invHalf);
}

float2 DepthCoordToUv(int2 dCoord)
{
    float2 rel = float2(dCoord - int2(DepthSubrectBaseX, DepthSubrectBaseY)) + 0.5f;
    return rel / float2(max(RenderWidth, 1), max(RenderHeight, 1));
}

int2 UvToMotionCoord(float2 uv)
{
    int2 activeMv = GetMotionActiveSize();
    int2 c = int2(MvSubrectBaseX, MvSubrectBaseY) + int2(uv * float2(activeMv));
    return ClampMotionCoord(c);
}

float LoadLinearDepth(int2 coord)
{
    float rawDepth = DepthTex.Load(int3(ClampDepthCoord(coord), 0)).r;
    return LinearizeDepthValue(rawDepth, DepthIsLinear, DepthIsReversed,
                               DepthLinearA, DepthLinearB, DepthLinearC, SkyDepthThreshold);
}

float3 ReconstructViewNormalFromHalfDepths(int2 halfCoord, float dC, float dL, float dR, float dU, float dD,
                                           float dUL, float dUR, float dDL, float dDR)
{
    int2 cC  = ClampHalfCoord(halfCoord);
    int2 cL  = ClampHalfCoord(halfCoord + int2(-1, 0));
    int2 cR  = ClampHalfCoord(halfCoord + int2(1, 0));
    int2 cU  = ClampHalfCoord(halfCoord + int2(0, -1));
    int2 cD  = ClampHalfCoord(halfCoord + int2(0, 1));
    int2 cUL = ClampHalfCoord(halfCoord + int2(-1, -1));
    int2 cUR = ClampHalfCoord(halfCoord + int2(1, -1));
    int2 cDL = ClampHalfCoord(halfCoord + int2(-1, 1));
    int2 cDR = ClampHalfCoord(halfCoord + int2(1, 1));

    float2 uvC  = DepthCoordToUv(HalfCoordToDepthCoord(cC));
    float2 uvL  = DepthCoordToUv(HalfCoordToDepthCoord(cL));
    float2 uvR  = DepthCoordToUv(HalfCoordToDepthCoord(cR));
    float2 uvU  = DepthCoordToUv(HalfCoordToDepthCoord(cU));
    float2 uvD  = DepthCoordToUv(HalfCoordToDepthCoord(cD));
    float2 uvUL = DepthCoordToUv(HalfCoordToDepthCoord(cUL));
    float2 uvUR = DepthCoordToUv(HalfCoordToDepthCoord(cUR));
    float2 uvDL = DepthCoordToUv(HalfCoordToDepthCoord(cDL));
    float2 uvDR = DepthCoordToUv(HalfCoordToDepthCoord(cDR));

    float invDC = rcp(max(dC, 1e-4f));
    float3 qC  = ReconstructViewPos(uvC,  1.0f,        TanHalfFov, TanHalfFovAspect);
    float3 qL  = ReconstructViewPos(uvL,  dL  * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qR  = ReconstructViewPos(uvR,  dR  * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qU  = ReconstructViewPos(uvU,  dU  * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qD  = ReconstructViewPos(uvD,  dD  * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qUL = ReconstructViewPos(uvUL, dUL * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qUR = ReconstructViewPos(uvUR, dUR * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qDL = ReconstructViewPos(uvDL, dDL * invDC, TanHalfFov, TanHalfFovAspect);
    float3 qDR = ReconstructViewPos(uvDR, dDR * invDC, TanHalfFov, TanHalfFovAspect);

    float iC = invDC;
    float iL = rcp(max(dL, 1e-4f));
    float iR = rcp(max(dR, 1e-4f));
    float iU = rcp(max(dU, 1e-4f));
    float iD = rcp(max(dD, 1e-4f));

    bool hasL = halfCoord.x > 0;
    bool hasR = halfCoord.x < (HalfWidth - 1);
    bool hasU = halfCoord.y > 0;
    bool hasD = halfCoord.y < (HalfHeight - 1);

    float diL = iC - iL;
    float diR = iR - iC;
    float diffX = abs(diR - diL);
    float tolX = max(abs(diL) + abs(diR), iC * 0.008f) * 0.45f + iC * 0.004f;
    bool coplanarX = (hasL && hasR && dL < 64000.0f && dR < 64000.0f && diffX < tolX);

    float3 dx;
    if (coplanarX)
        dx = 0.5f * (qR - qL);
    else if (hasL && hasR)
        dx = (abs(diR) < abs(diL)) ? (qR - qC) : (qC - qL);
    else if (hasR)
        dx = qR - qC;
    else if (hasL)
        dx = qC - qL;
    else
        dx = float3(2.0f * TanHalfFovAspect / float(max(HalfWidth, 1)), 0.0f, 0.0f);

    float diU = iC - iU;
    float diD = iD - iC;
    float diffY = abs(diD - diU);
    float tolY = max(abs(diU) + abs(diD), iC * 0.008f) * 0.45f + iC * 0.004f;
    bool coplanarY = (hasU && hasD && dU < 64000.0f && dD < 64000.0f && diffY < tolY);

    float3 dy;
    if (coplanarY)
        dy = 0.5f * (qU - qD);
    else if (hasU && hasD)
        dy = (abs(diD) < abs(diU)) ? (qC - qD) : (qU - qC);
    else if (hasU)
        dy = qU - qC;
    else if (hasD)
        dy = qC - qD;
    else
        dy = float3(0.0f, 2.0f * TanHalfFov / float(max(HalfHeight, 1)), 0.0f);

    float3 n = cross(dx, dy);

    if (coplanarX && coplanarY && dUL < 64000.0f && dUR < 64000.0f && dDL < 64000.0f && dDR < 64000.0f)
    {
        float gX = 0.5f * (iR - iL);
        float gY = 0.5f * (iD - iU);
        float diagTol = max(tolX, tolY) * 1.5f;
        float eUL = abs(rcp(max(dUL, 1e-4f)) - (iC - gX - gY));
        float eUR = abs(rcp(max(dUR, 1e-4f)) - (iC + gX - gY));
        float eDL = abs(rcp(max(dDL, 1e-4f)) - (iC - gX + gY));
        float eDR = abs(rcp(max(dDR, 1e-4f)) - (iC + gX + gY));
        if (max(max(eUL, eUR), max(eDL, eDR)) < diagTol)
        {
            float3 nDiag = cross(0.5f * (qUR - qDL), 0.5f * (qUL - qDR));
            if (dot(nDiag, n) < 0.0f)
                nDiag = -nDiag;
            n += nDiag * 0.5f;
        }
    }

    float lenSq = dot(n, n);
    if (lenSq < 1e-12f)
        return float3(0.0f, 0.0f, -1.0f);

    n *= rsqrt(lenSq);
    if (dot(n, qC) > 0.0f)
        n = -n;
    if (n.z > -0.005f)
    {
        n.z = -0.005f;
        n = normalize(n);
    }
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
    if (dtid.x == 0 && dtid.y == 0)
    {
        [unroll]
        for (uint si = 0u; si < 32u; ++si)
            SceneLightBuf[si] = 0u;
    }

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
            s_depth[ly][lx] = LoadLinearDepth(HalfCoordToDepthCoord(hc));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if ((int) dtid.x >= HalfWidth || (int) dtid.y >= HalfHeight)
        return;

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

    int2 dCoord = HalfCoordToDepthCoord(halfCoord);
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
            int2 pCol = int2(ColorSubrectBaseX, ColorSubrectBaseY) + pOut;
            float3 cTap = ToLinearColor(SourceColor.Load(int3(pCol, 0)).rgb, InColorEncodeMode) * exposureNorm;
            float pDepth = (PreUpscale > 0)
                               ? LoadLinearDepth(int2(DepthSubrectBaseX, DepthSubrectBaseY) + pOut)
                               : LoadLinearDepth(UvToDepthCoord((float2(pOut) + 0.5f) * invOut));
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
    float2 mvRefSize = float2(GetMotionActiveSize());
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

    float3 viewNormal = ReconstructViewNormalFromHalfDepths(halfCoord, dC, dL, dR, dU, dD, dUL, dUR, dDL, dDR);
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
