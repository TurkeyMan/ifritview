// Where each colour of a raw clips, from its own data, for the develop to
// clip at (develop.hlsl's clipped()): the white a camera states can be well
// above where a colour really stops (a Canon mRAW's green, a 12-bit NEF's).
// Counting (this, over the mosaic as the develop reads it, or LINEAR's or
// YCC's three colours a photosite): each colour's values over the top of
// the range, as a fraction of it from black to the stated white, into
// develop.hlsli's bins, and how many of each colour there are. A thread
// counts 4 x 4 photosites into its group's bins; then one atomic add a bin
// the group used. Then FIND, one thread: where each colour's values pile up
// at a top of their own, if they do.
#define COMPUTE
#include "develop.hlsli"

RWByteAddressBuffer clips : register(u0);

#ifdef FIND
uint bin(uint base, int i)
{
    return clips.Load((base + uint(i)) * 4);
}

// A colour's clip: where its largest values pile up, much denser there than
// just below and not a handful (1 in 20000 of the colour's values at least),
// its lower edge a step below the first of its bins under a tenth of its
// fullest (as a fraction of the range); 0 for none. A sky that's lost its
// green piles up there; an unclipped colour's values thin out to the top.
float find(uint c)
{
    const int band = int(0.01 * kClipBins / kClipSpan + 0.5);     // 1% of the range
    const int window = int(0.0025 * kClipBins / kClipSpan + 0.5);  // 0.25%
    const uint base = c * kClipBins, total = clips.Load((kClipTotal + c) * 4);
    int top = int(kClipBins) - 1;
    while (top > 0 && bin(base, top) == 0)
        --top;
    // The fullest window ending in the top band, and how full the band below is.
    uint best = 0;
    int best_end = top;
    for (int e = top; e > top - band && e >= window - 1; --e)
    {
        uint s = 0;
        for (int k = 0; k < window; ++k)
            s += bin(base, e - k);
        if (s > best)
        {
            best = s;
            best_end = e;
        }
    }
    uint below = 0;
    for (int v = max(top - 2 * band, 0); v < max(top - band, 0); ++v)
        below += bin(base, v);
    // (In floats: counts of millions overflow 32 bits once multiplied.)
    if (best == 0 || float(best) * 20000 < float(total) || float(best) * band < 6 * float(below) * window)
        return 0;
    uint peak = 0;
    int at = best_end;
    for (int k = 0; k < window; ++k)
    {
        const uint b = bin(base, best_end - k);
        if (b > peak)
        {
            peak = b;
            at = best_end - k;
        }
    }
    int edge = at;
    while (edge > 0 && float(bin(base, edge - 1)) * 10 >= float(peak))
        --edge;
    return kClipFrom + float(edge - 1) * kClipSpan / kClipBins;
}

// Where none piles up but the data's largest value, of any colour, is
// within a quarter of the stated white and under it, that's taken for the
// white of all three, as LibRaw's develop does (adjust_maximum): an sRAW
// at a high ISO stops short of its stated white with its clip smeared over
// a range of values, no pile, and its sky near the sun came out magenta.
[numthreads(1, 1, 1)]
void main()
{
    float3 found;
    float largest = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        found[c] = find(c);
        int top = int(kClipBins) - 1;
        while (top >= 0 && bin(c * kClipBins, top) == 0)
            --top;
        if (top >= 0)
            largest = max(largest, kClipFrom + float(top + 1) * kClipSpan / kClipBins);
    }
    if (all(found <= 0) && largest > 0 && largest < 1)
        found = largest;
    clips.Store3(kClipFound * 4, asuint(found));
    clips.Store(kClipWhite * 4, asuint(develop_white(found)));
}
#else
#ifdef LINEAR
Texture2D<float4> pixels : register(t0);  // unorm 16: data / 65535, R, G, B
#elif !defined(YCC)
Texture2D<float> mosaic : register(t0);  // unorm 16 (or halves, gains' fractions kept): data / 65535
#endif

groupshared uint bins[3 * kClipBins];
groupshared uint counted[3];

// A value of colour c, as a fraction of the range.
void count(uint c, float v, inout uint3 n)
{
    n += uint3(c == 0, c == 1, c == 2);
    if (v >= kClipFrom)
        InterlockedAdd(bins[c * kClipBins + min(uint((v - kClipFrom) / kClipSpan * kClipBins), kClipBins - 1)], 1u);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    for (uint i = index; i < 3 * kClipBins; i += 256)
        bins[i] = 0;
    if (index < 3)
        counted[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    read_levels();
    uint3 n = 0;
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k)
        {
            const int x = int(id.x) * 4 + k, y = int(id.y) * 4 + j;
            if (x >= size.x || y >= size.y)
                continue;
#ifdef LINEAR
            const float3 v = (pixels.Load(int3(x, y, 0)).rgb * 65535 - black[0].rgb) / (as_shot.w - black[0].rgb);
            count(0, v.r, n), count(1, v.g, n), count(2, v.b, n);
#elif defined(YCC)
            // At each block's first photosite, where its colour is.
            if (((x - ycc_at.x) & 1) != 0 || ((y - ycc_at.y) & (ycc.x - 1)) != 0)
                continue;
            const float3 v = (ycc_rgb(luma(x, y), chroma_at(x, y)) - black[0].rgb) / (as_shot.w - black[0].rgb);
            count(0, v.r, n), count(1, v.g, n), count(2, v.b, n);
#else
            const float b = black_at(x, y);
            count(flags.y ? 1 : colour_at(x, y), (mosaic.Load(int3(x, y, 0)) * 65535 - b) / (as_shot.w - b), n);
#endif
        }
    InterlockedAdd(counted[0], n.x);
    InterlockedAdd(counted[1], n.y);
    InterlockedAdd(counted[2], n.z);
    GroupMemoryBarrierWithGroupSync();
    for (uint b = index; b < 3 * kClipBins; b += 256)
        if (bins[b])
            clips.InterlockedAdd(b * 4, bins[b]);
    if (index < 3 && counted[index])
        clips.InterlockedAdd((kClipTotal + index) * 4, counted[index]);
}
#endif
