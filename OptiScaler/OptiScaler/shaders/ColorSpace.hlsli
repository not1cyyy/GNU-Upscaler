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
    // scRGB linear (1.0 = 80 nits) -> SMPTE ST.2084 PQ [0, 1]
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
    // SMPTE ST.2084 PQ [0, 1] -> scRGB linear (1.0 = 80 nits)
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

float3 EncodeColor(float3 color, uint mode)
{
    if (mode == 1u)
        return LinearToSrgb(color);
    if (mode == 2u)
        return LinearToPQ(color);
    if (mode == 3u)
        return SrgbToLinear(color);
    if (mode == 4u)
        return PQToLinear(color);
    if (mode == 5u)
        return LinearToPQ(SrgbToLinear(color));
    if (mode == 6u)
        return LinearToSrgb(PQToLinear(color));
    return color;
}
