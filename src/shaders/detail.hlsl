// Noise reduction and sharpening (detail.h), a region of level 0 at a time:
// the passes GpuView::filter_detail queues, one mode each. A region's
// texels are image positions from `origin`; reads past its edges clamp to
// them (the image's own at the image's edges). Step for step what
// detail.cpp's apply_detail() does on the CPU.
#include "detail.hlsli"

#define DETAIL_ENCODE 0   // level 0 (t0) to the encoding
#define DETAIL_SMOOTH 1   // t0 smoothed by the B3 spline along `axis`, `step` apart: colour guided by luminance
#define DETAIL_SHRINK 2   // the sum so far (t2), plus scale `step`'s detail (t0 less t1) shrunk
#define DETAIL_COMBINE 3  // the coarsest (t0) and the details kept (t1) together
#define DETAIL_BLUR 4     // t0's luminance by the sharpening Gaussian along `axis`
#define DETAIL_DECODE 5   // the encoding (t0), sharpened (t1 its blur), back to level 0's data (t2's alpha; t3 the decode's table)
#define DETAIL_TABLE 6    // the decode's table: texel i, each colour's shift at its inverse table's entry i (t0 DETAIL_SHAPING's)
#define DETAIL_SHAPING 7  // what the scales and the sharpening make of the noise: texel (j, p) (detail_shaping_texel())

cbuffer Params : register(b0)
{
    int mode;
    int axis;
    int step;
    int sharpening;     // decode: sharpen
    int2 origin;        // the image position of the region's texel (0, 0)
    int2 size;          // the region's
    int2 out_origin;    // decode: the image position of the target's pixel (0, 0)
    int2 image_size;    // level 0's
    float4 as_shot;     // what level 0 is white balanced with (its data / these)
    float4 luminance;   // shrink: scales 0..3's thresholds, multiples of the noise
    float4 colour;
    float4 coarsest;    // scale 4's: luminance, colour; z: colour's guide (DetailFilter::guide)
    float4 sharpen;     // amount, the Gaussian's sigma, halo
    float4 mask;        // low, high (0: none)
    float4 support;     // shrink: scales 0..3's support from the next, multiples of the noise (DetailFilter::support)
    float4 model;       // x: 1, encoded by the noise model (noise.hlsl's), else as it was
};

Texture2D<float4> a : register(t0);
Texture2D<float4> b : register(t1);
Texture2D<float4> c : register(t2);
Texture2D<float4> table : register(t3);  // DETAIL_TABLE's
ByteAddressBuffer noise : register(t8);  // noise.hlsl's

// Colour c's inverse table's entry i: its average (noise.hlsl's).
precise float table_average(uint c, uint i)
{
    const float first = asfloat(noise.Load((kNoiseModel + 11 + c) * 4)), last = asfloat(noise.Load((kNoiseModel + 14 + c) * 4));
    return first + (last - first) * i / (kInverseSize - 1);
}

// Colour c decoded from stabilised value d: f's own inverse, moved by the
// decode's table (DETAIL_TABLE) there, between its entries linearly; past
// its ends, by the last.
float decoded(float d, float offset, uint c)
{
    const float first = asfloat(noise.Load((kNoiseModel + 11 + c) * 4)), last = asfloat(noise.Load((kNoiseModel + 14 + c) * 4));
    const float t = clamp((d - first) / (last - first) * (kInverseSize - 1), 0.0, kInverseSize - 1.0);
    const uint i = min(uint(t), kInverseSize - 2);
    return detail_inverse(d, offset) + lerp(table.Load(int3(i, 0, 0))[c], table.Load(int3(i + 1, 0, 0))[c], t - i);
}

// kBandAutocorr's pair t at lag l.
float band(uint t, int l)
{
    l = abs(l);
    return l < kBandLags ? kBandAutocorr[t][l] : 0;
}

// The measured correlation of colour c at a pixel against colour d (dx, dy)
// on (noise.hlsl's kNoiseRho), from the half plane it's kept in.
float detail_cross(uint c, uint d, int dx, int dy)
{
    if (dy < 0 || (dy == 0 && dx < 0))
    {
        dx = -dx, dy = -dy;
        const uint t = c;
        c = d, d = t;
    }
    return asfloat(noise.Load((kNoiseRho + detail_series(c, d) * kSpatialLags + detail_lag_index(dx, dy)) * 4));
}

// The sharpening's Gaussian's taps (half_ each way, 9 at most), and with
// itself.
void detail_gaussian(int half_, out float g[19], out float gg[37])
{
    float total = 0;
    for (int k = 0; k < 19; ++k)
        g[k] = k <= 2 * half_ ? exp(-0.5 * (k - half_) * (k - half_) / max(sharpen.y * sharpen.y, 1e-6)) : 0, total += g[k];
    for (int k2 = 0; k2 < 19; ++k2)
        g[k2] /= total;
    for (int m = 0; m < 37; ++m)
    {
        gg[m] = 0;
        for (int a = max(0, m - 18); a <= min(m, 18); ++a)
            gg[m] += g[a] * g[m - a];
    }
}

// Colours cd's measured correlation a lag on, averaged both ways; else
// independent noise's.
float detail_rho(uint2 cd, int dx, int dy, bool measured)
{
    return measured ? (detail_cross(cd.x, cd.y, dx, dy) + detail_cross(cd.y, cd.x, dx, dy)) / 2 : (dx == 0 && dy == 0 ? 1.0 : 0.0);
}

// DETAIL_SHAPING's texel (j, p), detail.cpp's noise_shaping(): what the
// scales and the sharpening make of the noise of pair of colours p, measured
// per unit of its deviations' product (its correlation itself summed, at a
// pixel or a lag on alike), else of its covariance at a pixel. j < 6: scale
// j's detail, c_j - c_j+1, in two dimensions P_j x P_j - P_j+1 x P_j+1 (the
// coarsest P_5 x P_5): its autocorrelation a sum of products of
// kBandAutocorr's, so the noise has in it the correlation summed over the
// lags times that (k); the sharpening's high pass, (1 - G x G) of luminance
// (G the Gaussian's taps), of it (d) and against it (x), alike with each A
// convolved with G (and G with itself). j = 6: the whole high pass's, of the
// pixel's own (D, X). Once per image and setting, not per pixel.
float4 detail_shaping_texel(int j, uint p)
{
    const int r = kSpatialReach;
    const bool measured = noise.Load((kNoisePixel + 3) * 4) != 0;
    const uint2 cd = detail_pair(p);
    const int half_ = sharpening ? int(ceil(3 * sharpen.y)) : 0;
    float g[19], gg[37];
    detail_gaussian(half_, g, gg);
    if (j == 6)
    {
        float total_d = 0, total_x = 0;
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
            {
                const float gx = abs(dx) <= half_ ? g[dx + half_] : 0, gy = abs(dy) <= half_ ? g[dy + half_] : 0;
                const float ggx = abs(dx) <= 2 * half_ ? gg[dx + 2 * half_] : 0, ggy = abs(dy) <= 2 * half_ ? gg[dy + 2 * half_] : 0;
                const float rh = detail_rho(cd, dx, dy, measured), one = dx == 0 && dy == 0 ? 1.0 : 0.0;
                total_d += rh * (one - 2 * gx * gy + ggx * ggy), total_x += rh * (one - gx * gy);
            }
        return float4(total_d, total_x, 0, 0);
    }
    float sk = 0, sq = 0, sqq = 0;
    const int count = j < 5 ? 3 : 1;
    for (int u = 0; u < count; ++u)
    {
        const uint t = u == 0 ? j : u == 1 ? 6 + j : j + 1;
        const float coefficient = u == 1 ? -2 : 1;
        float A[13], GA[13], GGA[13];
        for (int l = -r; l <= r; ++l)
        {
            float ga = 0, gga = 0;
            for (int k = -half_; k <= half_; ++k)
                ga += g[k + half_] * band(t, l - k);
            for (int k3 = -2 * half_; k3 <= 2 * half_; ++k3)
                gga += gg[k3 + 2 * half_] * band(t, l - k3);
            A[l + r] = band(t, l), GA[l + r] = ga, GGA[l + r] = gga;
        }
        for (int dy2 = -r; dy2 <= r; ++dy2)
            for (int dx2 = -r; dx2 <= r; ++dx2)
            {
                const float rh = coefficient * detail_rho(cd, dx2, dy2, measured);
                sk += rh * A[dx2 + r] * A[dy2 + r], sq += rh * GA[dx2 + r] * GA[dy2 + r], sqq += rh * GGA[dx2 + r] * GGA[dy2 + r];
            }
    }
    return float4(sk, sk - 2 * sq + sqq, sk - sq, 0);
}

// The shaping from DETAIL_SHAPING's texture `t`, with the colours' correlation at a pixel and the sharpening.
DetailShaping detail_shaping(Texture2D<float4> t)
{
    DetailShaping w;
    const bool measured = noise.Load((kNoisePixel + 3) * 4) != 0;
    w.measured = measured ? 1 : 0;
    w.correlation = measured ? float3((detail_cross(0, 1, 0, 0) + detail_cross(1, 0, 0, 0)) / 2, (detail_cross(0, 2, 0, 0) + detail_cross(2, 0, 0, 0)) / 2,
                                      (detail_cross(1, 2, 0, 0) + detail_cross(2, 1, 0, 0)) / 2)
                             : float3(0, 0, 0);
    w.amount = sharpening ? sharpen.x : 0, w.halo = sharpen.z;
    [unroll] for (uint p = 0; p < 6; ++p)
    {
        [unroll] for (int j = 0; j < 6; ++j)
        {
            const float4 v = t.Load(int3(j, p, 0));
            w.k[j][p] = v.x, w.d[j][p] = v.y, w.x[j][p] = v.z;
        }
        const float4 whole = t.Load(int3(6, p, 0));
        w.D[p] = whole.x, w.X[p] = whole.y;
    }
    return w;
}

// The encoding's offset per colour: the noise model's, or as it was.
float3 encoding()
{
    return model.x > 0 ? asfloat(noise.Load3(kNoiseModel * 4)) : float3(kDetailFloor, kDetailFloor, kDetailFloor);
}

float4 at(Texture2D<float4> t, int2 p)
{
    return t.Load(int3(clamp(p, 0, size - 1), 0));
}

float shrink(float d, float t)
{
    return d > t ? d - t : d < -t ? d + t : 0;
}

// Its direction, the hue, kept.
float2 shrink(float2 d, float t)
{
    const float m = length(d);
    return m > t ? d * (m - t) / m : 0;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 p = int2(position.xy);
    const int2 dir = axis == 0 ? int2(1, 0) : int2(0, 1);
    const float3 offset = encoding();
    if (mode == DETAIL_ENCODE)
        return float4(detail_encode(a.Load(int3(clamp(p + origin, 0, image_size - 1), 0)).rgb * as_shot.rgb, offset), 0);
    if (mode == DETAIL_SMOOTH)
    {
        // Luminance plainly; colour weighted down by how far each tap's
        // luminance is from the centre's (detail.cpp's smooth()).
        const float noise_l = asfloat(noise.Load(kNoiseSigma * 4));
        const float sigma_l = coarsest.z * noise_l, inv = sigma_l > 0 ? 1 / (2 * sigma_l * sigma_l) : 0;
        const float centre = at(a, p).x;
        float l = 0, w = 0;
        float2 c = 0;
        for (int k = -2; k <= 2; ++k)
        {
            const float3 v = at(a, p + k * step * dir).xyz;
            const float d = v.x - centre, g = kB3[k + 2] * (inv > 0 ? exp(-d * d * inv) : 1);
            l += kB3[k + 2] * v.x;
            c += g * v.yz;
            w += g;
        }
        return float4(l, c / w, 0);
    }
    if (mode == DETAIL_SHRINK)
    {
        const int j = step;
        const float3 d = at(a, p).xyz - at(b, p).xyz;
        const float noise_l = asfloat(noise.Load(kNoiseSigma * 4)), noise_c = asfloat(noise.Load((kNoiseSigma + 1) * 4));
        float tl = (j < 4 ? luminance[j] : coarsest.x) * noise_l;
        const float tc = (j < 4 ? colour[j] : coarsest.y) * noise_c;
        if (j < 4 && support[j] > 0)
        {
            // The next scale's luminance gradient here, against its noise:
            // a fine detail on an edge of the coarser is shrunk less.
            const int gap = 1 << j;
            const float2 g = float2(at(b, p + int2(gap, 0)).x - at(b, p - int2(gap, 0)).x,
                                    at(b, p + int2(0, gap)).x - at(b, p - int2(0, gap)).x);
            const float r = length(g) / 2 / (support[j] * noise_l);
            tl /= 1 + r * r;
        }
        const float3 kept = float3(shrink(d.x, tl), shrink(d.yz, tc));
        return float4((j > 0 ? at(c, p).xyz : 0) + kept, 0);
    }
    if (mode == DETAIL_COMBINE)
        return float4(at(a, p).xyz + at(b, p).xyz, 0);
    if (mode == DETAIL_BLUR)
    {
        const int half_width = int(ceil(3 * sharpen.y));
        float sum = 0, total = 0;
        for (int k = -half_width; k <= half_width; ++k)
        {
            const float w = exp(-0.5 * k * k / (sharpen.y * sharpen.y));
            sum += w * at(a, p + k * dir).x;
            total += w;
        }
        return float4(sum / total, 0, 0, 0);
    }
    if (mode == DETAIL_SHAPING)
        return detail_shaping_texel(p.x, uint(p.y));
    if (mode == DETAIL_TABLE)
    {
        // Each colour's shift from f's own inverse at its inverse table's
        // entry i (detail.hlsli's detail_unbiased()), by the share kept at
        // that entry's level, taken for all three colours (a neutral
        // pixel's): what's per image and per setting, once, so each pixel
        // decodes by a lookup.
        float4 shift = 0;
        if (model.x > 0 && (noise.Load((kNoiseModel + 9) * 4) & 2))
        {
            const uint i = uint(p.x);
            const float noise_l = asfloat(noise.Load(kNoiseSigma * 4)), noise_c = asfloat(noise.Load((kNoiseSigma + 1) * 4));
            float tl[5], tc[5];
            [unroll] for (int j = 0; j < 4; ++j)
                tl[j] = luminance[j] * noise_l, tc[j] = colour[j] * noise_c;
            tl[4] = coarsest.x * noise_l, tc[4] = coarsest.y * noise_c;
            const DetailShaping shaping = detail_shaping(a);
            [unroll] for (uint k = 0; k < 3; ++k)
            {
                precise float d = table_average(k, i), y = detail_inverse(d, offset[k]);
                const float kept = detail_kept(float3(y, y, y), offset, asfloat(noise.Load3(kNoiseLaw * 4)),
                                               asfloat(noise.Load3((kNoiseLaw + 3) * 4)), asfloat(noise.Load3((kNoiseLaw + 6) * 4)),
                                               asfloat(noise.Load3((kNoiseLaw + 9) * 4)), tl, tc, shaping)[k];
                // Keep the small shift directly, as the CPU does: adding it
                // to y and subtracting y again rounds away quiet-colour precision.
                shift[k] = (1 - kept) * (asfloat(noise.Load((kNoiseInverse + k * kInverseSize + i) * 4)) - y);
            }
        }
        return shift;
    }
    // DETAIL_DECODE
    const int2 image = p + out_origin, q = image - origin;
    float3 e = at(a, q).xyz;
    if (sharpening)
    {
        const float d = e.x - at(b, q).x;
        float m = 1;
        if (mask.y > 0)
        {
            const float2 g = float2(at(b, q + int2(1, 0)).x - at(b, q - int2(1, 0)).x, at(b, q + int2(0, 1)).x - at(b, q - int2(0, 1)).x) / 2;
            m = smoothstep(mask.x, mask.y, length(g));
        }
        e.x += sharpen.x * m * d * sharpen.z / (sharpen.z + abs(d));
    }
    // By the model: each colour by the share of its noise taken out
    // (DETAIL_TABLE's, looked up); else f's own inverse.
    float3 rgb = detail_decode(e, offset);
    if (model.x > 0 && (noise.Load((kNoiseModel + 9) * 4) & 2))
    {
        const float3 ch = detail_channels(e);
        [unroll] for (uint k = 0; k < 3; ++k)
            rgb[k] = decoded(ch[k], offset[k], k);
    }
    return float4(rgb / as_shot.rgb, c.Load(int3(clamp(image, 0, image_size - 1), 0)).a);
}
