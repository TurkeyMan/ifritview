// A raw's colour field (develop.hlsl's clipped()) spread smoothly from where
// it's measured across where it isn't. Each texel holds two measures, each
// a mean and its weight (develop.hlsl's GATHER: log2 ratios, or log2
// brightness, weighed by how sure they are). A pyramid, each level half the
// last: DOWN, each texel the weighted mean of the four under it and the sum
// of their weights; UP, back from the smallest, each level's own mean
// weighed against the level above's (bilinear, its weight shared among the
// four texels under it). Where a level has measures its own count, as much
// as those around it; where it has none, the level above's fills in: a
// weighted mean throughout, so it never leaves the range of what's
// measured. x, y: the first mean and weight; z, w: the second's.
//
// A weight over 0 is a measurement of the texel's own; where there's none,
// UP leaves how much the fill rests on, from the measurements around, as a
// weight under 0: its confidence, not a measurement (RELAX holds only what's
// measured; develop.hlsl's chromaticity_at() weighs each ratio by either, so
// a ratio measured nowhere around counts for nothing, rather than as grey).
#include "estimate.hlsli"
#include "clip_layout.hlsli"

// clip.hlsl's: where nothing's past any colour's limit (kClipAny 0), there's
// no field to spread, and each pass writes nothing (what its target held
// stays: the develop doesn't read the field when nothing's censored).
ByteAddressBuffer clips : register(t8);

Texture2D<float4> source : register(t0);

// A texel's measures with their weights as confidences: measured or filled.
float4 confident(float4 v)
{
    return float4(v.x, abs(v.y), v.z, abs(v.w));
}

#ifdef DOWN
float4 main(float4 position : SV_Position) : SV_Target
{
    if (clips.Load(kClipAny * 4) == 0)
        discard;  // nothing written: the develop never reads it
    uint w, h;
    source.GetDimensions(w, h);
    const int2 p = int2(position.xy) * 2;
    float2 sum = 0, weight = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
        {
            const int2 q = p + int2(i, j);
            if (q.x >= int(w) || q.y >= int(h))
                continue;
            const float4 v = source.Load(int3(q, 0));
            const float2 m = max(v.yw, 0);  // what's measured only
            sum += v.xz * m;
            weight += m;
        }
    const float2 mean = float2(weight.x > 0 ? sum.x / weight.x : 0, weight.y > 0 ? sum.y / weight.y : 0);
    return float4(mean.x, weight.x, mean.y, weight.y);
}
#elif defined(RELAX)
// Across where nothing measured it (no weight over 0), each texel relaxed
// towards those around it, what's measured held; a level's sweeps start
// from UP's fill, the level above's sweeps already settled its broad shape.
// The ratios (x; RATIOS: z too, the first field's two) as a membrane (a
// Jacobi sweep of Laplace's equation): an area's colour comes from its own
// edge, each part from the nearest of it, and stays in the range of what's
// there, where the pyramid's fill alone would bring in the colour of a
// wide surround (a flame whitening towards its core keeps whitening). The
// magnitude (z, the second field's) as a thin plate (plate_step()): it
// continues the slope around it on in, so what rose towards an area rises on
// across it, where a membrane would level off.
float4 main(float4 position : SV_Position) : SV_Target
{
    if (clips.Load(kClipAny * 4) == 0)
        discard;  // nothing written: the develop never reads it
    uint w, h;
    source.GetDimensions(w, h);
    const int2 p = int2(position.xy);
    float4 own = source.Load(int3(p, 0));
#define AT(i, j) source.Load(int3(clamp(p + int2(i, j), 0, int2(w, h) - 1), 0))
    const float4 n4 = AT(-1, 0) + AT(1, 0) + AT(0, -1) + AT(0, 1);
    if (own.y <= 0)
        own.x = n4.x / 4;
#ifdef RATIOS
    if (own.w <= 0)
        own.z = n4.z / 4;
#else
    if (own.w <= 0)
    {
        const float corners = AT(-1, -1).z + AT(1, -1).z + AT(-1, 1).z + AT(1, 1).z;
        const float far = AT(-2, 0).z + AT(2, 0).z + AT(0, -2).z + AT(0, 2).z;
        own.z = plate_step(own.z, n4.z, corners, far);
    }
#endif
#undef AT
    return own;
}
#elif defined(UP)
Texture2D<float4> above : register(t1);  // the next level, smaller, filled already

float4 main(float4 position : SV_Position) : SV_Target
{
    if (clips.Load(kClipAny * 4) == 0)
        discard;  // nothing written: the develop never reads it
    uint w, h;
    above.GetDimensions(w, h);
    const float2 f = (floor(position.xy) + 0.5) / 2 - 0.5;
    const int2 a = int2(floor(f));
    const float2 t = f - a;
    const int2 lo = clamp(a, 0, int2(w, h) - 1), hi = clamp(a + 1, 0, int2(w, h) - 1);
#define AT(i, j) confident(above.Load(int3(i, j, 0)))
    const float4 top = lerp(AT(lo.x, lo.y), AT(hi.x, lo.y), t.x);
    const float4 up = lerp(top, lerp(AT(lo.x, hi.y), AT(hi.x, hi.y), t.x), t.y);
#undef AT
    const float4 own = source.Load(int3(int2(position.xy), 0));
    const float2 around = up.yw / 4, total = own.yw + around;
    const float2 mean = float2(total.x > 0 ? (own.x * own.y + up.x * around.x) / total.x : up.x,
                               total.y > 0 ? (own.z * own.w + up.z * around.y) / total.y : up.z);
    // Measured: its own weight; filled: the confidence it rests on, under 0.
    return float4(mean.x, own.y > 0 ? own.y : -around.x, mean.y, own.w > 0 ? own.w : -around.y);
}
#endif
