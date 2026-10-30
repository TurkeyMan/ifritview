// sRGB's curve: linear light to its encoding, the power law carried on past
// 1 (white) for what's brighter; the same clipped to 0..1 (what's stored or
// shown); and back.
#ifndef SRGB_HLSLI
#define SRGB_HLSLI

float srgb_encode_open(float v)
{
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(max(v, 0.0), 1.0 / 2.4) - 0.055;
}

float srgb_encode(float v)
{
    return srgb_encode_open(saturate(v));
}

float srgb_decode(float v)
{
    return v <= 0.04045 ? v / 12.92 : pow(max(v + 0.055, 0.0) / 1.055, 2.4);
}

#endif
