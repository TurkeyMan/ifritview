// Noise reduction and sharpening (detail.h): what the filter's passes
// (detail.hlsl) and its noise measure (noise.hlsl) share. As detail.cpp's
// encode() and decode(), the CPU's.
#ifndef DETAIL_HLSLI
#define DETAIL_HLSLI

static const float kDetailFloor = 0.002;  // DetailFilter::kFloor
static const float kB3[5] = {1.0 / 16, 4.0 / 16, 6.0 / 16, 4.0 / 16, 1.0 / 16};
// The noise measure: a histogram of the finest scale's absolute detail,
// luminance's then colour's, kNoiseBins each over 0..kNoiseRange, then the
// two standard deviations (floats) after them.
static const uint kNoiseBins = 1024;
static const float kNoiseRange = 0.064;
static const uint kNoiseSigma = 2 * kNoiseBins;  // words: luminance, colour
static const int kNoiseStride = 4;               // the lattice it's measured on

// White-balanced scene-linear RGB to its encoding: each channel's square
// root (shot noise about the same size at every brightness), as luminance
// and the two colour differences.
float3 detail_encode(float3 rgb)
{
    const float3 e = sqrt(max(rgb, 0.0) + kDetailFloor);
    return float3((e.r + 2 * e.g + e.b) / 4, e.r - e.g, e.b - e.g);
}

float3 detail_decode(float3 c)
{
    const float g = c.x - (c.y + c.z) / 4;
    const float3 e = max(float3(c.y + g, g, c.z + g), 0.0);
    return e * e - kDetailFloor;
}

#endif
