#ifndef NL_COMMON_HLSLI
#define NL_COMMON_HLSLI

static const float3 kLumaCoeff = float3(0.2126f, 0.7152f, 0.0722f);
static const float kPi = 3.14159265f;

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

float3 ToLinearColor(float3 color, int inEncodeMode)
{
    if (inEncodeMode == 3)
        return SrgbToLinear(color);
    if (inEncodeMode == 4)
        return PQToLinear(color);
    return max(color, 0.0f);
}

float LinearizeDepthValue(float rawDepth, int depthIsLinear, int depthIsReversed,
                          float depthLinearA, float depthLinearB, float depthLinearC,
                          float skyDepthThreshold)
{
    bool rawSky = (depthIsReversed > 0 && rawDepth <= 1e-6f) || (depthIsReversed == 0 && rawDepth >= 0.99999f);
    if (rawSky)
        return 65000.0f;

    float linDepth;
    if (depthIsLinear > 0)
    {
        float z = (depthIsReversed > 0) ? (1.0f - rawDepth) : rawDepth;
        linDepth = (z <= 1.0001f) ? max(z, 0.0f) * max(depthLinearB, 100.0f) : max(z, 0.0f);
    }
    else if (depthIsReversed > 0)
    {
        float revOffset = depthLinearB - depthLinearC;
        linDepth = depthLinearA / max(revOffset + rawDepth * depthLinearC, 1e-5f);
    }
    else
    {
        linDepth = depthLinearA / max(depthLinearB - rawDepth * depthLinearC, 1e-5f);
    }

    return (linDepth >= skyDepthThreshold) ? 65000.0f : linDepth;
}

float3 ReconstructViewPos(float2 uv, float linearDepth, float tanHalfFov, float tanHalfFovAspect)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    return float3(ndc.x * tanHalfFovAspect * linearDepth, ndc.y * tanHalfFov * linearDepth, linearDepth);
}

float2 ProjectViewPosToUv(float3 p, float invTanHalfFov, float invTanHalfFovAspect)
{
    float invZ = rcp(max(p.z, 1e-4f));
    float ndcX = p.x * invZ * invTanHalfFovAspect;
    float ndcY = p.y * invZ * invTanHalfFov;
    return float2(ndcX * 0.5f + 0.5f, 0.5f - ndcY * 0.5f);
}

float3 ViewToWorldPos(float3 viewPos, float3 camPos, float3 camRight, float3 camUp, float3 camFwd)
{
    return camPos + camRight * viewPos.x + camUp * viewPos.y + camFwd * viewPos.z;
}

float3 ViewToWorldDir(float3 viewDir, float3 camRight, float3 camUp, float3 camFwd)
{
    return normalize(camRight * viewDir.x + camUp * viewDir.y + camFwd * viewDir.z);
}

float3 WorldToViewDir(float3 worldDir, float3 camRight, float3 camUp, float3 camFwd)
{
    return normalize(float3(dot(worldDir, camRight), dot(worldDir, camUp), dot(worldDir, camFwd)));
}

// --- World-Space Spatial Hash Radiance Cache (131,072 cells x 2 uints = 1 MB) ---
static const uint kWorldHashMask = 131071u;

uint Hash3UInt(int3 p)
{
    uint h = asuint(p.x) * 73856093u ^ asuint(p.y) * 19349663u ^ asuint(p.z) * 83492791u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

void ComputeWorldHashAddr(float3 worldPos, float3 worldNormal, float viewDist,
                          out uint outIdx, out uint outSig22)
{
    // Distance-adaptive voxel size (~0.25m near camera, coarser at range)
    float voxelSize = clamp(max(viewDist * 0.045f, 0.25f), 0.25f, 8.0f);
    int3 cell = int3(floor(worldPos / voxelSize));
    // Quantize normal into 6 dominant octahedron faces + 3-bit sub-sector
    int3 qNorm = int3(round(worldNormal * 2.0f));
    int lodBucket = int(clamp(floor(log2(max(voxelSize * 4.0f, 1.0f))), 0.0f, 5.0f));
    int3 key = cell + qNorm * 131 + int3(lodBucket * 17, lodBucket * 31, lodBucket * 53);

    uint h = Hash3UInt(key);
    outIdx = h & kWorldHashMask;
    uint h2 = Hash3UInt(key + int3(19, -7, 43));
    outSig22 = (h2 >> 10) & 0x3FFFFFu;
    if (outSig22 == 0u)
        outSig22 = 1u;
}

uint PackRadiance101010(float3 rad)
{
    // Log-HDR encoding [0, 32.0] into 10-10-10 bits + 2-bit weight flag
    float3 enc = saturate(log2(1.0f + max(rad, 0.0f) * 2.0f) / 6.0f);
    uint r = uint(round(enc.r * 1023.0f));
    uint g = uint(round(enc.g * 1023.0f));
    uint b = uint(round(enc.b * 1023.0f));
    return (r & 0x3FFu) | ((g & 0x3FFu) << 10) | ((b & 0x3FFu) << 20) | (1u << 30);
}

float3 UnpackRadiance101010(uint packed)
{
    float3 enc = float3(
        float(packed & 0x3FFu),
        float((packed >> 10) & 0x3FFu),
        float((packed >> 20) & 0x3FFu)
    ) / 1023.0f;
    return max((exp2(enc * 6.0f) - 1.0f) * 0.5f, 0.0f);
}

bool LookupWorldHash(StructuredBuffer<uint> worldHashBuf, float3 worldPos, float3 worldNormal,
                     float viewDist, int frameIndex, out float3 outRadiance)
{
    uint idx, sig22;
    ComputeWorldHashAddr(worldPos, worldNormal, viewDist, idx, sig22);

    uint meta = worldHashBuf[idx * 2u];
    uint cellSig = meta & 0x3FFFFFu;
    if (cellSig == 0u || cellSig != sig22)
    {
        outRadiance = float3(0.0f, 0.0f, 0.0f);
        return false;
    }

    uint stamp = (meta >> 22) & 0x3FFu;
    uint age = (uint(frameIndex) - stamp) & 0x3FFu;
    if (age > 90u)
    {
        outRadiance = float3(0.0f, 0.0f, 0.0f);
        return false;
    }

    uint packedCol = worldHashBuf[idx * 2u + 1u];
    if ((packedCol >> 30) == 0u)
    {
        outRadiance = float3(0.0f, 0.0f, 0.0f);
        return false;
    }

    outRadiance = UnpackRadiance101010(packedCol);
    return true;
}

#endif
