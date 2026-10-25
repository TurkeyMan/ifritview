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
    float4 black;        // per colour (R, G, B), data units
    float4 as_shot;      // R, G, B multipliers, the least 1; w: saturation in data units
    int4 map_x;          // photosite x = map_x.x * out.x + map_x.y * out.y + map_x.z
    int4 map_y;          // photosite y = map_y.x * out.x + map_y.y * out.y + map_y.z
    int4 size;           // the mosaic's width, height; z: black levels measured on the GPU (levels)
};

Texture2D<float> mosaic : register(t0);  // unorm 16: data / 65535
ByteAddressBuffer levels : register(t8);  // ljpeg_black's: sum R G B G2, count R G B G2, zeros

static float3 black_level;

// The photosite at (x, y) as a balanced value, 1 = saturation.
float site(int x, int y)
{
    x = clamp(x, 0, size.x - 1);
    y = clamp(y, 0, size.y - 1);
    const int c = colour[(y & 1) * 2 + (x & 1)];
    const float d = mosaic.Load(int3(x, y, 0)) * 65535;
    return (d - black_level[c]) / (as_shot.w - black_level[c]) * as_shot[c];
}

// As LibRaw's: measured where the sensor is masked, if those were read and
// not mostly zero; else what the file says.
float3 black_levels()
{
    if (size.z != 0)
    {
        const uint4 sum = levels.Load4(0), count = levels.Load4(16);
        const uint zero = levels.Load(32);
        if (zero < count.x && count.y && count.z && count.w)
            return float3(sum.x / count.x, sum.y / count.y, sum.z / count.z);
    }
    return black.rgb;
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
    rgb = clamp(rgb, 0, 1);
    return float4(rgb / as_shot.rgb, 1);
}
