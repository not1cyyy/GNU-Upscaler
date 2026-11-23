#pragma once

#include "pch.h"
#include "SysUtils.h"
#include <shaders/Shader_Common.h>

struct CompareParams
{
    float DiffThreshold = 0.03f;
    float PinkAmount = 0.6f;
    float InvOutputSize[2] = { 0, 0 };
    uint32_t HudlessEncodeMode = 0;
    uint32_t PresentEncodeMode = 0;
};

static const std::string hcCode = R"(
Texture2D gRefTex : register(t0);
Texture2D gBackCopyTex : register(t1);

SamplerState gPointClamp : register(s0);

cbuffer Params : register(b0)
{
    float DiffThreshold;
    float PinkAmount;
    float2 InvOutputSize;
    uint HudlessEncodeMode;
    uint PresentEncodeMode;
};
)" + std::string(kColorSpaceHlsl) + R"(
struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 refC = gRefTex.Sample(gPointClamp, i.uv);
    float4 backC = gBackCopyTex.Sample(gPointClamp, i.uv);

    float3 refRgb = EncodeColor(refC.rgb, HudlessEncodeMode);
    float3 backRgb = EncodeColor(backC.rgb, PresentEncodeMode);

    float3 diff = abs(refRgb - backRgb);
    float maxPeak = max(max(refRgb.r, max(refRgb.g, refRgb.b)), max(backRgb.r, max(backRgb.g, backRgb.b)));
    float d = max(max(diff.r, diff.g), diff.b) / max(1.0f, maxPeak);

    // soft threshold
    float m = smoothstep(DiffThreshold, DiffThreshold * 2.0f, d);

    const float3 pink = float3(1.0f, 0.4f, 0.6f);
    float3 outRgb = lerp(backRgb, pink, m * PinkAmount);
    return float4(outRgb, backC.a);
}
)";
