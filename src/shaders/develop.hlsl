// A raw's level 0: its mosaic (RawMosaic, one photosite a texel) developed.
// Each output pixel, in the image as shown (the camera's framing, turned as
// it was held), comes from the photosite under it (corrected first, where
// the raw has its own corrections of its mosaic: CORRECT): black taken off
// and scaled to saturation, the as-shot white balance applied (the demosaic
// follows colour edges far better on balanced data), the two missing
// colours found (a Bayer's from Malvar-He-Cutler's 5 x 5 kernels, X-Trans's
// from its green made first, GREEN; none for a monochrome sensor's: grey),
// clipped at saturation (all channels at 1:
// highlights stay neutral), and the balance taken off again. Linear camera
// RGB out; the frame's own white balance and the camera's colour matrix are
// the view's, per frame.
cbuffer Params : register(b0)
{
    // The filter pattern (RawMosaic's) over 6 x 6 photosites, a whole number
    // of it (Bayer's 2 x 2 or X-Trans's 6 x 6): each site's colour as LibRaw
    // counts them (0 R, 1 G, 2 B, 3 the other G), 2 bits a site, row-major,
    // 16 a word; w: its period (2 or 6).
    uint4 cfa;
    float4 as_shot;      // R, G, B multipliers, the least 1; w: saturation in data units
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

float2 chroma_at(int x, int y)
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
            const float2 c = chroma_at(cx, cy);
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
    rgb = (rgb - black[0].rgb) / (as_shot.w - black[0].rgb);
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
    const float3 rgb = (d - black[0].rgb) / (as_shot.w - black[0].rgb);
    return float4(clamp(rgb * to_balance(), 0, 1) / as_shot.rgb, 1);
}
#else
Texture2D<float> mosaic : register(t0);  // unorm 16: data / 65535
ByteAddressBuffer corrections : register(t9);  // the raw's own (a DNG's opcodes, a Phase One's), as dng_opcodes.h lays them out

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

float4 main(float4 position : SV_Position) : SV_Target
{
    read_levels();
    const int2 o = int2(position.xy);
    const int x = map_x.x * o.x + map_x.y * o.y + map_x.z;
    const int y = map_y.x * o.x + map_y.y * o.y + map_y.z;
    // Grey: no other colours to find.
    float3 rgb = flags.y ? site(x, y) : cfa.w == 6 ? xtrans(x, y) : bayer(x, y);
    // lensfun's vignetting brings up what the lens darkened: what the sensor
    // clipped stays clipped (neutral), and the rest may pass it.
    if (size.w && word(2) == 2)
        rgb = clamp(rgb, 0, 1) * rgb_gain(x, y);
    else
    {
        if (size.w)
            rgb *= rgb_gain(x, y);
        rgb = clamp(rgb, 0, 1);
    }
    return float4(rgb / as_shot.rgb, 1);
}
#endif
#endif
#endif
