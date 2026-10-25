// Where two images differ, as an image the view then shows (and resamples)
// as it does any: difference_image()'s (compare.cpp) on the device, at the
// image's size. Its errors are compare_images()'s, a byte a pixel (the
// largest channel difference of the two as 8-bit pixels: what the numbers
// say and what's saved); over them, A's luminance dimmed to a quarter, from
// A as it's shown (`source`, through view.hlsli's mapping at the frame's
// exposure). Both are worked in 8-bit sRGB values as the CPU's are, so it
// looks as it did there; the result goes back to linear light.
#include "view.hlsli"

Texture2D<float> errors : register(t2);

float srgb_encode(float v)
{
    v = saturate(v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

float srgb_decode(float v)
{
    return v <= 0.04045 ? v / 12.92 : pow(max(v + 0.055, 0.0) / 1.055, 2.4);
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const int3 p = int3(position.xy, 0);
    // A's pixel as Image keeps it: shown, sRGB-encoded, premultiplied.
    float4 c = source.Load(p);
    c.a = saturate(c.a);
    c.rgb = max(c.rgb, 0);
    const float3 shown = c.a > 0 ? saturate(tone_map(c.rgb / c.a)) : 0;
    const uint3 v = uint3(round(float3(srgb_encode(shown.r), srgb_encode(shown.g), srgb_encode(shown.b)) * c.a * 255));
    const uint grey = ((v.r * 54 + v.g * 183 + v.b * 19) >> 8) / 4;

    // The amplified error to a colour: black, red, yellow, white.
    const int e = min(255, int(round(errors.Load(p) * 255)) * gain);
    const int3 heat = int3(min(255, e * 3), clamp(e * 3 - 255, 0, 255), clamp(e * 3 - 510, 0, 255));
    const float3 rgb = float3(min(uint3(heat) + grey, 255u)) / 255;
    return float4(srgb_decode(rgb.r), srgb_decode(rgb.g), srgb_decode(rgb.b), 1);
}
