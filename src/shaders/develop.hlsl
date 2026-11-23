// A raw's level 0: its mosaic (RawMosaic, one photosite a texel) developed.
// Each output pixel, in the image as shown (the camera's framing, turned as
// it was held), comes from the photosite under it (corrected first, where
// the raw has its own corrections of its mosaic: CORRECT): black taken off
// and scaled to the stated white, the as-shot white balance applied (the
// demosaic follows colour edges far better on balanced data), the two
// missing colours found (a Bayer's from Malvar-He-Cutler's 5 x 5 kernels,
// X-Trans's from its green made first, GREEN; none for a monochrome
// sensor's: grey), the colours past their linearity limits estimated
// (clipped(), from GATHER's colour field), and the balance taken off again.
// Linear camera RGB out; the frame's own white balance and the camera's
// colour matrix are the view's, per frame.
#if defined(PILE) || defined(DRIFT)
#define COMPUTE
#endif
#include "develop.hlsli"
#include "estimate.hlsli"
#include "drift.hlsli"

// Whether the pixel developed() last made touches blocks at an sRAW camera's
// white: its readings bounds only, as a whole (estimate_whole()).
static bool g_whole = false;

#ifdef COMPUTE
RWByteAddressBuffer clips : register(u0);  // clip.hlsl's
#else
ByteAddressBuffer clips : register(t10);  // clip.hlsl's
#endif

// Where each colour clips, balanced (1 = the stated white): where the data
// shows its values piling up (clip.hlsl's FIND), else the stated white.
float3 clip_stops()
{
    return clip_stops(asfloat(clips.Load3(kClipFound * 4)));  // develop.hlsli's
}

// The sensor's model: each colour is linear above black, its noise
// Poisson-Gaussian (variance a x + b), up to its own linearity limit: the
// lower of where its values pile up (clip.hlsl's FIND over the mosaic, LIMIT
// over the developed image) and where its ratio to the others starts to
// drift as the light on a surface rises (drift.hlsli; the camera's
// remembered one where the image doesn't show it); the stated white where
// neither's found. Past its limit a colour is censored: its true value is
// only known to be at least what was read (estimate.hlsli). Each colour is
// limited by its own limit (times its balance) only, never by another's.
// Balanced. (An sRAW's piles are its camera's white instead, pile_limits().)
float3 fraction_limits(uint at)
{
    float3 limit = asfloat(clips.Load3(at * 4));
    limit += (limit <= 0) * (1 - limit);
    if (flags.y)
        limit = min(limit.r, min(limit.g, limit.b));
    return clip_stops() * limit;
}

float3 clip_limits()
{
    return fraction_limits(kClipLimit);
}

// Whether any photosite is past its colour's limit (COMBINE's kClipAny):
// where none is, nothing's censored, and GATHER and the develop's shares
// are skipped. PILE and DRIFT run before COMBINE, and always look.
bool censoring()
{
#ifdef COMPUTE
    return true;
#else
    return clips.Load(kClipAny * 4) != 0;
#endif
}

// Where each colour's values pile up (LIMIT's), balanced: an sRAW's
// camera's white.
float3 pile_limits()
{
    return fraction_limits(kClipPiled);
}

// How sure a balanced value is in log terms, under shot noise: the inverse
// of its log's variance, x / (a g) for value x with balance g, taken in
// units of a. (The read noise b isn't measured yet: it would make dark
// values count for less still.)
float3 sureness(float3 c)
{
    return max(c, 0) / to_balance();
}

// Brightness, balanced: camera RGB weighed roughly as the eye weighs it.
// (estimate.hlsli's chromaticity has it 1.)
float luma(float3 c)
{
    return dot(c, float3(0.25, 0.5, 0.25));
}

// The colour field (highlight.hlsl's, from GATHER's): at a texel each
// kRatioCell x kRatioCell output pixels, `field` the log2 ratios of red to
// green and blue to green, each with its weight; `field_more` red to blue
// and the log2 magnitude, likewise.
Texture2D<float4> field : register(t2);
Texture2D<float4> field_more : register(t3);
static const int kRatioCell = 4;  // gpu_view's kCell
static const int kRatioStep = 2;  // GATHER reads every kRatioStep-th pixel of a cell each way

float4 field_load(Texture2D<float4> t, int2 o)
{
    uint w, h;
    t.GetDimensions(w, h);
    const float2 p = (float2(o) + 0.5) / kRatioCell - 0.5;
    const int2 a = int2(floor(p));
    const float2 f = p - a;
    const int2 lo = clamp(a, 0, int2(w, h) - 1), hi = clamp(a + 1, 0, int2(w, h) - 1);
    // Weights as confidences: measured, or (under 0) what a fill rests on.
#define AT(i, j) (t.Load(int3(i, j, 0)) * float4(1, sign(t.Load(int3(i, j, 0)).y), 1, sign(t.Load(int3(i, j, 0)).w)))
    const float4 top = lerp(AT(lo.x, lo.y), AT(hi.x, lo.y), f.x);
    const float4 bottom = lerp(AT(lo.x, hi.y), AT(hi.x, hi.y), f.x);
#undef AT
    return lerp(top, bottom, f.y);
}

// The field's chromaticity at o, its brightness 1: the log colours (green's
// 0) that best fit its three log ratios, each weighed by how much was
// measured of it here or, filled in, around (a ratio measured nowhere around
// counts for next to nothing, not as grey); and its magnitude, continued.
float3 chromaticity_at(int2 o, out float continuation)
{
    const float4 a = field_load(field, o), b = field_load(field_more, o);
    continuation = exp2(b.z);
    float q[3];
    estimate_chromaticity(a.x, a.y, a.z, a.w, b.x, b.y, q);
    return float3(q[0], q[1], q[2]);
}

// estimate_colours() on float3s.
float3 estimated(float3 c, float3 share, float3 bound, float3 q, float continuation)
{
    const float3 sure = sureness(c);
    float v[3] = {c.r, c.g, c.b}, s[3] = {share.r, share.g, share.b}, l[3] = {bound.r, bound.g, bound.b},
          cq[3] = {q.r, q.g, q.b}, su[3] = {sure.r, sure.g, sure.b}, r[3];
    if (g_whole)
        estimate_whole(v, s, cq, continuation, r);
    else
        estimate_colours(v, s, l, cq, su, continuation, r);
    return float3(r[0], r[1], r[2]);
}

// Balanced values (1 = the stated white) at output pixel o: each colour
// measured up to its linearity limit, and past it estimated
// (estimate.hlsli). Censoring is each colour's own, and so are its
// censored areas, usually nested (the colour with the least headroom clips
// over the largest area, the next inside that). A colour made only from
// censored photosites (or an sRAW's blocks) is censored, at least the least
// of what they read (`bound`); made from some, it's mixed: no measurement and
// no bound, kept but for its censored share (`share`). The colour field
// holds the log ratios between colours, which vary smoothly over the scene,
// apart from brightness: each known where both
// its colours are measured, and across a censored area interpolated
// smoothly from around it (a weighted mean, so it stays in the range of
// what's around), each measurement weighed by how sure it is under the
// noise model; where a colour's estimate meets a further colour's censored
// area, the ratios its estimate gives count too, as half a measurement. The
// magnitude where nothing's measured continues from the values around as a
// thin plate would (GATHER's second pass, highlight.hlsl's RELAX), so it
// rises on into the area as it rose towards it. Scaled so the first colour
// to clip under the balance clips at 1; what's estimated goes past it. How
// it's shown is the display mapping's.
float3 clipped(float3 balanced, float3 share, float3 bound, int2 o)
{
    const float3 stops = clip_stops();
    const float white = develop_white(asfloat(clips.Load3(kClipFound * 4)));
    // Nothing clipped below: with the black taken off, the readout's noise
    // lies either side of 0, and held at 0 it lifted the shadows and tinted
    // them (each colour by its own noise); the display shows below 0 as black.
    const float3 c = min(balanced, stops);
    if (!any(share > 0))
        return c / white;
    float continuation;
    const float3 q = chromaticity_at(o, continuation);
    return estimated(c, share, bound, q, continuation) / white;
}

#ifdef YCC
// Canon's sRAW and mRAW as stored: luma at every photosite, the colour
// differences only at each block's first (2 wide, ycc.x tall). The colour is
// filled in guided by the luma: over the blocks around, a least-squares line
// of colour against the blocks' mean luma, evaluated at this photosite's own
// luma, so colour changes where the brightness does instead of bleeding
// across edges (He et al.'s guided filter, at the colour samples). Then
// Canon's formula to camera RGB, and black, scale and the clip under the
// as-shot balance as for the others.
// `share`: of each colour, the share of the blocks it's made from that are
// censored in it, and `bound` the least they read of it. An sRAW's camera
// puts its own white, one colour, where it clipped, before encoding (every
// clipped block decodes to the same Y, Cb and Cr; a colour around it can be
// above that white's), so a block is censored in all three colours where
// all three are at that white's or over (LIMIT's piles); and in one colour
// where that's past its linearity limit (a rolled-off green). That white is
// no colour of the scene's: its blocks are left out of the colour filled in
// (where any around aren't), and a pixel made from any is estimated as a
// whole (g_whole, estimate_whole()). For clipped().
#define DEVELOPED
float3 developed(int2 o, out float3 share, out float3 bound)
{
    const int x = clamp(map_x.x * o.x + map_x.y * o.y + map_x.z, 0, size.x - 1);
    const int y = clamp(map_y.x * o.x + map_y.y * o.y + map_y.z, 0, size.y - 1);
    const float Y = luma(x, y);
    const float3 limits = clip_limits(), piles = pile_limits();
    share = 0;
    bound = 1e30;
    g_whole = false;
    const int bw = 2, bh = ycc.x;
    const int bx = int(floor(float(x - ycc_at.x) / bw)), by = int(floor(float(y - ycc_at.y) / bh));
    float n = 0, nw = 0, sy = 0, syy = 0;
    float2 sc = 0, syc = 0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i)
        {
            const int cx = ycc_at.x + (bx + i) * bw, cy = ycc_at.y + (by + j) * bh;
            if (cx < 0 || cy < 0 || cx >= size.x || cy >= size.y)
                continue;
            float m = luma(cx, cy) + luma(cx + 1, cy);
            if (bh == 2)
                m = (m + luma(cx, cy + 1) + luma(cx + 1, cy + 1)) / 4;
            else
                m /= 2;
            const float2 c = chroma_at(cx, cy);
            const float3 read = (ycc_rgb(luma(cx, cy), c) - black[0].rgb) / (as_shot.w - black[0].rgb) * as_shot.rgb;
            const bool white = all(read >= piles);
            const float3 past = max(white ? 1.0 : 0.0, float3(read >= limits));
            share += past;
            bound = min(bound, read + (1 - past) * 1e30);
            n += 1;
            g_whole = g_whole || white;
            // The camera's white is no colour of the scene's: a block at it
            // counts for the colour here only where every block around is.
            const float w = white ? 1e-4 : 1;
            nw += w;
            sy += w * m;
            syy += w * m * m;
            sc += w * c;
            syc += w * m * c;
        }
    const float mean_y = sy / nw, var_y = syy / nw - mean_y * mean_y;
    const float2 mean_c = sc / nw, cov = syc / nw - mean_y * mean_c;
    const float2 a = cov / (var_y + ycc_mul.w), b = mean_c - a * mean_y;
    const float2 cd = a * Y + b;  // the colour differences here: Cb, Cr
    const float3 rgb = (ycc_rgb(Y, cd) - black[0].rgb) / (as_shot.w - black[0].rgb);
    share /= n;
    return rgb * as_shot.rgb;
}

float3 after_clip(float3 rgb, int2 o)
{
    return rgb;
}
#elif defined(LINEAR)
// Already R, G, B a photosite (a linear DNG, Canon's sRAW): black off and
// scaled per channel, clipped at saturation under the as-shot balance as the
// mosaic's are, no demosaic. `black` is per channel here.
Texture2D<float4> pixels : register(t0);  // unorm 16: data / 65535

// `share`: whether each colour is past its limit, nothing mixed in, and
// `bound` what it read (estimate_censor()).
#define DEVELOPED
float3 developed(int2 o, out float3 share, out float3 bound)
{
    const int x = clamp(map_x.x * o.x + map_x.y * o.y + map_x.z, 0, size.x - 1);
    const int y = clamp(map_y.x * o.x + map_y.y * o.y + map_y.z, 0, size.y - 1);
    const float3 d = pixels.Load(int3(x, y, 0)).rgb * 65535;
    const float3 rgb = (d - black[0].rgb) / (as_shot.w - black[0].rgb) * to_balance();
    const float3 limits = clip_limits();
    [unroll] for (uint c = 0; c < 3; ++c)
        estimate_censor(rgb[c], limits[c], share[c], bound[c]);
    return rgb;
}

float3 after_clip(float3 rgb, int2 o)
{
    return rgb;
}
#else
Texture2D<float> mosaic : register(t0);  // unorm 16: data / 65535
#ifdef COMPUTE
RWByteAddressBuffer corrections : register(u2);
#else
ByteAddressBuffer corrections : register(t9);  // the raw's own (a DNG's opcodes, a Phase One's), as dng_opcodes.h lays them out
#endif

uint word(uint i)
{
    return corrections.Load(i * 4);
}

float word_f(uint i)
{
    return asfloat(word(i));
}

// The gain map at word `d` (its 16 words) at photosite (x, y), plane `p`:
// bilinear between its points, held at the edges; 1 where it doesn't
// apply. Its grid in photosites from its area's corner, the first one's
// centre at 0.5: the DNG SDK's (its grid relative to the area, scaled), and
// LibRaw's Phase One flat fields (whole photosites apart, from one).
float map_gain(uint d, int x, int y, uint p)
{
    // top, left, bottom, right; plane, planes, row pitch, column pitch;
    // points down, across, planes in the map, where its gains start
    const uint4 area = corrections.Load4(d * 4);
    if (y < int(area.x) || x < int(area.y) || y >= int(area.z) || x >= int(area.w))
        return 1;
    const uint4 h1 = corrections.Load4(d * 4 + 16), h2 = corrections.Load4(d * 4 + 32);
    if ((uint(y) - area.x) % h1.z || (uint(x) - area.y) % h1.w || p < h1.x || p >= h1.x + h1.y)
        return 1;
    const float4 grid = asfloat(corrections.Load4(d * 4 + 48));  // spacing down, across, origin down, across
    const uint rows = h2.x, cols = h2.y, planes = h2.z;
    const uint q = min(p - h1.x, planes - 1);
    const float fy = clamp((y - int(area.x) + 0.5 - grid.z) / grid.x, 0, rows - 1);
    const float fx = clamp((x - int(area.y) + 0.5 - grid.w) / grid.y, 0, cols - 1);
    const uint r0 = uint(fy), c0 = uint(fx), r1 = min(r0 + 1, rows - 1), c1 = min(c0 + 1, cols - 1);
    const float ty = fy - r0, tx = fx - c0;
    const uint g = h2.w;
    const float a = word_f(g + (r0 * cols + c0) * planes + q), b = word_f(g + (r0 * cols + c1) * planes + q);
    const float c = word_f(g + (r1 * cols + c0) * planes + q), e = word_f(g + (r1 * cols + c1) * planes + q);
    return lerp(lerp(a, b, tx), lerp(c, e, tx), ty);
}

#ifdef CORRECT
// The raw's own corrections of its mosaic, each photosite once, into a
// mosaic of the same kind (data / 65535, its black still in: unorm 16, or
// half floats where gains leave fractions, as gpu_view says) that the develop
// then reads as it would the raw's: the readout's own (a Phase One's) and
// the gain maps over the mosaic (OpcodeList2, a Phase One's flat fields),
// exact at every photosite. Made in the develop the demosaic would make
// them 13 times a pixel, once for each photosite it reads.

// The `i`th of the 16-bit values from word `at` on, two a word, the first low.
uint half_word(uint at, uint i)
{
    return word(at + i / 2) >> (i % 2 * 16) & 0xffff;
}

// OpcodeList2's maps multiplied (a Phase One's flat fields), at photosite (x, y).
float mosaic_gain(int x, int y)
{
    float g = 1;
    const uint n = word(0);
    for (uint i = 0; i < n; ++i)
        g *= map_gain(48 + i * 16, x, y, 0);
    return g;
}

// The photosite at (x, y) less its black, data units. With the readout's
// own corrections (a Phase One's: dng_opcodes.h's Readout), as LibRaw's
// phase_one_subtract_black and phase_one_correct make them: its own black
// too, less its row's and its column's offsets, held at 0, then through its
// quarter's curve.
float level(int x, int y)
{
    const float d = mosaic.Load(int3(x, y, 0)) * 65535 - black_at(x, y);
    if (!word(3))
        return d;
    const bool right = x >= int(word(42)), below = y >= int(word(43));
    int v = int(round(d)) - int(word(47));
    if (word(40))
    {
        const uint r = word(word(40) + y), c = word(word(41) + x);  // int16 pairs
        v += (right ? int(r) >> 16 : int(r << 16) >> 16) + (below ? int(c) >> 16 : int(c << 16) >> 16);
    }
    v = clamp(v, 0, 65535);
    if (word(44))
        v = half_word(word(44), (uint(below) * 2 + uint(right)) * 65536 + v);
    return v;
}

// Whether (x, y) is one of the readout's defects (each row's listed).
bool defective(int x, int y)
{
    const uint rows = word(45);
    if (!rows)
        return false;
    const uint end = word(rows + y + 1);
    for (uint k = word(rows + y); k < end; ++k)
        if (word(k) == uint(x))
            return true;
    return false;
}

// The photosite at (x, y) as level() has it; a defect, the rounded mean of
// its neighbours' of its colour, as LibRaw's phase_one_correct has them: a
// green's (LibRaw's colour 1's) four diagonal and four two away, others'
// eight two away. (Column, row) offsets. Neighbours that are defects too
// are left out: LibRaw takes those as it finds them, repaired if listed
// before, as read if after (a column of them, in some backs' lists, each
// photosite's below still as read).
float repaired(int x, int y)
{
    if (!defective(x, y))
        return level(x, y);
    static const int2 around[12] = {int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1), int2(0, -2), int2(-2, 0),
                                    int2(2, 0),   int2(0, 2),  int2(-2, -2), int2(2, -2), int2(-2, 2), int2(2, 2)};
    const uint j = filter_of(site_at(x, y)) != 1 ? 4 : 0;
    uint sum = 0, n = 0;
    for (uint k = j; k < j + 8; ++k)
    {
        const int2 p = int2(x, y) + around[k];
        if (all(p >= 0) && all(p < size.xy) && !defective(p.x, p.y))
        {
            sum += uint(level(p.x, p.y));
            ++n;
        }
    }
    return n ? (sum + n / 2) / n : level(x, y);
}

// The photosite at (x, y): black off, defects repaired, through the gain
// maps over the mosaic. Data units.
float gained(int x, int y)
{
    return repaired(x, y) * mosaic_gain(x, y);
}

// gained() at (x, y), or where that's outside the mosaic at the nearest
// photosite of its colour inside.
float gained_near(int x, int y)
{
    x = x < 0 ? x & 1 : x >= size.x ? size.x - 2 + ((x - size.x) & 1) : x;
    y = y < 0 ? y & 1 : y >= size.y ? size.y - 2 + ((y - size.y) & 1) : y;
    return gained(x, y);
}

// A bad column's photosite, from the photosites around it once they're
// gained (the DNG SDK's, as LibRaw's phase_one_correct has them, after its
// flat fields). A column on its own: the mean of the estimates along the
// smoothest of seven directions (phase_one_fix_pixel_grad), between its
// row's two nearest of its colour. Others: the mean of the nearest of its
// colour in the columns around (phase_one_fix_col_pixel_avg). (Row,
// column) offsets, as LibRaw's.
float column_fixed(int x, int y, bool alone)
{
    if (!alone)
    {
        static const int2 sets[3][8] = {
            {int2(-2, -2), int2(-2, 2), int2(2, -2), int2(2, 2), int2(0, 0), int2(0, 0), int2(0, 0), int2(0, 0)},
            {int2(-2, -4), int2(-4, -2), int2(2, -4), int2(4, -2), int2(-2, 4), int2(-4, 2), int2(2, 4), int2(4, 2)},
            {int2(-4, -4), int2(-4, 4), int2(4, -4), int2(4, 4), int2(0, 0), int2(0, 0), int2(0, 0), int2(0, 0)}};
        for (int s = 0; s < 3; ++s)
        {
            float total = 0, n = 0;
            for (int i = 0; i < 8 && any(sets[s][i] != 0); ++i)
            {
                const int2 p = int2(x, y) + sets[s][i].yx;
                if (all(p >= 0) && all(p < size.xy))
                {
                    total += gained(p.x, p.y);
                    n += 1;
                }
            }
            if (n)
                return total / n;
        }
        return gained(x, y);
    }
    static const int2 grads[7][12] = {
        {int2(-4, -2), int2(4, 2), int2(-3, -1), int2(1, 1), int2(-1, -1), int2(3, 1), int2(-4, -1), int2(0, 1), int2(-2, -1), int2(2, 1), int2(0, -1), int2(4, 1)},
        {int2(-2, -2), int2(2, 2), int2(-3, -1), int2(-1, 1), int2(-1, -1), int2(1, 1), int2(1, -1), int2(3, 1), int2(-2, -1), int2(0, 1), int2(0, -1), int2(2, 1)},
        {int2(-2, -4), int2(2, 4), int2(-1, -3), int2(1, 1), int2(-1, -1), int2(1, 3), int2(-2, -1), int2(0, 3), int2(-1, -2), int2(1, 2), int2(0, -3), int2(2, 1)},
        {int2(0, -2), int2(0, 2), int2(-1, -1), int2(-1, 1), int2(1, -1), int2(1, 1), int2(-1, -2), int2(-1, 2), int2(0, -1), int2(0, -1), int2(1, -2), int2(1, 2)},
        {int2(-2, 4), int2(2, -4), int2(-1, 3), int2(1, -1), int2(-1, 1), int2(1, -3), int2(-2, 1), int2(0, -3), int2(-1, 2), int2(1, -2), int2(0, 3), int2(2, -1)},
        {int2(-2, 2), int2(2, -2), int2(-3, 1), int2(-1, -1), int2(-1, 1), int2(1, -1), int2(1, 1), int2(3, -1), int2(-2, 1), int2(0, -1), int2(0, 1), int2(2, -1)},
        {int2(-4, 2), int2(4, -2), int2(-3, 1), int2(1, -1), int2(-1, 1), int2(3, -1), int2(-4, 1), int2(0, -1), int2(-2, 1), int2(2, -1), int2(0, 1), int2(4, -1)}};
    float est[7], grad[7], least = 1e30;
    for (int i = 0; i < 7; ++i)
    {
        float v[12];
        for (int j = 0; j < 12; ++j)
            v[j] = gained_near(x + grads[i][j].y, y + grads[i][j].x);
        est[i] = v[0] + v[1];
        grad[i] = 0;
        for (int k = 0; k < 12; k += 2)
            grad[i] += abs(v[k] - v[k + 1]);
        least = min(least, grad[i]);
    }
    float total = 0, n = 0;
    for (int i = 0; i < 7; ++i)
        if (grad[i] <= least * 1.5)
        {
            total += est[i];
            n += 2;
        }
    const float a = gained_near(x - 2, y), b = gained_near(x + 2, y);
    return clamp(total / n, min(a, b), max(a, b));
}

// The photosite at (x, y) corrected: data units, black off.
float corrected(int x, int y)
{
    const uint at = word(46);  // the bad columns' bits: 1 bad, 3 bad and on its own
    const uint bad = at ? word(at + uint(x) / 16) >> (uint(x) % 16 * 2) & 3 : 0;
    return bad ? column_fixed(x, y, bad == 3) : gained(x, y);
}

float4 main(float4 position : SV_Position) : SV_Target
{
    read_levels();
    const int x = int(position.x), y = int(position.y);
    return (corrected(x, y) + black_at(x, y)) / 65535;
}
#elif defined(IMPULSE)
// Hot and cold photosites taken out before the demosaic spreads them, as
// impulse.h has it for the CPU: far above (or below) all eight of its colour
// around it, by more than they differ among themselves and a floor, and the
// only such peak where it is: a glint lighting a photosite of each colour
// makes its nearest of the other colours peaks of theirs too, and is kept. It
// becomes the median of its colour's eight.
static const float kSpread = 1;      // impulse.h's kImpulseSpread
static const float kFloor = 0.005;   // impulse.h's kImpulseFloor: of the range from black to white
static const int kShared = 2;        // impulse.h's kImpulseShared

// Past an edge, mirrored: the same colour, as the parity is kept.
float level_at(int x, int y)
{
    x = x < 0 ? -x : x >= size.x ? 2 * (size.x - 1) - x : x;
    y = y < 0 ? -y : y >= size.y ? 2 * (size.y - 1) - y : y;
    return mosaic.Load(int3(x, y, 0)) * 65535 - black_at(x, y);
}

// The eight of (x, y)'s colour around it, `s` away.
void neighbours(int x, int y, int s, out float n[8])
{
    n[0] = level_at(x - s, y - s), n[1] = level_at(x, y - s), n[2] = level_at(x + s, y - s), n[3] = level_at(x - s, y);
    n[4] = level_at(x + s, y), n[5] = level_at(x - s, y + s), n[6] = level_at(x, y + s), n[7] = level_at(x + s, y + s);
}

// How far (x, y) stands above (`up`) or below all eight of its colour around it.
float beyond(int x, int y, int s, bool up)
{
    float n[8];
    neighbours(x, y, s, n);
    float lo = n[0], hi = n[0];
    for (int i = 1; i < 8; ++i)
        lo = min(lo, n[i]), hi = max(hi, n[i]);
    const float v = level_at(x, y);
    return up ? v - hi : lo - v;
}

float main(float4 position : SV_Position) : SV_Target
{
    read_levels();
    const int x = int(position.x), y = int(position.y);
    const float raw = mosaic.Load(int3(x, y, 0));
    const float v = raw * 65535 - black_at(x, y);
    const int s = flags.y ? 1 : 2;
    float n[8];
    neighbours(x, y, s, n);
    float lo = n[0], hi = n[0];
    for (int i = 1; i < 8; ++i)
        lo = min(lo, n[i]), hi = max(hi, n[i]);
    const float t = max(kSpread * (hi - lo), kFloor * (as_shot.w - black_at(x, y)));
    const bool hot = v - hi > t, cold = lo - v > t;
    if (!hot && !cold)
        return raw;
    if (!flags.y)
    {
        // Its nearest of the other colours: peaks of theirs too, a glint.
        int peaks = 0;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i)
                if ((i || j) && beyond(x + i, y + j, 2, hot) > 0)
                    ++peaks;
        if (peaks >= kShared)
            return raw;
    }
    // The median of eight: the mean of the middle two, sorted in place.
    for (int a = 1; a < 8; ++a)
        for (int b = a; b > 0 && n[b - 1] > n[b]; --b)
        {
            const float k = n[b];
            n[b] = n[b - 1], n[b - 1] = k;
        }
    return ((n[3] + n[4]) / 2 + black_at(x, y)) / 65535;
}
#else
// OpcodeList3's, on R, G, B at (x, y): its gain maps and radial vignette;
// or lensfun's vignetting (pa), the same polynomial as the fall-off to
// divide out (lens_database.h).
float3 rgb_gain(int x, int y)
{
    float3 g = 1;
    const uint n2 = word(0), n3 = word(1);
    for (uint i = 0; i < n3; ++i)
    {
        const uint d = 48 + (n2 + i) * 16;
        g *= float3(map_gain(d, x, y, 0), map_gain(d, x, y, 1), map_gain(d, x, y, 2));
    }
    const uint vignette = word(2);
    if (vignette)
    {
        const float dx = x + 0.5 - word_f(9), dy = y + 0.5 - word_f(10);
        const float r2 = (dx * dx + dy * dy) * word_f(11);
        const float p = 1 + r2 * (word_f(4) + r2 * (word_f(5) + r2 * (word_f(6) + r2 * (word_f(7) + r2 * word_f(8)))));
        g *= vignette == 2 ? 1 / p : p;
    }
    return g;
}

// The photosite at (x, y) as a balanced value, 1 = saturation (held at the
// mosaic's edges).
float site(int x, int y)
{
    x = clamp(x, 0, size.x - 1);
    y = clamp(y, 0, size.y - 1);
    const float d = mosaic.Load(int3(x, y, 0)) * 65535, b = black_at(x, y);
    return (d - b) / (as_shot.w - b) * (flags.x ? 1 : as_shot[colour_at(x, y)]);
}

bool inside(int2 p)
{
    return all(p >= 0) && all(p < size.xy);
}

#ifdef GREEN
// X-Trans's green, everywhere, as the develop's colour step (xtrans())
// reads it: a photosite of the mosaic a texel, balanced. Where the sensor
// has no green, an estimate along each of four directions, across, down and
// both diagonals: between the nearest greens either side, linear for their
// distances, with the curvature of the photosite's own colour along it (as
// Hamilton and Adams correct a Bayer's: the same colour's nearest either
// side, scaled for its distances; a quadratic's exact), held within the
// greens around it. The four are weighed by how little the mosaic changes
// along each around here (same colours either side, over the 3 x 3; the
// greens that made it), so an edge's estimate along it wins over those
// across it: no zipper. X-Trans's greens are never more than a photosite
// away across or down, its own colour at most six.

// The steps from p along d to the nearest photosite of colour c, at most
// `reach`; 0 for none inside the mosaic.
int nearest(int2 p, int2 d, int c, int reach)
{
    for (int k = 1; k <= reach; ++k)
    {
        const int2 q = p + k * d;
        if (!inside(q))
            return 0;
        if (colour_at(q.x, q.y) == c)
            return k;
    }
    return 0;
}

float green_at(int2 p)
{
    const int c = colour_at(p.x, p.y);
    const float own = site(p.x, p.y);
    if (c == 1)
        return own;
    float lo = 1e30, hi = -1e30;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
        {
            const int2 q = p + int2(i, j);
            if (inside(q) && colour_at(q.x, q.y) == 1)
            {
                lo = min(lo, site(q.x, q.y));
                hi = max(hi, site(q.x, q.y));
            }
        }
    static const int2 kDirections[4] = {int2(1, 0), int2(0, 1), int2(1, 1), int2(1, -1)};
    float total = 0, weights = 0;
    for (int k = 0; k < 4; ++k)
    {
        const int2 d = kDirections[k];
        const int a = nearest(p, d, 1, 3), b = nearest(p, -d, 1, 3);
        if (!a || !b)
            continue;
        const float ga = site(p.x + a * d.x, p.y + a * d.y), gb = site(p.x - b * d.x, p.y - b * d.y);
        float g = (b * ga + a * gb) / (a + b);
        const int s = nearest(p, d, c, 6), t = nearest(p, -d, c, 6);
        if (s && t)
            g += float(a * b) / float(s * t) * (own - (t * site(p.x + s * d.x, p.y + s * d.y) + s * site(p.x - t * d.x, p.y - t * d.y)) / (s + t));
        if (lo <= hi)
            g = clamp(g, lo, hi);
        // The change along d a step, from the greens either side and each
        // pair of photosites of one colour either side of the 3 x 3's.
        float change = abs(ga - gb) / (a + b), n = 1;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i)
            {
                const int2 q1 = p + int2(i, j) + d, q2 = p + int2(i, j) - d;
                if (inside(q1) && inside(q2) && colour_at(q1.x, q1.y) == colour_at(q2.x, q2.y))
                {
                    change += abs(site(q1.x, q1.y) - site(q2.x, q2.y)) / 2;
                    n += 1;
                }
            }
        const float w = 1 / (1e-10 + change / n * (change / n));
        total += w * g;
        weights += w;
    }
    return weights > 0 ? total / weights : lo <= hi ? (lo + hi) / 2 : own;
}

float main(float4 position : SV_Position) : SV_Target
{
    read_levels();
    return green_at(int2(position.xy));
}
#else
Texture2D<float> green : register(t1);  // X-Trans's (GREEN's)

// Malvar, He and Cutler's 5 x 5 kernels: a Bayer mosaic's missing colours at
// (x, y).
float3 bayer(int x, int y)
{
    const int c = colour_at(x, y);
    const float m = site(x, y);
    const float cross = site(x - 1, y) + site(x + 1, y) + site(x, y - 1) + site(x, y + 1);
    const float far = site(x - 2, y) + site(x + 2, y) + site(x, y - 2) + site(x, y + 2);
    const float diag = site(x - 1, y - 1) + site(x + 1, y - 1) + site(x - 1, y + 1) + site(x + 1, y + 1);
    if (c == 1)
    {
        // Green: the colour on either side, and the one above and below.
        const float h = (5 * m + 4 * (site(x - 1, y) + site(x + 1, y)) - diag - (site(x - 2, y) + site(x + 2, y)) +
                         0.5 * (site(x, y - 2) + site(x, y + 2))) / 8;
        const float v = (5 * m + 4 * (site(x, y - 1) + site(x, y + 1)) - diag - (site(x, y - 2) + site(x, y + 2)) +
                         0.5 * (site(x - 2, y) + site(x + 2, y))) / 8;
        const bool red_row = colour_at(x + 1, y) == 0;
        return float3(red_row ? h : v, m, red_row ? v : h);
    }
    const float g = (4 * m + 2 * cross - far) / 8;
    const float other = (6 * m + 2 * diag - 1.5 * far) / 8;  // blue at red, red at blue
    return c == 0 ? float3(m, g, other) : float3(other, g, m);
}

// X-Trans's red and blue at (x, y), with its green (GREEN's): each the
// green here plus the difference of colour and green at the photosites of
// that colour within two, weighed by nearness and by how alike their green
// is to this one's (within about 1% counts as alike), so a colour isn't
// carried across an edge in the green. Colour differences change slowly
// where the brightness doesn't: no moiré from the sparser red and blue.
float from_differences(int x, int y, int k, float g)
{
    float total = 0, weights = 0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i)
        {
            const int2 q = int2(x + i, y + j);
            if (!inside(q) || colour_at(q.x, q.y) != k)
                continue;
            const float gq = green.Load(int3(q, 0));
            const float w = 1 / ((i * i + j * j) * (1e-4 + 0.005 * (g + gq) + abs(g - gq)));
            total += w * (site(q.x, q.y) - gq);
            weights += w;
        }
    return weights > 0 ? g + total / weights : g;
}

float3 xtrans(int x, int y)
{
    const int c = colour_at(x, y);
    const float own = site(x, y), g = c == 1 ? own : green.Load(int3(x, y, 0));
    return float3(c == 0 ? own : from_differences(x, y, 0, g), g, c == 2 ? own : from_differences(x, y, 2, g));
}

// `share`: of each colour, the share of its photosites within two of (x, y)
// (all the demosaic reads) that are past its limit, and `bound` the least
// those read (estimate_censor()), for clipped(); but the colour of the
// pixel's own photosite is that photosite alone: measured or censored. A
// Bayer's shares are what its kernels read (estimate_bayer_shares()).
#define DEVELOPED
float3 developed(int2 o, out float3 share, out float3 bound)
{
    const int x = map_x.x * o.x + map_x.y * o.y + map_x.z;
    const int y = map_y.x * o.x + map_y.y * o.y + map_y.z;
    // Grey: no other colours to find.
    float3 rgb = flags.y ? site(x, y) : cfa.w == 6 ? xtrans(x, y) : bayer(x, y);
    const float3 limits = clip_limits();
    bound = 1e30;
    share = 0;
    if (censoring())
    {
        // Arrays keep the colour-indexed accumulators addressable in FXC.
        float past[3] = {0, 0, 0}, n[3] = {0, 0, 0}, low[3] = {1e30, 1e30, 1e30};
        float cen[25];  // each photosite's within two: censored or not, row by row
        for (int j = -2; j <= 2; ++j)
            for (int i = -2; i <= 2; ++i)
            {
                const float v = site(x + i, y + j);
                const int c = flags.y ? 1 : colour_at(clamp(x + i, 0, size.x - 1), clamp(y + j, 0, size.y - 1));
                float censored, least;
                estimate_censor(v, limits[c], censored, least);
                past[c] += censored;
                n[c] += 1;
                cen[(j + 2) * 5 + i + 2] = censored;
                if (censored > 0)
                    low[c] = min(low[c], least);
            }
        bound = float3(low[0], low[1], low[2]);
        share = float3(past[0], past[1], past[2]) / max(float3(n[0], n[1], n[2]), 1.0);
        if (flags.y)
            share = share.g, bound = bound.g;
        if (!flags.y && cfa.w != 6)
        {
            // A Bayer's: by what its kernels read (estimate_bayer_shares()).
            float s[3];
            estimate_bayer_shares(cen, colour_at(clamp(x, 0, size.x - 1), clamp(y, 0, size.y - 1)),
                                  colour_at(clamp(x + 1, 0, size.x - 1), clamp(y, 0, size.y - 1)) == 0, s);
            share = float3(s[0], s[1], s[2]);
        }
        // The pixel's own photosite is its colour's reading here, not one
        // of those around interpolated in: measured, or censored, as it is.
        // This bound holds for Bayer too, after its kernel shares are found.
        const int own = flags.y ? 1 : colour_at(clamp(x, 0, size.x - 1), clamp(y, 0, size.y - 1));
        float censored, least;
        estimate_censor(site(x, y), limits[own], censored, least);
        if (flags.y)
            share = censored, bound = censored > 0 ? least : 1e30;
        else
        {
            share = float3(own == 0 ? censored : share.r, own == 1 ? censored : share.g, own == 2 ? censored : share.b);
            const float own_bound = censored > 0 ? least : 1e30;
            bound = float3(own == 0 ? own_bound : bound.r, own == 1 ? own_bound : bound.g, own == 2 ? own_bound : bound.b);
        }
    }
    if (size.w && word(2) != 2)
    {
        const float3 gain = rgb_gain(x, y);
        rgb *= gain;
        bound *= gain;
    }
    return rgb;
}

// lensfun's vignetting brings up what the lens darkened: what the sensor
// clipped stays clipped, and the rest may pass it.
float3 after_clip(float3 rgb, int2 o)
{
    if (size.w && word(2) == 2)
        rgb *= rgb_gain(map_x.x * o.x + map_x.y * o.y + map_x.z, map_y.x * o.x + map_y.y * o.y + map_y.z);
    return rgb;
}
#endif
#endif
#endif

#ifdef DEVELOPED
#ifdef PILE
// Where each colour's values pile up over the developed image, for
// clip.hlsl's LIMIT: every fourth pixel each way, each colour counted by its
// level from kPileFrom of the way to its clip.
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    read_levels();
    const int2 o = int2(id.xy) * 4;
    if (o.x >= map_x.w || o.y >= map_y.w)
        return;
    const float3 stops = clip_stops();
    float3 share, bound;
    const float3 near = developed(o, share, bound) / stops;
    [unroll] for (uint c = 0; c < 3; ++c)
        if (near[c] >= kPileFrom && near[c] < 1)
            clips.InterlockedAdd((kClipPile + c * kPileBins + min(uint((near[c] - kPileFrom) / (1 - kPileFrom) * kPileBins), kPileBins - 1)) * 4, 1u);
}
#elif defined(DRIFT)
// drift.hlsli's tiles, for clip.hlsl's DRIFT: a thread a tile of kDriftTile
// x kDriftTile output pixels, from every other one each way, as the develop
// has them before anything's estimated: its mean (balanced) and whether
// it's kept (w 1), then each colour's log2 noise variance (drift_tile()),
// two float4s each, row by row.
RWByteAddressBuffer tiles : register(u3);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    read_levels();
    const int across = (map_x.w + kDriftTile - 1) / kDriftTile, down = (map_y.w + kDriftTile - 1) / kDriftTile;
    if (int(id.x) >= across || int(id.y) >= down)
        return;
    const float3 stops = clip_stops();
    float t[kDriftTileSums];
    drift_start(t);
    for (int j = 1; j < kDriftTile; j += 2)
        for (int i = 1; i < kDriftTile; i += 2)
        {
            const int2 o = int2(id.xy) * kDriftTile + int2(i, j);
            if (o.x >= map_x.w || o.y >= map_y.w)
            {
                drift_sample(t, 0, 0, 0, false);
                continue;
            }
            float3 share, bound;
            const float3 v = developed(o, share, bound);
            drift_sample(t, v.r, v.g, v.b, all(share <= 0) && all(v < stops));
        }
    float mean[3], noise[3];
    const bool kept = drift_tile(t, mean, noise);
    const uint at = (id.y * uint(across) + id.x) * 32;
    tiles.Store4(at, asuint(float4(mean[0], mean[1], mean[2], kept ? 1 : 0)));
    tiles.Store4(at + 16, asuint(float4(noise[0], noise[1], noise[2], 0)));
}
#elif defined(GATHER)
// The colour field's measures, a texel each kRatioCell x kRatioCell output
// pixels, from every kRatioStep-th of them each way: flags.w 0, the
// log2 ratios of red and of blue to green; 1, of red to blue and the log2
// magnitude; each its sureness-weighed mean and its weight. A ratio is
// measured where both its colours are (sureness: the ratio's variance the
// sum of its colours'). flags.z 1: those only. 2: with the field from 1 at
// t2 and t3, a ratio where one colour is censored and estimated from the
// other two (measured) counts too, as half a measurement, and the
// magnitude is measured where any colour is (estimate.hlsli's fit, from the
// first field's chromaticity), weighed by how sure its measured colours
// are; highlight.hlsl then spreads both, the magnitude as a thin plate.
float4 main(float4 position : SV_Position) : SV_Target
{
    read_levels();
    if (!censoring())
        discard;  // nothing to estimate, no field: nothing written (nor read)
    const float3 stops = clip_stops(), g = to_balance();
    const int2 cell = int2(position.xy) * kRatioCell;
    float4 sum = 0;
    float2 weight = 0;
    for (int j = kRatioStep / 2; j < kRatioCell; j += kRatioStep)
        for (int i = kRatioStep / 2; i < kRatioCell; i += kRatioStep)
        {
            const int2 o = min(cell + int2(i, j), int2(map_x.w, map_y.w) - 1);
            float3 share, bound;
            const float3 c = clamp(developed(o, share, bound), 0, stops);
            const bool3 known = bool3(share.r <= 0 && c.r > 0, share.g <= 0 && c.g > 0, share.b <= 0 && c.b > 0);
            const uint measured = uint(known.r) + uint(known.g) + uint(known.b);
            float3 v = c, q = 1;
            if (flags.z == 2 && measured >= 1)
            {
                float continuation;
                q = chromaticity_at(o, continuation);
                v = estimated(c, share, bound, q, continuation);
            }
            [unroll] for (uint k = 0; k < 2; ++k)
            {
                if (flags.w && k == 1)
                    break;
                const uint x = flags.w ? 0 : k * 2, y = flags.w ? 2 : 1;
                float w = 0;
                if (known[x] && known[y])
                    w = 1 / (g[x] / c[x] + g[y] / c[y]);
                else if (flags.z == 2 && measured == 2 && (share[x] >= 1 || share[y] >= 1) && v[x] > 0 && v[y] > 0)
                    w = 0.5 / (g[x] / v[x] + g[y] / v[y]);  // one estimated from the other two
                if (w > 0)
                {
                    sum[k * 2] += w * log2(v[x] / v[y]);
                    weight[k] += w;
                }
            }
            if (flags.w && flags.z == 2 && measured >= 1)
            {
                // The fit's magnitude: the measured colours' log, less the chromaticity's, weighed.
                const float3 sure = sureness(c) * float3(known);
                const float3 lv = log2(max(c, 1e-30)) - log2(q);
                const float w = sure.r + sure.g + sure.b;
                sum[2] += dot(sure, lv);
                weight[1] += w;
            }
        }
    return float4(weight[0] > 0 ? sum[0] / weight[0] : 0, weight[0], weight[1] > 0 ? sum[2] / weight[1] : 0, weight[1]);
}
#else
float4 main(float4 position : SV_Position) : SV_Target
{
    read_levels();
    const int2 o = int2(position.xy);
    float3 share, bound;
    const float3 v = developed(o, share, bound);
    return float4(after_clip(clipped(v, share, bound, o), o) / as_shot.rgb, 1);
}
#endif
#endif
