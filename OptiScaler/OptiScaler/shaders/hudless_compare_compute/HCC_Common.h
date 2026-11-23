#pragma once

#include "pch.h"
#include <shaders/Shader_Common.h>

static const std::string shaderCode = std::string(kColorSpaceHlsl) + R"(
cbuffer Params : register(b0)
{
    float DiffThreshold;
    float PinkAmount;
    uint HudlessEncodeMode;
    uint PresentEncodeMode;
};

Texture2D<float3> Hudless : register(t0);
Texture2D<float3> PresentCopy : register(t1);
RWTexture2D<float3> Present : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height;
    PresentCopy.GetDimensions(width, height);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    uint2 pixelCoord = dispatchThreadID.xy;

    float3 hudless = EncodeColor(Hudless.Load(int3(pixelCoord, 0)), HudlessEncodeMode);
    float3 present = EncodeColor(PresentCopy.Load(int3(pixelCoord, 0)), PresentEncodeMode);

    float3 diff = abs(hudless - present);
    float maxPeak = max(max(hudless.r, max(hudless.g, hudless.b)), max(present.r, max(present.g, present.b)));
    float delta = max(max(diff.r, diff.g), diff.b) / max(1.0f, maxPeak);

    float uiMask = smoothstep(DiffThreshold, DiffThreshold * 2.0f, delta);

    const float3 pink = float3(1.0f, 0.4f, 0.6f);
    float3 outRgb = lerp(present, pink, uiMask * PinkAmount);

    Present[pixelCoord] = outRgb;
}
)";
