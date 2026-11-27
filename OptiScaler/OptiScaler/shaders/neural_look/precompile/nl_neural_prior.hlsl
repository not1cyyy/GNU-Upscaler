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
Texture2D<float2> MotionTex : register(t3);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]]
#endif
Texture2D<float4> PrevMaterial0Tex : register(t4);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]]
#endif
Texture2D<float4> PrevMaterial1Tex : register(t5);

#ifdef VK_MODE
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]]
SamplerState LinearClamp0 : register(s0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]]
SamplerState LinearClamp1 : register(s1);
#else
SamplerState LinearClamp : register(s0);
#define LinearClamp0 LinearClamp
#define LinearClamp1 LinearClamp
#endif

#ifdef VK_MODE
[[vk::binding(7, 0)]]
#endif
StructuredBuffer<uint> SceneLightBuf : register(t6);

#ifdef VK_MODE
[[vk::binding(8, 0)]]
#endif
StructuredBuffer<float4> ModelWeightsBuf : register(t7);

#ifdef VK_MODE
[[vk::binding(9, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutMaterial0 : register(u0);

#ifdef VK_MODE
[[vk::binding(10, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> OutMaterial1 : register(u1);

#ifdef VK_MODE
[[vk::binding(11, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture3D<float4> OutBilateralGrid0 : register(u2);

#ifdef VK_MODE
[[vk::binding(12, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture3D<float4> OutBilateralGrid1 : register(u3);

#ifdef VK_MODE
[[vk::binding(13, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture3D<float4> OutBilateralGrid2 : register(u4);

#ifdef VK_MODE
[[vk::binding(14, 0)]]
#endif
RWStructuredBuffer<float4> ResolvedSceneLightBuf : register(u5);

// Exact float4 layer offsets matching tools/train_neural_look.py & NL_Model.h (total 3013 float4s)
static const uint OFF_W_STEM = 0u;      // 96
static const uint OFF_B_STEM = 96u;     // 8
static const uint OFF_W_DW1  = 104u;    // 72
static const uint OFF_B_DW1  = 176u;    // 8
static const uint OFF_W_PW1A = 184u;    // 512
static const uint OFF_B_PW1A = 696u;    // 16
static const uint OFF_W_PW1B = 712u;    // 512
static const uint OFF_B_PW1B = 1224u;   // 8
static const uint OFF_W_DW2  = 1232u;   // 72
static const uint OFF_B_DW2  = 1304u;   // 8
static const uint OFF_W_PW2A = 1312u;   // 640
static const uint OFF_B_PW2A = 1952u;   // 16
static const uint OFF_W_PW2B = 1968u;   // 512
static const uint OFF_B_PW2B = 2480u;   // 8
static const uint OFF_W_MAT  = 2488u;   // 64
static const uint OFF_B_MAT  = 2552u;   // 2
static const uint OFF_W_BG1  = 2554u;   // 352
static const uint OFF_B_BG1  = 2906u;   // 8
static const uint OFF_W_BG2  = 2914u;   // 96
static const uint OFF_B_BG2  = 3010u;   // 3

// 10x10 tile x 32 channels packed as 16 FP16 pairs (6.4 KB LDS)
groupshared uint s_feat[10][10][16];
groupshared float4 s_globalPool[64][2];
groupshared float4 s_h2Pool[64][8];
groupshared float4 s_sceneKeyView;

float4 SiLU4(float4 x)
{
    return x * rcp(1.0f + exp(-clamp(x, -16.0f, 16.0f)));
}

void PackVec32(int ly, int lx, float4 v[8])
{
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        s_feat[ly][lx][i * 2 + 0] = f32tof16(v[i].x) | (f32tof16(v[i].y) << 16);
        s_feat[ly][lx][i * 2 + 1] = f32tof16(v[i].z) | (f32tof16(v[i].w) << 16);
    }
}

void UnpackVec32(int ly, int lx, out float4 v[8])
{
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        uint p0 = s_feat[ly][lx][i * 2 + 0];
        uint p1 = s_feat[ly][lx][i * 2 + 1];
        v[i] = float4(f16tof32(p0 & 0xFFFFu), f16tof32(p0 >> 16),
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
        // Fallback gentle top-front directional fill in current space
        keyDir = normalize(float3(0.25f, 0.85f, -0.45f));
        keyStrength = 0.25f;
        keyCol = max(sh0Rgb, float3(0.15f, 0.15f, 0.15f));
    }

    // Transform keyDir into view space for shaders that need immediate view-space key direction
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
    // Thread 0 of group (0,0) updates ResolvedSceneLightBuf with smooth temporal inertia
    if (tid == 0)
    {
        float4 sh0, shX, shY, shZ, kDir, kCol, wUp;
        ResolveCurrentSceneLight(sh0, shX, shY, shZ, kDir, kCol, wUp);
        s_sceneKeyView = kDir;

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
    int2 tileOrigin = int2(gid.xy) * 8 - 1;

    // Step A: Load 10x10 tile (covering 8x8 interior + 1px halo for Block1_DW), run Stem -> h0 into LDS
    for (uint passIdx = 0u; passIdx < 2u; ++passIdx)
    {
        uint idx = tid + passIdx * 64u;
        if (idx < 100u)
        {
            int ly = int(idx / 10u);
            int lx = int(idx % 10u);
            int2 p8 = Clamp8(tileOrigin + int2(lx, ly));
            float2 uv8 = (float2(p8) + 0.5f) / float2(max(EighthWidth, 1), max(EighthHeight, 1));

            float4 cd8 = PyramidMip1Tex.Load(int3(p8, 0));
            float4 n8 = EighthNormalTex.Load(int3(p8, 0));
            int2 p32 = clamp(int2(uv8 * float2(Mip3Width, Mip3Height)), int2(0, 0),
                             int2(max(Mip3Width - 1, 0), max(Mip3Height - 1, 0)));
            float3 coarseRgb = PyramidMip3Tex.Load(int3(p32, 0)).rgb;

            float3 rgb = max(cd8.rgb, 0.0f);
            float luma = Luma(rgb);
            float coarseLuma = Luma(max(coarseRgb, 0.0f));
            float invLuma = rcp(max(luma, 0.02f));
            float2 nxy = clamp(n8.xy, -1.0f, 1.0f);
            float nz = -sqrt(max(0.0f, 1.0f - dot(nxy, nxy)));
            float skyMask = (cd8.a < 64000.0f && cd8.a < SkyDepthThreshold) ? 1.0f : 0.0f;
            float ndotl = saturate(dot(float3(nxy, nz), keyDirView) * 0.5f + 0.5f);

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

            float4 h0[8];
            [unroll]
            for (int og = 0; og < 8; ++og)
            {
                float4 b = ModelWeightsBuf[OFF_B_STEM + og];
                uint baseW = OFF_W_STEM + uint(og) * 12u;
                float4 acc = b;
                [unroll]
                for (int c = 0; c < 4; ++c)
                {
                    uint wIdx = baseW + uint(c) * 3u;
                    float s = dot(ModelWeightsBuf[wIdx + 0u], in0)
                            + dot(ModelWeightsBuf[wIdx + 1u], in1)
                            + dot(ModelWeightsBuf[wIdx + 2u], in2);
                    if (c == 0) acc.x += s;
                    else if (c == 1) acc.y += s;
                    else if (c == 2) acc.z += s;
                    else acc.w += s;
                }
                h0[og] = SiLU4(acc);
            }
            PackVec32(ly, lx, h0);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Step B: Block 1 (3x3 DW -> 32->64->32 + skip) on 8x8 interior threads
    // Wait: Block 2 also has a 3x3 DW, so compute Block 1 on 10x10 or 8x8?
    // Our 10x10 tile gives exact 3x3 DW1 for 8x8 interior; for Block 2's 3x3 DW we clamp to the 8x8 interior in LDS!
    int lx = int(gtid.x) + 1;
    int ly = int(gtid.y) + 1;

    float4 h0Center[8];
    UnpackVec32(ly, lx, h0Center);

    float4 dw1[8];
    [unroll]
    for (int og = 0; og < 8; ++og)
        dw1[og] = ModelWeightsBuf[OFF_B_DW1 + og];

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            uint tap = uint((dy + 1) * 3 + (dx + 1));
            float4 nVec[8];
            UnpackVec32(ly + dy, lx + dx, nVec);
            [unroll]
            for (int og = 0; og < 8; ++og)
                dw1[og] += nVec[og] * ModelWeightsBuf[OFF_W_DW1 + tap * 8u + og];
        }
    }

    float4 pw1a[16];
    [unroll]
    for (int og = 0; og < 16; ++og)
    {
        float4 acc = ModelWeightsBuf[OFF_B_PW1A + og];
        uint baseW = OFF_W_PW1A + uint(og) * 32u;
        [unroll]
        for (int c = 0; c < 4; ++c)
        {
            uint wIdx = baseW + uint(c) * 8u;
            float s = 0.0f;
            [unroll]
            for (int k = 0; k < 8; ++k)
                s += dot(ModelWeightsBuf[wIdx + k], dw1[k]);
            if (c == 0) acc.x += s;
            else if (c == 1) acc.y += s;
            else if (c == 2) acc.z += s;
            else acc.w += s;
        }
        pw1a[og] = SiLU4(acc);
    }

    float4 h1[8];
    [unroll]
    for (int og = 0; og < 8; ++og)
    {
        float4 acc = ModelWeightsBuf[OFF_B_PW1B + og] + h0Center[og];
        uint baseW = OFF_W_PW1B + uint(og) * 64u;
        [unroll]
        for (int c = 0; c < 4; ++c)
        {
            uint wIdx = baseW + uint(c) * 16u;
            float s = 0.0f;
            [unroll]
            for (int k = 0; k < 16; ++k)
                s += dot(ModelWeightsBuf[wIdx + k], pw1a[k]);
            if (c == 0) acc.x += s;
            else if (c == 1) acc.y += s;
            else if (c == 2) acc.z += s;
            else acc.w += s;
        }
        h1[og] = acc;
    }

    s_globalPool[tid][0] = h1[0];
    s_globalPool[tid][1] = h1[1];
    GroupMemoryBarrierWithGroupSync();

    // Write h1 into LDS interior [1..8][1..8] for Block 2's 3x3 DW
    PackVec32(ly, lx, h1);

    // Parallel reduce g[2] across 8x8 tile
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

    float4 g0 = s_globalPool[0][0] / 64.0f;
    float4 g1 = s_globalPool[0][1] / 64.0f;

    // Step C: Block 2 (3x3 DW clamped inside [1..8] + global concat 32+8=40 -> 64 -> 32 + skip)
    float4 dw2[8];
    [unroll]
    for (int og = 0; og < 8; ++og)
        dw2[og] = ModelWeightsBuf[OFF_B_DW2 + og];

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            uint tap = uint((dy + 1) * 3 + (dx + 1));
            int sy = clamp(ly + dy, 1, 8);
            int sx = clamp(lx + dx, 1, 8);
            float4 nVec[8];
            UnpackVec32(sy, sx, nVec);
            [unroll]
            for (int og = 0; og < 8; ++og)
                dw2[og] += nVec[og] * ModelWeightsBuf[OFF_W_DW2 + tap * 8u + og];
        }
    }

    float4 pw2a[16];
    [unroll]
    for (int og = 0; og < 16; ++og)
    {
        float4 acc = ModelWeightsBuf[OFF_B_PW2A + og];
        uint baseW = OFF_W_PW2A + uint(og) * 40u;
        [unroll]
        for (int c = 0; c < 4; ++c)
        {
            uint wIdx = baseW + uint(c) * 10u;
            float s = dot(ModelWeightsBuf[wIdx + 8u], g0) + dot(ModelWeightsBuf[wIdx + 9u], g1);
            [unroll]
            for (int k = 0; k < 8; ++k)
                s += dot(ModelWeightsBuf[wIdx + k], dw2[k]);
            if (c == 0) acc.x += s;
            else if (c == 1) acc.y += s;
            else if (c == 2) acc.z += s;
            else acc.w += s;
        }
        pw2a[og] = SiLU4(acc);
    }

    float4 h2[8];
    [unroll]
    for (int og = 0; og < 8; ++og)
    {
        float4 acc = ModelWeightsBuf[OFF_B_PW2B + og] + h1[og];
        uint baseW = OFF_W_PW2B + uint(og) * 64u;
        [unroll]
        for (int c = 0; c < 4; ++c)
        {
            uint wIdx = baseW + uint(c) * 16u;
            float s = 0.0f;
            [unroll]
            for (int k = 0; k < 16; ++k)
                s += dot(ModelWeightsBuf[wIdx + k], pw2a[k]);
            if (c == 0) acc.x += s;
            else if (c == 1) acc.y += s;
            else if (c == 2) acc.z += s;
            else acc.w += s;
        }
        h2[og] = acc;
        s_h2Pool[tid][og] = acc;
    }

    // Step D: Head_Material (32 -> 8 sigmoid) + temporal stabilization
    int2 coord8 = int2(dtid.xy);
    if (coord8.x < EighthWidth && coord8.y < EighthHeight)
    {
        float4 mat0, mat1;
        [unroll]
        for (int og = 0; og < 2; ++og)
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
            float4 sig = rcp(1.0f + exp(-clamp(acc, -16.0f, 16.0f)));
            if (og == 0) mat0 = sig;
            else mat1 = sig;
        }

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
            float mvPx = length((prevUv - uv) * float2(OutputWidth, OutputHeight));
            float diff = max(length(mat0.rgb - prevM0.rgb), abs(mat1.b - prevM1.b));
            float alpha = saturate(lerp(0.22f, 0.65f, saturate(mvPx / 12.0f)) + smoothstep(0.10f, 0.35f, diff) * 0.50f);
            mat0 = lerp(prevM0, mat0, alpha);
            mat1 = lerp(prevM1, mat1, alpha);
        }

        OutMaterial0[coord8] = mat0;
        OutMaterial1[coord8] = mat1;
    }

    GroupMemoryBarrierWithGroupSync();

    // Step E: Pool h2[8] across the 8x8 tile -> threads 0..7 emit Bilateral Grid luma slices 0..7!
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
        float4 h2p[8];
        [unroll]
        for (int k = 0; k < 8; ++k)
            h2p[k] = s_h2Pool[0][k] / 64.0f;

        float lz = float(tid) / 7.0f;
        float4 lumaEmbed = float4(lz, 2.0f * lz - 1.0f, sin(kPi * lz), cos(kPi * lz));

        float4 bg1[8];
        [unroll]
        for (int og = 0; og < 8; ++og)
        {
            float4 acc = ModelWeightsBuf[OFF_B_BG1 + og];
            uint baseW = OFF_W_BG1 + uint(og) * 44u;
            [unroll]
            for (int c = 0; c < 4; ++c)
            {
                uint wIdx = baseW + uint(c) * 11u;
                float s = dot(ModelWeightsBuf[wIdx + 8u], g0)
                        + dot(ModelWeightsBuf[wIdx + 9u], g1)
                        + dot(ModelWeightsBuf[wIdx + 10u], lumaEmbed);
                [unroll]
                for (int k = 0; k < 8; ++k)
                    s += dot(ModelWeightsBuf[wIdx + k], h2p[k]);
                if (c == 0) acc.x += s;
                else if (c == 1) acc.y += s;
                else if (c == 2) acc.z += s;
                else acc.w += s;
            }
            bg1[og] = SiLU4(acc);
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
                float s = 0.0f;
                [unroll]
                for (int k = 0; k < 8; ++k)
                    s += dot(ModelWeightsBuf[wIdx + k], bg1[k]);
                if (c == 0) acc.x += s;
                else if (c == 1) acc.y += s;
                else if (c == 2) acc.z += s;
                else acc.w += s;
            }
            bgOut[og] = acc;
        }

        uint3 gridCoord = uint3(gid.x, gid.y, tid);
        OutBilateralGrid0[gridCoord] = bgOut[0];
        OutBilateralGrid1[gridCoord] = bgOut[1];
        OutBilateralGrid2[gridCoord] = bgOut[2];
    }
}
