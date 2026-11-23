// Where each colour of a raw clips, from its own data, for the develop to
// clip at (develop.hlsl's clipped()): the white a camera states can be well
// above where a colour really stops (a Canon mRAW's green, a 12-bit NEF's).
// Counting (this, over the mosaic as the develop reads it, or LINEAR's or
// YCC's three colours a photosite): each colour's values over the top of
// the range, as a fraction of it from black to the stated white, into
// develop.hlsli's bins, and how many of each colour there are. A thread
// counts 4 x 4 photosites into its group's bins; then one atomic add a bin
// the group used. Then FIND, one thread: where each colour's values pile up
// at a top of their own, if they do. Over the developed image (develop.hlsl's
// PILE and DRIFT): LIMIT, where they pile up under that; DRIFT, where a
// colour's ratio to the others drifts as the light on a surface rises
// (drift.hlsli); and COMBINE, each colour's linearity limit from both.
#define COMPUTE
#include "develop.hlsli"
#include "drift.hlsli"

RWByteAddressBuffer clips : register(u0);

#ifdef LIMIT
// Each colour's linearity limit, from PILE's counts over the developed
// image: level by level from a quarter of its clip up, the first where its
// values pile up (six times as many as the mean of the four levels below,
// and, but for an sRAW, four times all those past the next: its top). What
// FIND finds over the mosaic, here also where FIND doesn't look, under
// three quarters of the stated white: a Canon sRAW's camera puts one white
// where it clipped, before encoding, and decoded, that white's colours can
// sit well under the stated white (blue at under half of it, as much again
// as the balance lifts it) and under what's around it (develop.hlsl's YCC).
static const uint kPileCount = 64;

float pile_count(uint c, uint b)
{
    return float(clips.Load((kClipPile + c * kPileBins + b) * 4));
}

[numthreads(1, 1, 1)]
void main()
{
    float3 limit = 1;
    [unroll] for (uint c = 0; c < 3; ++c)
        for (uint b = 4; b < kPileBins; ++b)
        {
            const float k = pile_count(c, b);
            if (k < kPileCount)
                continue;
            float below = 0, above = 0;
            for (uint e = b - 4; e < b; ++e)
                below += pile_count(c, e);
            for (uint f = b + 2; f < kPileBins; ++f)
                above += pile_count(c, f);
            if (k > 6 * below / 4 && (ycc.x || above * 4 < k))
            {
                limit[c] = kPileFrom + float(b) * (1 - kPileFrom) / kPileBins;
                break;
            }
        }
    // The piles (an sRAW's: its camera's white, censoring all three where all
    // three reach it); and, until COMBINE has the drift's too, the limits.
    clips.Store3(kClipPiled * 4, asuint(limit));
    clips.Store3(kClipLimit * 4, asuint(ycc.x ? float3(1, 1, 1) : limit));
}
#elif defined(DRIFT)
// drift.hlsli's runs: a thread a tile (develop.hlsl's DRIFT's), the middle of
// a run along each axis, its sums counted into its group's in fixed point
// (a group's runs can't overflow 32 bits), then the group's added to the
// image's, as floats.
RWByteAddressBuffer tiles : register(u2);

static const float kDriftScale = 1048576;
static const float kDriftNoiseScale = 67108864;  // the noise's variances (sums 4 and 5), far smaller

float drift_scale(uint k)
{
    return k % kDriftSums >= 4 ? kDriftNoiseScale : kDriftScale;
}
groupshared int sums[kDriftGroups * 3 * kDriftBins * kDriftSums];

float4 tile_at(int x, int y, int across)
{
    return asfloat(tiles.Load4(uint(y * across + x) * 32));
}

float4 noise_at(int x, int y, int across)
{
    return asfloat(tiles.Load4(uint(y * across + x) * 32 + 16));
}

void add_float(uint word, float v)
{
    uint seen = clips.Load(word * 4);
    for (int k = 0; k < 256; ++k)
    {
        uint was;
        clips.InterlockedCompareExchange(word * 4, seen, asuint(asfloat(seen) + v), was);
        if (was == seen)
            break;
        seen = was;
    }
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    for (uint i = index; i < kDriftGroups * 3 * kDriftBins * kDriftSums; i += 256)
        sums[i] = 0;
    GroupMemoryBarrierWithGroupSync();
    const int across = (map_x.w + kDriftTile - 1) / kDriftTile, down = (map_y.w + kDriftTile - 1) / kDriftTile;
    const int2 p = int2(id.xy);
    if (!flags.y && p.x < across && p.y < down && tile_at(p.x, p.y, across).w > 0)
    {
        float3 found = asfloat(clips.Load3(kClipFound * 4));
        found += (found <= 0) * (1 - found);
        const float3 clip = found * to_balance();
        float stops[3] = {clip.r, clip.g, clip.b};
        const float4 m = tile_at(p.x, p.y, across);
        const uint group = uint(p.x * 2 >= across) + 2 * uint(p.y * 2 >= down);  // its quadrant
        float mid[3] = {m.r, m.g, m.b};
        [unroll] for (uint axis = 0; axis < 2; ++axis)
        {
            const int2 step = axis == 0 ? int2(1, 0) : int2(0, 1);
            const int2 a = p - step, b = p + step;
            if (any(a < 0) || b.x >= across || b.y >= down)
                continue;
            const float4 l = tile_at(a.x, a.y, across), h = tile_at(b.x, b.y, across);
            if (l.w <= 0 || h.w <= 0)
                continue;
            const float4 ln = noise_at(a.x, a.y, across), hn = noise_at(b.x, b.y, across);
            float lo[3] = {l.r, l.g, l.b}, hi[3] = {h.r, h.g, h.b}, lo_noise[3] = {ln.r, ln.g, ln.b}, hi_noise[3] = {hn.r, hn.g, hn.b};
            [unroll] for (int c = 0; c < 3; ++c)
            {
                int bin;
                float s[kDriftSums];
                if (drift_run(c, lo, mid, hi, lo_noise, hi_noise, stops, bin, s))
                {
                    for (uint k = 0; k < kDriftSums; ++k)
                        InterlockedAdd(sums[((group * 3 + uint(c)) * kDriftBins + uint(bin)) * kDriftSums + k], int(round(s[k] * drift_scale(k))));
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = index; j < kDriftGroups * 3 * kDriftBins * kDriftSums; j += 256)
        if (sums[j] != 0)
            add_float(kClipDrift + j, float(sums[j]) / drift_scale(j));
}
#elif defined(COMBINE)
// Each colour's linearity limit: the lower of its piles' (LIMIT's; not an
// sRAW's, whose piles are its camera's white, develop.hlsl's YCC) and its
// drift's (drift_limit()), or where the drift isn't sure the camera's
// remembered one (gpu_view's, raw_linearity.h), if there is one.
[numthreads(1, 1, 1)]
void main()
{
    const float3 piled = asfloat(clips.Load3(kClipPiled * 4)), stored = asfloat(clips.Load3(kClipStored * 4));
    float3 drifted = 1, sure = 0, limit = 1;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        // The colour's sums in each quadrant (each quadrant's colours' in turn).
        float s[kDriftGroups * kDriftBins * kDriftSums];
        for (uint g = 0; g < kDriftGroups; ++g)
            for (uint i = 0; i < kDriftBins * kDriftSums; ++i)
                s[g * kDriftBins * kDriftSums + i] = asfloat(clips.Load((kClipDrift + (g * 3 + c) * kDriftBins * kDriftSums + i) * 4));
        int ok = 0;
        if (!flags.y)
            drifted[c] = drift_limit(s, ok);
        sure[c] = ok;
        const float chosen = ok != 0 ? drifted[c] : stored[c] > 0 ? stored[c] : 1;
        limit[c] = ycc.x ? chosen : min(piled[c], chosen);
    }
    // How many of PILE's values are past their colour's limit (or an sRAW's
    // white), under its clip: any, and the develop estimates.
    float censored = 0;
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        const float from = min(limit[k], piled[k]);
        if (from >= 1)
            continue;
        for (uint b = uint(max((from - kPileFrom) / (1 - kPileFrom) * kPileBins, 0.0)); b < kPileBins; ++b)
            censored += float(clips.Load((kClipPile + k * kPileBins + b) * 4));
    }
    clips.Store(kClipCensored * 4, asuint(censored));
    // Whether any photosite is past its colour's limit: each colour's largest
    // value over the mosaic (the top of its highest bin, as the clip pass
    // counted them: the photosites the develop reads, as fractions of the
    // range) against its limit as such a fraction. An sRAW's always is (its
    // camera's white isn't in those counts); a grey raw's one colour is
    // counted as green, against the least limit, as the develop has it.
    const float3 found = asfloat(clips.Load3(kClipFound * 4));
    uint any = ycc.x ? 1u : 0u;
    [unroll] for (uint e = 0; e < 3; ++e)
    {
        if (flags.y && e != 1)
            continue;
        int top = int(kClipBins) - 1;
        while (top >= 0 && clips.Load((e * kClipBins + uint(top)) * 4) == 0)
            --top;
        const float largest = top >= 0 ? kClipFrom + float(top + 1) * kClipSpan / kClipBins : kClipFrom;
        const float lim = flags.y ? min(limit.r, min(limit.g, limit.b)) : limit[e];
        if (largest >= (found[e] > 0 ? found[e] : 1) * lim)
            any = 1;
    }
    clips.Store(kClipAny * 4, any);
    clips.Store3(kClipDrifted * 4, asuint(drifted));
    clips.Store3(kClipDriftSure * 4, asuint(sure));
    clips.Store3(kClipLimit * 4, asuint(limit));
}
#elif defined(FIND)
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
    // The readout's noise, from the masked border (a Bayer mosaic's, its
    // black measured there): half the mean square difference of photosites
    // of a colour two rows apart (the column's own offset cancels), as a
    // variance in the develop's balanced units, where it clips 1 (clipped()).
    float3 read = 0, counted = 0;
    read_levels();
    if (measured && cfa.w == 2 && !flags.y)
    {
        const float white = develop_white(found);
        const float3 balance = to_balance();
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            const uint lo = levels.Load((kLevelsSquares + 2 * k) * 4), hi = levels.Load((kLevelsSquares + 2 * k + 1) * 4);
            const uint n = levels.Load((kLevelsDifferences + k) * 4);
            // Its colour as a mask (G2 is green), not an index: FXC can't
            // write a vector's component by one.
            const float3 colour = k == 0 ? float3(1, 0, 0) : k == 2 ? float3(0, 0, 1) : float3(0, 1, 0);
            const float variance = n ? (float(hi) * 4294967296.0 + float(lo)) / float(n) / 2 : 0;
            const float scale = dot(balance, colour) / (as_shot.w - float(measured_sum[k]) / float(max(measured_count[k], 1u))) / white;
            read += colour * (variance * scale * scale);
            counted += n ? colour : float3(0, 0, 0);
        }
    }
    clips.Store3(kClipReadNoise * 4, asuint(all(counted > 0) ? read / max(counted, 1.0) : float3(0, 0, 0)));
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
