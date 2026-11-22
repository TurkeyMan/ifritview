// What a raw's develop (develop.hlsl) and the measure of its clips
// (clip.hlsl) share: their parameters, the filter pattern, the black level
// and Canon's sRAW as stored.
#ifndef DEVELOP_HLSLI
#define DEVELOP_HLSLI

cbuffer Params : register(b0)
{
    // The filter pattern (RawMosaic's) over 6 x 6 photosites, a whole number
    // of it (Bayer's 2 x 2 or X-Trans's 6 x 6): each site's colour as LibRaw
    // counts them (0 R, 1 G, 2 B, 3 the other G), 2 bits a site, row-major,
    // 16 a word; w: its period (2 or 6).
    uint4 cfa;
    float4 as_shot;      // R, G, B multipliers, the least 1; w: the white the file states, in data units
    int4 map_x;          // photosite x = map_x.x * out.x + map_x.y * out.y + map_x.z
    int4 map_y;          // photosite y = map_y.x * out.x + map_y.y * out.y + map_y.z
    int4 size;           // the mosaic's width, height; z: black levels measured on the GPU (levels); w: the raw's own corrections
    int4 ycc;            // YCC: block height, formula, hue, luma offset (RawMosaic::Ycc)
    int4 ycc_at;         // YCC: the first block's x, y
    float4 ycc_mul;      // YCC: sraw_mul; w: the guided fill's regularisation, in luma units squared
    int4 flags;          // x: the data is white balanced already (the camera did it): not again; y: grey, no filter (a monochrome sensor)
    float4 black[9];     // per site of the 6 x 6, data units (LINEAR, YCC: per channel, R, G, B in the first)
};

// The as-shot balance, as it's still to be applied to the data.
float3 to_balance()
{
    return flags.x ? 1 : as_shot.rgb;
}

// The pattern's site at photosite (x, y), x and y not negative (past the
// right and bottom edges the pattern goes on). By 6, not the period: a
// constant's remainder is a multiply, a uniform's a long division.
uint site_at(int x, int y)
{
    return uint(y) % 6 * 6 + uint(x) % 6;
}

// LibRaw's colour at a site, as levels counts them.
uint filter_of(uint i)
{
    return cfa[i / 16] >> (i % 16 * 2) & 3;
}

// The colour at (x, y): 0 R, 1 G, 2 B.
int colour_at(int x, int y)
{
    const uint f = filter_of(site_at(x, y));
    return f == 3 ? 1 : int(f);
}

// ljpeg_black's: sum R G B G2, count R G B G2, zeros. A compute pass reads
// it among its buffers.
#ifdef COMPUTE
RWByteAddressBuffer levels : register(u1);
#else
ByteAddressBuffer levels : register(t8);
#endif

// The black level as LibRaw's: measured where the sensor is masked (per
// colour), if those were read and not mostly zero; else what the file says
// for each site of the pattern. Read once a pixel (read_levels).
static bool measured;
static uint4 measured_sum, measured_count;

void read_levels()
{
    measured = false;
    if (size.z != 0)
    {
        measured_sum = levels.Load4(0), measured_count = levels.Load4(16);
        const uint zero = levels.Load(32);
        measured = zero < measured_count.x && measured_count.y && measured_count.z && measured_count.w;
    }
}

// At photosite (x, y), data units.
float black_at(int x, int y)
{
    const uint i = site_at(x, y);
    if (measured)
        return measured_sum[filter_of(i)] / measured_count[filter_of(i)];
    return black[i / 4][i % 4];
}

#ifdef YCC
// Canon's sRAW and mRAW as stored: luma at every photosite, the colour
// differences only at each block's first (2 wide, ycc.x tall).
Texture2D<float4> pixels : register(t0);  // unorm 16: Y, then the colour differences + 8192, as int16

int2 at(int x, int y)
{
    return int2(clamp(x, 0, size.x - 1), clamp(y, 0, size.y - 1));
}

float luma(int x, int y)
{
    return pixels.Load(int3(at(x, y), 0)).r * 65535 - ycc.w;
}

float2 chroma_at(int x, int y)
{
    const float2 v = round(pixels.Load(int3(at(x, y), 0)).gb * 65535);
    return v - 65536 * (v >= 32768) - 8192;  // as int16, less the neutral offset
}

// Camera RGB, data units, from luma Y and colour differences cd (Cb, Cr) by
// Canon's formula and sraw_mul.
float3 ycc_rgb(float Y, float2 cd)
{
    float3 rgb;
    if (ycc.y == 1)
    {
        const float c1 = cd.x * 4 + ycc.z, c2 = cd.y * 4 + ycc.z;
        rgb = Y + float3(50 * c1 + 22929 * c2, -5640 * c1 - 11751 * c2, 29040 * c1 - 101 * c2) / 16384;
    }
    else
        rgb = Y + float3(cd.y, (-778 * cd.x - cd.y * 2048) / 4096, cd.x);
    return rgb * ycc_mul.rgb;
}
#endif

#include "clip_layout.hlsli"

#endif
