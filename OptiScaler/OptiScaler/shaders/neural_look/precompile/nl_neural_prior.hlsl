#include "nl_common.hlsli"

#ifdef VK_MODE
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    int EighthWidth;
    int EighthHeight;
    int Mip3Width;
    int Mip3Height;

    int GridWidth;
    int GridHeight;
    int OutputWidth;
    int OutputHeight;

    int MotionWidth;
    int MotionHeight;
    int DisplaySizeMV;
    int LowResMV;

    int RenderWidth;
    int RenderHeight;
    int MvSubrectBaseX;
    int MvSubrectBaseY;

    int ResetHistory;
    int FrameIndex;
    int HasCameraBasis;
    int TotalTiles;

    float MvScaleX;
    float MvScaleY;
    float SkyDepthThreshold;
    float _pad0;

    float3 CameraRight;
    float _padCam0;
    float3 CameraUp;
    float _padCam1;
    float3 CameraFwd;
    float _padCam2;
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> PyramidMip1Tex : register(t0);

#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float4> PyramidMip3Tex : register(t1);

#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
Texture2D<float4> EighthNormalTex : register(t2);

#ifdef VK_MODE
[[vk::binding(4, 0)]]
#endif
Texture2D<float4> EighthFeaturesTex : register(t3);

#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
Texture2D<float2> MotionTex : register(t4);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]]
#endif
Texture2D<float4> PrevMaterial0Tex : register(t5);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
#endif
Texture2D<float4> PrevMaterial1Tex : register(t6);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]]
#endif
Texture2D<float4> PrevMaterial2Tex : register(t7);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]]
SamplerState LinearClamp0 : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
SamplerState LinearClamp1 : register(s1);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]]
SamplerState LinearClamp2 : register(s2);
#else
SamplerState LinearClamp : register(s0);
#define LinearClamp0 LinearClamp
#define LinearClamp1 LinearClamp
#define LinearClamp2 LinearClamp
#endif

#ifdef VK_MODE
[[vk::binding(9, 0)]]
#endif
Texture3D<float4> PrevBilateralGrid0Tex : register(t8);

#ifdef VK_MODE
[[vk::binding(10, 0)]]
#endif
Texture3D<float4> PrevBilateralGrid1Tex : register(t9);

#ifdef VK_MODE
[[vk::binding(11, 0)]]
#endif
Texture3D<float4> PrevBilateralGrid2Tex : register(t10);

#ifdef VK_MODE
[[vk::binding(12, 0)]]
#endif
StructuredBuffer<uint> SceneLightBuf : register(t11);

#ifdef VK_MODE
[[vk::binding(13, 0)]]
#endif
StructuredBuffer<float4> ModelWeightsBuf : register(t12);

#ifdef VK_MODE
[[vk::binding(14, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutMaterial0 : register(u0);

#ifdef VK_MODE
[[vk::binding(15, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutMaterial1 : register(u1);

#ifdef VK_MODE
[[vk::binding(16, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutMaterial2 : register(u2);

#ifdef VK_MODE
[[vk::binding(17, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture3D<float4> OutBilateralGrid0 : register(u3);

#ifdef VK_MODE
[[vk::binding(18, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture3D<float4> OutBilateralGrid1 : register(u4);

#ifdef VK_MODE
[[vk::binding(19, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture3D<float4> OutBilateralGrid2 : register(u5);

#ifdef VK_MODE
[[vk::binding(20, 0)]]
#endif
RWStructuredBuffer<float4> ResolvedSceneLightBuf : register(u6);

// Exact float4 layer offsets matching tools/train_neural_look.py & NL_Model.h (v2: 3143 float4s)
static const uint OFF_W_STEM = 0u;      // 160 (32 x 20)
static const uint OFF_B_STEM = 160u;    // 8
static const uint OFF_W_DW1  = 168u;    // 72
static const uint OFF_B_DW1  = 240u;    // 8
static const uint OFF_W_PW1A = 248u;    // 512
static const uint OFF_B_PW1A = 760u;    // 16
static const uint OFF_W_PW1B = 776u;    // 512
static const uint OFF_B_PW1B = 1288u;   // 8
static const uint OFF_W_DW2  = 1296u;   // 72
static const uint OFF_B_DW2  = 1368u;   // 8
static const uint OFF_W_PW2A = 1376u;   // 640
static const uint OFF_B_PW2A = 2016u;   // 16
static const uint OFF_W_PW2B = 2032u;   // 512
static const uint OFF_B_PW2B = 2544u;   // 8
static const uint OFF_W_MAT  = 2552u;   // 128 (16 x 32)
static const uint OFF_B_MAT  = 2680u;   // 4
static const uint OFF_W_BG1  = 2684u;   // 352
static const uint OFF_B_BG1  = 3036u;   // 8
static const uint OFF_W_BG2  = 3044u;   // 96
static const uint OFF_B_BG2  = 3140u;   // 3

// 12x12 tile (2-px halo for zero-seam DW1+DW2) + 10x10 h1 tile = ~25.9 KB LDS < 32 KB limit
groupshared uint s_feat[12][12][16];
groupshared uint s_h1[10][10][16];
groupshared float4 s_globalPool[64][2];
groupshared float4 s_h2Pool[64][8];
groupshared float4 s_sceneKeyView;
groupshared float4 s_worldUpView;

min16float4 SiLU4H(min16float4 x)
{
    return x * rcp(min16float(1.0f) + exp(-clamp(x, min16float(-14.0f), min16float(14.0f))));
}

void PackFeat32(int ly, int lx, min16float4 v[8])
{
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float4 fv = float4(v[i]);
        s_feat[ly][lx][i * 2 + 0] = f32tof16(fv.x) | (f32tof16(fv.y) << 16);
        s_feat[ly][lx][i * 2 + 1] = f32tof16(fv.z) | (f32tof16(fv.w) << 16);
    }
}

void UnpackFeat32(int ly, int lx, out min16float4 v[8])
{
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        uint p0 = s_feat[ly][lx][i * 2 + 0];
        uint p1 = s_feat[ly][lx][i * 2 + 1];
        v[i] = min16float4(f16tof32(p0 & 0xFFFFu), f16tof32(p0 >> 16),
                           f16tof32(p1 & 0xFFFFu), f16tof32(p1 >> 16));
    }
}

void PackH1_32(int ly, int lx, min16float4 v[8])
{
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float4 fv = float4(v[i]);
        s_h1[ly][lx][i * 2 + 0] = f32tof16(fv.x) | (f32tof16(fv.y) << 16);
        s_h1[ly][lx][i * 2 + 1] = f32tof16(fv.z) | (f32tof16(fv.w) << 16);
    }
}

void UnpackH1_32(int ly, int lx, out min16float4 v[8])
{
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        uint p0 = s_h1[ly][lx][i * 2 + 0];
        uint p1 = s_h1[ly][lx][i * 2 + 1];
        v[i] = min16float4(f16tof32(p0 & 0xFFFFu), f16tof32(p0 >> 16),
                           f16tof32(p1 & 0xFFFFu), f16tof32(p1 >> 16));
    }
}

void ResolveCurrentSceneLight(out float4 outSh0, out float4 outShX, out float4 outShY, out float4 outShZ,
                              out float4 outKeyDir, out float4 outKeyCol, out float4 outWorldUp)
{
    float count = max(float(SceneLightBuf[4]), 1.0f);
    float invCount = rcp(count * 16.0f);

    float3 sh0Rgb = float3(SceneLightBuf[0], SceneLightBuf[1], SceneLightBuf[2]) * invCount;
    float avgLuma = float(SceneLightBuf[3]) * invCount;
    float skyFrac = saturate(float(SceneLightBuf[5]) / count);

    float3 shXRgb = float3(asint(SceneLightBuf[6]),  asint(SceneLightBuf[7]),  asint(SceneLightBuf[8]))  * invCount;
    float3 shYRgb = float3(asint(SceneLightBuf[9]),  asint(SceneLightBuf[10]), asint(SceneLightBuf[11])) * invCount;
    float3 shZRgb = float3(asint(SceneLightBuf[12]), asint(SceneLightBuf[13]), asint(SceneLightBuf[14])) * invCount;

    float3 rawKeyDir = float3(asint(SceneLightBuf[15]), asint(SceneLightBuf[16]), asint(SceneLightBuf[17])) / 16.0f;
    float keyW = float(SceneLightBuf[18]) / 16.0f;
    float3 rawKeyCol = float3(SceneLightBuf[19], SceneLightBuf[20], SceneLightBuf[21]) / 16.0f;

    float3 keyDir;
    float keyStrength;
    float3 keyCol;
    float kLen = length(rawKeyDir);
    if (keyW > 0.1f && kLen > 1e-3f)
    {
        keyDir = rawKeyDir / kLen;
        keyStrength = saturate(kLen / max(keyW, 1e-4f));
        keyCol = rawKeyCol / max(keyW, 1e-4f);
    }
    else
    {
        keyDir = normalize(float3(0.25f, 0.85f, -0.45f));
        keyStrength = 0.25f;
        keyCol = max(sh0Rgb, float3(0.15f, 0.15f, 0.15f));
    }

    float3 keyDirView = (HasCameraBasis > 0) ? WorldToViewDir(keyDir, CameraRight, CameraUp, CameraFwd) : keyDir;
    float3 worldUpInView = (HasCameraBasis > 0)
                               ? WorldToViewDir(float3(0.0f, 1.0f, 0.0f), CameraRight, CameraUp, CameraFwd)
                               : float3(0.0f, 1.0f, 0.0f);

    outSh0 = float4(sh0Rgb, avgLuma);
    outShX = float4(shXRgb, 0.0f);
    outShY = float4(shYRgb, 0.0f);
    outShZ = float4(shZRgb, 0.0f);
    outKeyDir = float4(keyDirView, keyStrength);
    outKeyCol = float4(keyCol, skyFrac);
    outWorldUp = float4(worldUpInView, HasCameraBasis > 0 ? 1.0f : 0.0f);
}

int2 Clamp8(int2 p)
{
    return clamp(p, int2(0, 0), int2(EighthWidth - 1, EighthHeight - 1));
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

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID,
            uint tid : SV_GroupIndex)
{
    if (tid == 0)
    {
        float4 sh0, shX, shY, shZ, kDir, kCol, wUp;
        ResolveCurrentSceneLight(sh0, shX, shY, shZ, kDir, kCol, wUp);
        s_sceneKeyView = kDir;
        s_worldUpView = wUp;

        if (gid.x == 0 && gid.y == 0)
        {
            if (ResetHistory > 0 || FrameIndex <= 2)
            {
                ResolvedSceneLightBuf[0] = sh0;
                ResolvedSceneLightBuf[1] = shX;
                ResolvedSceneLightBuf[2] = shY;
                ResolvedSceneLightBuf[3] = shZ;
                ResolvedSceneLightBuf[4] = kDir;
                ResolvedSceneLightBuf[5] = kCol;
                ResolvedSceneLightBuf[6] = wUp;
                ResolvedSceneLightBuf[7] = float4(float(FrameIndex), 0.0f, 0.0f, 0.0f);
            }
            else
            {
                const float alpha = 0.08f;
                float4 prevKDir = ResolvedSceneLightBuf[4];
                float3 smoothDir = (dot(prevKDir.xyz, prevKDir.xyz) < 0.1f)
                                       ? kDir.xyz
                                       : normalize(lerp(prevKDir.xyz, kDir.xyz, alpha));

                ResolvedSceneLightBuf[0] = lerp(ResolvedSceneLightBuf[0], sh0, alpha);
                ResolvedSceneLightBuf[1] = lerp(ResolvedSceneLightBuf[1], shX, alpha);
                ResolvedSceneLightBuf[2] = lerp(ResolvedSceneLightBuf[2], shY, alpha);
                ResolvedSceneLightBuf[3] = lerp(ResolvedSceneLightBuf[3], shZ, alpha);
                ResolvedSceneLightBuf[4] = float4(smoothDir, lerp(prevKDir.w, kDir.w, alpha));
                ResolvedSceneLightBuf[5] = lerp(ResolvedSceneLightBuf[5], kCol, alpha);
                ResolvedSceneLightBuf[6] = wUp;
                ResolvedSceneLightBuf[7] = float4(float(FrameIndex), 0.0f, 0.0f, 0.0f);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    float3 keyDirView = s_sceneKeyView.xyz;
    float3 worldUpInView = s_worldUpView.xyz;
    int2 tileOrigin12 = int2(gid.xy) * 8 - 2;

    // Step A: Load 12x12 tile (covering 8x8 interior + 2px halo for zero-seam DW1+DW2), run 20-ch Stem -> s_feat
    [unroll]
    for (uint passIdx = 0u; passIdx < 3u; ++passIdx)
    {
        uint idx = tid + passIdx * 64u;
        if (idx < 144u)
        {
            int ly = int(idx / 12u);
            int lx = int(idx % 12u);
            int2 p8 = Clamp8(tileOrigin12 + int2(lx, ly));
            float2 uv8 = (float2(p8) + 0.5f) / float2(max(EighthWidth, 1), max(EighthHeight, 1));

            float4 cd8 = PyramidMip1Tex.Load(int3(p8, 0));
            float4 n8 = EighthNormalTex.Load(int3(p8, 0));
            float4 f8 = EighthFeaturesTex.Load(int3(p8, 0));
            int2 p32 = clamp(int2(uv8 * float2(Mip3Width, Mip3Height)), int2(0, 0),
                             int2(max(Mip3Width - 1, 0), max(Mip3Height - 1, 0)));
            float3 coarseRgb = PyramidMip3Tex.Load(int3(p32, 0)).rgb;

            float3 rgb = max(cd8.rgb, 0.0f);
            float luma = Luma(rgb);
            float coarseLuma = Luma(max(coarseRgb, 0.0f));
            float invLuma = rcp(max(luma, 0.02f));
            float2 nxy = clamp(n8.xy, -1.0f, 1.0f);
            float nz = -sqrt(max(0.0f, 1.0f - dot(nxy, nxy)));
            float3 vNorm = float3(nxy, nz);
            float skyMask = (cd8.a < 64000.0f && cd8.a < SkyDepthThreshold) ? 1.0f : 0.0f;
            float ndotl = saturate(dot(vNorm, keyDirView) * 0.5f + 0.5f);
            float worldUpDot = clamp(dot(vNorm, worldUpInView), -1.0f, 1.0f);
            float rgbSum = max(rgb.r + rgb.g + rgb.b, 0.04f);

            float4 in0 = float4(log2(1.0f + luma) * 0.25f,
                                clamp(rgb.r * invLuma, 0.0f, 3.0f),
                                clamp(rgb.g * invLuma, 0.0f, 3.0f),
                                clamp(rgb.b * invLuma, 0.0f, 3.0f));
            float4 in1 = float4(saturate(log2(1.0f + min(cd8.a, 1024.0f)) * 0.1f),
                                nxy.x, nxy.y, abs(nz));
            float4 in2 = float4(saturate(n8.w * 4.0f),
                                skyMask,
                                clamp(luma / max(coarseLuma + 0.05f, 0.05f) - 1.0f, -1.0f, 2.0f),
                                ndotl);
            float4 in3 = float4(worldUpDot, f8.x, f8.y, f8.z);
            float4 in4 = float4(f8.w,
                                clamp((rgb.r - rgb.g) / rgbSum, -1.0f, 1.0f),
                                clamp((rgb.r + rgb.g - 2.0f * rgb.b) / rgbSum, -1.5f, 1.5f),
                                uv8.y * 2.0f - 1.0f);

            min16float4 h0[8];
            [unroll]
            for (int og = 0; og < 8; ++og)
            {
                float4 acc = ModelWeightsBuf[OFF_B_STEM + og];
                uint baseW = OFF_W_STEM + uint(og) * 20u;
                [unroll]
                for (int c = 0; c < 4; ++c)
                {
                    uint wIdx = baseW + uint(c) * 5u;
                    float s = dot(ModelWeightsBuf[wIdx + 0u], in0)
                            + dot(ModelWeightsBuf[wIdx + 1u], in1)
                            + dot(ModelWeightsBuf[wIdx + 2u], in2)
                            + dot(ModelWeightsBuf[wIdx + 3u], in3)
                            + dot(ModelWeightsBuf[wIdx + 4u], in4);
                    if (c == 0) acc.x += s;
                    else if (c == 1) acc.y += s;
                    else if (c == 2) acc.z += s;
                    else acc.w += s;
                }
                h0[og] = SiLU4H(min16float4(acc));
            }
            PackFeat32(ly, lx, h0);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Step B: Block 1 on 10x10 tile (interior 8x8 + 1px halo for zero-seam Block 2 DW2) -> s_h1[10][10]
    [unroll]
    for (uint passB = 0u; passB < 2u; ++passB)
    {
        uint idx = tid + passB * 64u;
        if (idx < 100u)
        {
            int by = int(idx / 10u);
            int bx = int(idx % 10u);
            int fy = by + 1;
            int fx = bx + 1;

            min16float4 h0Center[8];
            UnpackFeat32(fy, fx, h0Center);

            min16float4 dw1[8];
            [unroll]
            for (int og = 0; og < 8; ++og)
                dw1[og] = min16float4(ModelWeightsBuf[OFF_B_DW1 + og]);

            [unroll]
            for (int dy = -1; dy <= 1; ++dy)
            {
                [unroll]
                for (int dx = -1; dx <= 1; ++dx)
                {
                    uint tap = uint((dy + 1) * 3 + (dx + 1));
                    min16float4 nVec[8];
                    UnpackFeat32(fy + dy, fx + dx, nVec);
                    [unroll]
                    for (int og = 0; og < 8; ++og)
                        dw1[og] += nVec[og] * min16float4(ModelWeightsBuf[OFF_W_DW1 + tap * 8u + og]);
                }
            }

            min16float4 pw1a[16];
            [unroll]
            for (int og = 0; og < 16; ++og)
            {
                min16float4 acc = min16float4(ModelWeightsBuf[OFF_B_PW1A + og]);
                uint baseW = OFF_W_PW1A + uint(og) * 32u;
                [unroll]
                for (int c = 0; c < 4; ++c)
                {
                    uint wIdx = baseW + uint(c) * 8u;
                    min16float s = min16float(0.0f);
                    [unroll]
                    for (int k = 0; k < 8; ++k)
                        s += dot(min16float4(ModelWeightsBuf[wIdx + k]), dw1[k]);
                    if (c == 0) acc.x += s;
                    else if (c == 1) acc.y += s;
                    else if (c == 2) acc.z += s;
                    else acc.w += s;
                }
                pw1a[og] = SiLU4H(acc);
            }

            min16float4 h1[8];
            [unroll]
            for (int og = 0; og < 8; ++og)
            {
                min16float4 acc = min16float4(ModelWeightsBuf[OFF_B_PW1B + og]) + h0Center[og];
                uint baseW = OFF_W_PW1B + uint(og) * 64u;
                [unroll]
                for (int c = 0; c < 4; ++c)
                {
                    uint wIdx = baseW + uint(c) * 16u;
                    min16float s = min16float(0.0f);
                    [unroll]
                    for (int k = 0; k < 16; ++k)
                        s += dot(min16float4(ModelWeightsBuf[wIdx + k]), pw1a[k]);
                    if (c == 0) acc.x += s;
                    else if (c == 1) acc.y += s;
                    else if (c == 2) acc.z += s;
                    else acc.w += s;
                }
                h1[og] = acc;
            }

            PackH1_32(by, bx, h1);
            if (by >= 1 && by <= 8 && bx >= 1 && bx <= 8)
            {
                uint innerTid = uint((by - 1) * 8 + (bx - 1));
                s_globalPool[innerTid][0] = float4(h1[0]);
                s_globalPool[innerTid][1] = float4(h1[1]);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Parallel reduce g[2] across 8x8 interior
    [unroll]
    for (uint s = 32u; s > 0u; s >>= 1u)
    {
        if (tid < s)
        {
            s_globalPool[tid][0] += s_globalPool[tid + s][0];
            s_globalPool[tid][1] += s_globalPool[tid + s][1];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    min16float4 g0 = min16float4(s_globalPool[0][0] / 64.0f);
    min16float4 g1 = min16float4(s_globalPool[0][1] / 64.0f);

    // Step C: Block 2 on 8x8 interior (exact 3x3 DW2 with 1px halo in s_h1, ZERO tile edge clamping!)
    int h1y = int(gtid.y) + 1;
    int h1x = int(gtid.x) + 1;
    min16float4 h1Center[8];
    UnpackH1_32(h1y, h1x, h1Center);

    min16float4 dw2[8];
    [unroll]
    for (int og = 0; og < 8; ++og)
        dw2[og] = min16float4(ModelWeightsBuf[OFF_B_DW2 + og]);

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            uint tap = uint((dy + 1) * 3 + (dx + 1));
            min16float4 nVec[8];
            UnpackH1_32(h1y + dy, h1x + dx, nVec);
            [unroll]
            for (int og = 0; og < 8; ++og)
                dw2[og] += nVec[og] * min16float4(ModelWeightsBuf[OFF_W_DW2 + tap * 8u + og]);
        }
    }

    min16float4 pw2a[16];
    [unroll]
    for (int og = 0; og < 16; ++og)
    {
        min16float4 acc = min16float4(ModelWeightsBuf[OFF_B_PW2A + og]);
        uint baseW = OFF_W_PW2A + uint(og) * 40u;
        [unroll]
        for (int c = 0; c < 4; ++c)
        {
            uint wIdx = baseW + uint(c) * 10u;
            min16float s = dot(min16float4(ModelWeightsBuf[wIdx + 8u]), g0)
                         + dot(min16float4(ModelWeightsBuf[wIdx + 9u]), g1);
            [unroll]
            for (int k = 0; k < 8; ++k)
                s += dot(min16float4(ModelWeightsBuf[wIdx + k]), dw2[k]);
            if (c == 0) acc.x += s;
            else if (c == 1) acc.y += s;
            else if (c == 2) acc.z += s;
            else acc.w += s;
        }
        pw2a[og] = SiLU4H(acc);
    }

    float4 h2[8];
    [unroll]
    for (int og = 0; og < 8; ++og)
    {
        min16float4 acc = min16float4(ModelWeightsBuf[OFF_B_PW2B + og]) + h1Center[og];
        uint baseW = OFF_W_PW2B + uint(og) * 64u;
        [unroll]
        for (int c = 0; c < 4; ++c)
        {
            uint wIdx = baseW + uint(c) * 16u;
            min16float s = min16float(0.0f);
            [unroll]
            for (int k = 0; k < 16; ++k)
                s += dot(min16float4(ModelWeightsBuf[wIdx + k]), pw2a[k]);
            if (c == 0) acc.x += s;
            else if (c == 1) acc.y += s;
            else if (c == 2) acc.z += s;
            else acc.w += s;
        }
        h2[og] = float4(acc);
        s_h2Pool[tid][og] = h2[og];
    }

    // Step D: Head_Material (32 -> 16: 7 continuous sigmoid + 6 semantic softmax) + temporal stabilization
    int2 coord8 = int2(dtid.xy);
    if (coord8.x < EighthWidth && coord8.y < EighthHeight)
    {
        float4 logits[4];
        [unroll]
        for (int og = 0; og < 4; ++og)
        {
            float4 acc = ModelWeightsBuf[OFF_B_MAT + og];
            uint baseW = OFF_W_MAT + uint(og) * 32u;
            [unroll]
            for (int c = 0; c < 4; ++c)
            {
                uint wIdx = baseW + uint(c) * 8u;
                float s = 0.0f;
                [unroll]
                for (int k = 0; k < 8; ++k)
                    s += dot(ModelWeightsBuf[wIdx + k], h2[k]);
                if (c == 0) acc.x += s;
                else if (c == 1) acc.y += s;
                else if (c == 2) acc.z += s;
                else acc.w += s;
            }
            logits[og] = clamp(acc, -16.0f, 16.0f);
        }

        float4 sig0 = rcp(1.0f + exp(-logits[0]));
        float4 sig1 = rcp(1.0f + exp(-logits[1]));

        // 6-class semantic softmax over channels 7..12: logits[1].w, logits[2].x, y, z, w, logits[3].x
        float s0 = logits[1].w; // skin
        float s1 = logits[2].x; // fabric
        float s2 = logits[2].y; // sand
        float s3 = logits[2].z; // soil
        float s4 = logits[2].w; // grass
        float s5 = logits[3].x; // hard_surface
        float sMax = max(max(max(s0, s1), max(s2, s3)), max(s4, s5));
        float e0 = exp(s0 - sMax);
        float e1 = exp(s1 - sMax);
        float e2 = exp(s2 - sMax);
        float e3 = exp(s3 - sMax);
        float e4 = exp(s4 - sMax);
        float e5 = exp(s5 - sMax);
        float invSum = rcp(max(e0 + e1 + e2 + e3 + e4 + e5, 1e-6f));

        float pSkin   = e0 * invSum;
        float pFabric = e1 * invSum;
        float pSand   = e2 * invSum;
        float pSoil   = e3 * invSum;
        float pGrass  = e4 * invSum;

        float4 mat0 = sig0;                                       // albedo.rgb, directMask
        float4 mat1 = float4(sig1.x, sig1.y, sig1.z, pSkin);      // roughness, metallic, wetness, skinProb
        float4 mat2 = float4(pFabric, pSand, pSoil, pGrass);      // fabricProb, sandProb, soilProb, grassProb

        float2 uv = (float2(coord8) + 0.5f) / float2(max(EighthWidth, 1), max(EighthHeight, 1));
        int activeW = (DisplaySizeMV > 0) ? OutputWidth : RenderWidth;
        int activeH = (DisplaySizeMV > 0) ? OutputHeight : RenderHeight;
        int2 mvCoord = ClampMotionCoord(int2(MvSubrectBaseX, MvSubrectBaseY) + int2(uv * float2(activeW, activeH)));
        float2 mv = MotionTex.Load(int3(mvCoord, 0)).rg * float2(MvScaleX, MvScaleY);
        float2 prevUv = uv + mv / float2(max(activeW, 1), max(activeH, 1));

        if (ResetHistory == 0 && FrameIndex > 3 && all(prevUv >= 0.002f) && all(prevUv <= 0.998f))
        {
            float4 prevM0 = PrevMaterial0Tex.SampleLevel(LinearClamp0, prevUv, 0.0f);
            float4 prevM1 = PrevMaterial1Tex.SampleLevel(LinearClamp1, prevUv, 0.0f);
            float4 prevM2 = PrevMaterial2Tex.SampleLevel(LinearClamp2, prevUv, 0.0f);
            float mvPx = length((prevUv - uv) * float2(OutputWidth, OutputHeight));
            float diff = max(length(mat0.rgb - prevM0.rgb), max(abs(mat1.w - prevM1.w), length(mat2 - prevM2) * 0.5f));
            float alphaCont = saturate(lerp(0.22f, 0.65f, saturate(mvPx / 12.0f)) + smoothstep(0.10f, 0.35f, diff) * 0.50f);
            float alphaSem = saturate(lerp(0.10f, 0.45f, saturate(mvPx / 12.0f)) + smoothstep(0.12f, 0.40f, diff) * 0.55f);

            mat0 = lerp(prevM0, mat0, alphaCont);
            mat1.xyz = lerp(prevM1.xyz, mat1.xyz, alphaCont);
            mat1.w = lerp(prevM1.w, mat1.w, alphaSem);
            mat2 = lerp(prevM2, mat2, alphaSem);

            // Re-normalize semantic probabilities if sum exceeds 1.0
            float semSum = mat1.w + mat2.x + mat2.y + mat2.z + mat2.w;
            if (semSum > 1.0f)
            {
                float invS = rcp(semSum);
                mat1.w *= invS;
                mat2 *= invS;
            }
        }

        OutMaterial0[coord8] = mat0;
        OutMaterial1[coord8] = mat1;
        OutMaterial2[coord8] = mat2;
    }

    GroupMemoryBarrierWithGroupSync();

    // Step E: Pool h2[8] across 8x8 tile -> threads 0..7 emit Bilateral Grid + temporal smoothing!
    [unroll]
    for (uint s = 32u; s > 0u; s >>= 1u)
    {
        if (tid < s)
        {
            [unroll]
            for (int k = 0; k < 8; ++k)
                s_h2Pool[tid][k] += s_h2Pool[tid + s][k];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid < 8u && int(gid.x) < GridWidth && int(gid.y) < GridHeight)
    {
        min16float4 h2p[8];
        [unroll]
        for (int k = 0; k < 8; ++k)
            h2p[k] = min16float4(s_h2Pool[0][k] / 64.0f);

        float lz = float(tid) / 7.0f;
        min16float4 lumaEmbed = min16float4(lz, 2.0f * lz - 1.0f, sin(kPi * lz), cos(kPi * lz));

        min16float4 bg1[8];
        [unroll]
        for (int og = 0; og < 8; ++og)
        {
            min16float4 acc = min16float4(ModelWeightsBuf[OFF_B_BG1 + og]);
            uint baseW = OFF_W_BG1 + uint(og) * 44u;
            [unroll]
            for (int c = 0; c < 4; ++c)
            {
                uint wIdx = baseW + uint(c) * 11u;
                min16float s = dot(min16float4(ModelWeightsBuf[wIdx + 8u]), g0)
                             + dot(min16float4(ModelWeightsBuf[wIdx + 9u]), g1)
                             + dot(min16float4(ModelWeightsBuf[wIdx + 10u]), lumaEmbed);
                [unroll]
                for (int k = 0; k < 8; ++k)
                    s += dot(min16float4(ModelWeightsBuf[wIdx + k]), h2p[k]);
                if (c == 0) acc.x += s;
                else if (c == 1) acc.y += s;
                else if (c == 2) acc.z += s;
                else acc.w += s;
            }
            bg1[og] = SiLU4H(acc);
        }

        float4 bgOut[3];
        [unroll]
        for (int og = 0; og < 3; ++og)
        {
            float4 acc = ModelWeightsBuf[OFF_B_BG2 + og];
            uint baseW = OFF_W_BG2 + uint(og) * 32u;
            [unroll]
            for (int c = 0; c < 4; ++c)
            {
                uint wIdx = baseW + uint(c) * 8u;
                min16float s = min16float(0.0f);
                [unroll]
                for (int k = 0; k < 8; ++k)
                    s += dot(min16float4(ModelWeightsBuf[wIdx + k]), bg1[k]);
                if (c == 0) acc.x += float(s);
                else if (c == 1) acc.y += float(s);
                else if (c == 2) acc.z += float(s);
                else acc.w += float(s);
            }
            bgOut[og] = acc;
        }

        uint3 gridCoord = uint3(gid.x, gid.y, tid);
        if (ResetHistory == 0 && FrameIndex > 3)
        {
            float4 pBg0 = PrevBilateralGrid0Tex.Load(int4(gridCoord, 0));
            float4 pBg1 = PrevBilateralGrid1Tex.Load(int4(gridCoord, 0));
            float4 pBg2 = PrevBilateralGrid2Tex.Load(int4(gridCoord, 0));
            bgOut[0] = lerp(pBg0, bgOut[0], 0.14f);
            bgOut[1] = lerp(pBg1, bgOut[1], 0.14f);
            bgOut[2] = lerp(pBg2, bgOut[2], 0.14f);
        }

        OutBilateralGrid0[gridCoord] = bgOut[0];
        OutBilateralGrid1[gridCoord] = bgOut[1];
        OutBilateralGrid2[gridCoord] = bgOut[2];
    }
}
