// One separable pass of a resample: along x (axis 0) or y (axis 1) from
// `source` into the target, with the filter and mapping in resample.hlsli.
// The mip chain's halvings and the view's first pass.
#include "resample.hlsli"

cbuffer Params : register(b0)
{
    int axis;
    int filter;
    float inv;       // source texels per output pixel, along `axis`
    float offset;    // source position of output pixel 0's left/top edge
    int size;        // the source's size along `axis`: taps past it are clamped
    int other;       // added to the other coordinate (output row/column -> source)
    float2 unused;
};

Texture2D<float4> source : register(t0);

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 o = int2(position.xy);
    const float u = ((axis == 0 ? o.x : o.y) + 0.5) * inv + offset;
    int first, count;
    taps(filter, u, inv, first, count);
    float4 sum = 0;
    float total = 0;
    for (int k = 0; k < count; ++k)
    {
        const int t = first + k;
        const float w = weight(filter, float(t), u, inv);
        const int i = clamp(t, 0, size - 1);
        const int2 at = axis == 0 ? int2(i, o.y + other) : int2(o.x + other, i);
        sum += w * source.Load(int3(at, 0));
        total += w;
    }
    return total != 0 ? sum / total : 0;
}
