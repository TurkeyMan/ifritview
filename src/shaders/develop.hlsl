// A raw's level 0: its Bayer mosaic (RawMosaic, one photosite a texel)
// developed. Each output pixel, in the image as shown (the camera's framing,
// turned as it was held), comes from the photosite under it: black taken
// off and scaled to saturation, the as-shot white balance applied (the
// demosaic follows colour edges far better on balanced data), the two
// missing colours from Malvar-He-Cutler's 5 x 5 kernels, clipped at
// saturation (all channels at 1: highlights stay neutral), and the balance
// taken off again. Linear camera RGB out; the frame's own white balance and
// the camera's colour matrix are the view's, per frame.
cbuffer Params : register(b0)
{
    int4 colour;         // the filter over (row & 1, col & 1), row-major: 0 R, 1 G, 2 B
    float4 black;        // per photosite of the 2 x 2 (as colour), data units
    float4 as_shot;      // R, G, B multipliers, the least 1; w: saturation in data units
    int4 map_x;          // photosite x = map_x.x * out.x + map_x.y * out.y + map_x.z
    int4 map_y;          // photosite y = map_y.x * out.x + map_y.y * out.y + map_y.z
    int4 size;           // the mosaic's width, height; z: black levels measured on the GPU (levels); w: a DNG's corrections
    int4 filter;         // per photosite: LibRaw's colour (0 R, 1 G, 2 B, 3 the other G), as levels counts them
    int4 ycc;            // YCC: block height, formula, hue, luma offset (RawMosaic::Ycc)
    int4 ycc_at;         // YCC: the first block's x, y
    float4 ycc_mul;      // YCC: sraw_mul; w: the guided fill's regularisation, in luma units squared
};

ByteAddressBuffer levels : register(t8);  // ljpeg_black's: sum R G B G2, count R G B G2, zeros

#ifdef YCC
// Canon's sRAW and mRAW as stored: luma at every photosite, the colour
// differences only at each block's first (2 wide, ycc.x tall). The colour is
// filled in guided by the luma: over the blocks around, a least-squares line
// of colour against the blocks' mean luma, evaluated at this photosite's own
// luma, so colour changes where the brightness does instead of bleeding
// across edges (He et al.'s guided filter, at the colour samples). Then
// Canon's formula to camera RGB, and black, scale and the clip under the
// as-shot balance as for the others.
Texture2D<float4> pixels : register(t0);  // unorm 16: Y, then the colour differences + 8192, as int16

int2 at(int x, int y)
{
    return int2(clamp(x, 0, size.x - 1), clamp(y, 0, size.y - 1));
}

float luma(int x, int y)
{
    return pixels.Load(int3(at(x, y), 0)).r * 65535 - ycc.w;
}

float2 colour_at(int x, int y)
{
    const float2 v = round(pixels.Load(int3(at(x, y), 0)).gb * 65535);
    return v - 65536 * (v >= 32768) - 8192;  // as int16, less the neutral offset
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 o = int2(position.xy);
    const int x = clamp(map_x.x * o.x + map_x.y * o.y + map_x.z, 0, size.x - 1);
    const int y = clamp(map_y.x * o.x + map_y.y * o.y + map_y.z, 0, size.y - 1);
    const float Y = luma(x, y);
    const int bw = 2, bh = ycc.x;
    const int bx = int(floor(float(x - ycc_at.x) / bw)), by = int(floor(float(y - ycc_at.y) / bh));
    float n = 0, sy = 0, syy = 0;
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
            const float2 c = colour_at(cx, cy);
            n += 1;
            sy += m;
            syy += m * m;
            sc += c;
            syc += m * c;
        }
    const float mean_y = sy / n, var_y = syy / n - mean_y * mean_y;
    const float2 mean_c = sc / n, cov = syc / n - mean_y * mean_c;
    const float2 a = cov / (var_y + ycc_mul.w), b = mean_c - a * mean_y;
    const float2 cd = a * Y + b;  // the colour differences here: Cb, Cr
    float3 rgb;
    if (ycc.y == 1)
    {
        const float c1 = cd.x * 4 + ycc.z, c2 = cd.y * 4 + ycc.z;
        rgb = Y + float3(50 * c1 + 22929 * c2, -5640 * c1 - 11751 * c2, 29040 * c1 - 101 * c2) / 16384;
    }
    else
        rgb = Y + float3(cd.y, (-778 * cd.x - cd.y * 2048) / 4096, cd.x);
    rgb *= ycc_mul.rgb;
    rgb = (rgb - black.rgb) / (as_shot.w - black.rgb);
    return float4(clamp(rgb * as_shot.rgb, 0, 1) / as_shot.rgb, 1);
}
#elif defined(LINEAR)
// Already R, G, B a photosite (a linear DNG, Canon's sRAW): black off and
// scaled per channel, clipped at saturation under the as-shot balance as the
// mosaic's are, no demosaic. `black` is per channel here.
Texture2D<float4> pixels : register(t0);  // unorm 16: data / 65535

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 o = int2(position.xy);
    const int x = clamp(map_x.x * o.x + map_x.y * o.y + map_x.z, 0, size.x - 1);
    const int y = clamp(map_y.x * o.x + map_y.y * o.y + map_y.z, 0, size.y - 1);
    const float3 d = pixels.Load(int3(x, y, 0)).rgb * 65535;
    const float3 rgb = (d - black.rgb) / (as_shot.w - black.rgb);
    return float4(clamp(rgb * as_shot.rgb, 0, 1) / as_shot.rgb, 1);
}
#else
Texture2D<float> mosaic : register(t0);  // unorm 16: data / 65535
ByteAddressBuffer corrections : register(t9);  // a DNG's opcodes, as dng_opcodes.h lays them out

static float4 black_level;
static float4 shading;  // each photosite of the 2 x 2's lens shading gain, here (OpcodeList2)

float word_f(uint i)
{
    return asfloat(corrections.Load(i * 4));
}

// The gain map at word `d` (its 16 words) at photosite (x, y), plane `p`,
// as the DNG SDK interpolates it: bilinear between its points, held at
// the edges; 1 where it doesn't apply.
float map_gain(uint d, int x, int y, uint p)
{
    // top, left, bottom, right; plane, planes, row pitch, column pitch;
    // points down, across, planes in the map, where its gains start
    const uint4 area = corrections.Load4(d * 4), h1 = corrections.Load4(d * 4 + 16), h2 = corrections.Load4(d * 4 + 32);
    const float4 grid = asfloat(corrections.Load4(d * 4 + 48));  // spacing v, h, origin v, h
    if (y < int(area.x) || x < int(area.y) || y >= int(area.z) || x >= int(area.w) || (uint(y) - area.x) % h1.z ||
        (uint(x) - area.y) % h1.w || p < h1.x || p >= h1.x + h1.y)
        return 1;
    const uint rows = h2.x, cols = h2.y, planes = h2.z;
    const uint q = min(p - h1.x, planes - 1);
    const float fy = clamp(((y + 0.5 - area.x) / (area.z - area.x) - grid.z) / grid.x, 0, rows - 1);
    const float fx = clamp(((x + 0.5 - area.y) / (area.w - area.y) - grid.w) / grid.y, 0, cols - 1);
    const uint r0 = uint(fy), c0 = uint(fx), r1 = min(r0 + 1, rows - 1), c1 = min(c0 + 1, cols - 1);
    const float ty = fy - r0, tx = fx - c0;
    const uint g = h2.w;
    const float a = word_f(g + (r0 * cols + c0) * planes + q), b = word_f(g + (r0 * cols + c1) * planes + q);
    const float c = word_f(g + (r1 * cols + c0) * planes + q), e = word_f(g + (r1 * cols + c1) * planes + q);
    return lerp(lerp(a, b, tx), lerp(c, e, tx), ty);
}

// OpcodeList2's maps multiplied, at photosite (x, y).
float mosaic_gain(int x, int y)
{
    float g = 1;
    const uint n = corrections.Load(0);
    for (uint i = 0; i < n; ++i)
        g *= map_gain(48 + i * 16, x, y, 0);
    return g;
}

// OpcodeList3's, on R, G, B at (x, y): its gain maps and radial vignette.
float3 rgb_gain(int x, int y)
{
    float3 g = 1;
    const uint n2 = corrections.Load(0), n3 = corrections.Load(4);
    for (uint i = 0; i < n3; ++i)
    {
        const uint d = 48 + (n2 + i) * 16;
        g *= float3(map_gain(d, x, y, 0), map_gain(d, x, y, 1), map_gain(d, x, y, 2));
    }
    if (corrections.Load(8))
    {
        const float dx = x + 0.5 - word_f(9), dy = y + 0.5 - word_f(10);
        const float r2 = (dx * dx + dy * dy) * word_f(11);
        g *= 1 + r2 * (word_f(4) + r2 * (word_f(5) + r2 * (word_f(6) + r2 * (word_f(7) + r2 * word_f(8)))));
    }
    return g;
}

// The photosite at (x, y) as a balanced value, 1 = saturation.
float site(int x, int y)
{
    x = clamp(x, 0, size.x - 1);
    y = clamp(y, 0, size.y - 1);
    const int i = (y & 1) * 2 + (x & 1), c = colour[i];
    const float d = mosaic.Load(int3(x, y, 0)) * 65535;
    return (d - black_level[i]) / (as_shot.w - black_level[i]) * shading[i] * as_shot[c];
}

// Each photosite of the 2 x 2's, as LibRaw's: measured where the sensor is
// masked (per colour), if those were read and not mostly zero; else what the
// file says.
float4 black_levels()
{
    if (size.z != 0)
    {
        const uint4 sum = levels.Load4(0), count = levels.Load4(16);
        const uint zero = levels.Load(32);
        if (zero < count.x && count.y && count.z && count.w)
        {
            float4 b;
            for (int i = 0; i < 4; ++i)
                b[i] = sum[filter[i]] / count[filter[i]];
            return b;
        }
    }
    return black;
}

int colour_at(int x, int y)
{
    return colour[(y & 1) * 2 + (x & 1)];
}

float4 main(float4 position : SV_Position) : SV_Target
{
    black_level = black_levels();
    const int2 o = int2(position.xy);
    const int x = map_x.x * o.x + map_x.y * o.y + map_x.z;
    const int y = map_y.x * o.x + map_y.y * o.y + map_y.z;
    // The lens shading barely changes over the few photosites the demosaic
    // reads: each colour's, once, at its photosite in this 2 x 2.
    shading = 1;
    if (size.w)
        for (int i = 0; i < 4; ++i)
            shading[i] = mosaic_gain((x & ~1) + (i & 1), (y & ~1) + (i >> 1));
    const int c = colour_at(x, y);
    const float m = site(x, y);
    const float cross = site(x - 1, y) + site(x + 1, y) + site(x, y - 1) + site(x, y + 1);
    const float far = site(x - 2, y) + site(x + 2, y) + site(x, y - 2) + site(x, y + 2);
    const float diag = site(x - 1, y - 1) + site(x + 1, y - 1) + site(x - 1, y + 1) + site(x + 1, y + 1);
    float3 rgb;
    if (c == 1)
    {
        // Green: the colour on either side, and the one above and below.
        const float h = (5 * m + 4 * (site(x - 1, y) + site(x + 1, y)) - diag - (site(x - 2, y) + site(x + 2, y)) +
                         0.5 * (site(x, y - 2) + site(x, y + 2))) / 8;
        const float v = (5 * m + 4 * (site(x, y - 1) + site(x, y + 1)) - diag - (site(x, y - 2) + site(x, y + 2)) +
                         0.5 * (site(x - 2, y) + site(x + 2, y))) / 8;
        const bool red_row = colour_at(x + 1, y) == 0;
        rgb = float3(red_row ? h : v, m, red_row ? v : h);
    }
    else
    {
        const float g = (4 * m + 2 * cross - far) / 8;
        const float other = (6 * m + 2 * diag - 1.5 * far) / 8;  // blue at red, red at blue
        rgb = c == 0 ? float3(m, g, other) : float3(other, g, m);
    }
    if (size.w)
        rgb *= rgb_gain(x, y);
    rgb = clamp(rgb, 0, 1);
    return float4(rgb / as_shot.rgb, 1);
}
#endif
