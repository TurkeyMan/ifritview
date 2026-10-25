// A DNG's WarpRectilinear (dng_opcodes.h): the lens's distortion and
// lateral chromatic aberration taken out. The develop has made the whole
// active area (camera RGB, unturned); each output pixel, in the image as
// shown, goes back to its place in that (map_x, map_y), then to where the
// DNG says the lens put it, each colour its own place for the chromatic
// aberration, read there with Catmull-Rom's cubic.
cbuffer Params : register(b0)
{
    int4 map_x;  // active-area x = map_x.x * out.x + map_x.y * out.y + map_x.z
    int4 map_y;
    int4 size;   // the active area's width, height
};

Texture2D<float4> developed : register(t0);
ByteAddressBuffer corrections : register(t8);

float word_f(uint i)
{
    return asfloat(corrections.Load(i * 4));
}

// Where the lens put what belongs at (x, y), for plane p (the DNG spec's
// formula: radial kr0..kr3, tangential kt0, kt1, about the centre, in
// units of the distance to the farthest corner).
float2 source(float2 at, uint p)
{
    const float2 c = float2(word_f(17), word_f(18));
    const float m = word_f(19);
    const uint k = 20 + p * 6;
    const float2 d = (at - c) / m;
    const float r2 = dot(d, d);
    const float f = word_f(k) + r2 * (word_f(k + 1) + r2 * (word_f(k + 2) + r2 * word_f(k + 3)));
    const float kt0 = word_f(k + 4), kt1 = word_f(k + 5);
    const float2 t = float2(kt0 * 2 * d.x * d.y + kt1 * (r2 + 2 * d.x * d.x), kt1 * 2 * d.x * d.y + kt0 * (r2 + 2 * d.y * d.y));
    return c + m * (f * d + t);
}

float4 texel(int x, int y)
{
    return developed.Load(int3(clamp(x, 0, size.x - 1), clamp(y, 0, size.y - 1), 0));
}

float4 weights(float t)  // Catmull-Rom's, for the four texels around
{
    const float t2 = t * t, t3 = t2 * t;
    return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1, -1.5 * t3 + 2 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}

float4 cubic(float2 at)
{
    const float2 b = floor(at), t = at - b;
    const float4 wx = weights(t.x), wy = weights(t.y);
    float4 v = 0;
    for (int j = 0; j < 4; ++j)
    {
        float4 row = 0;
        for (int i = 0; i < 4; ++i)
            row += wx[i] * texel(int(b.x) - 1 + i, int(b.y) - 1 + j);
        v += wy[j] * row;
    }
    return v;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 o = int2(position.xy);
    const float2 at = float2(map_x.x * o.x + map_x.y * o.y + map_x.z, map_y.x * o.x + map_y.y * o.y + map_y.z);
    float3 rgb;
    if (corrections.Load(16 * 4) == 1)
        rgb = cubic(source(at, 0)).rgb;
    else
        rgb = float3(cubic(source(at, 0)).r, cubic(source(at, 1)).g, cubic(source(at, 2)).b);
    return float4(max(rgb, 0), 1);
}
