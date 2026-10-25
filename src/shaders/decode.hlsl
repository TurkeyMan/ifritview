// An image's data, as the file had it, to linear light, premultiplied:
// level 0 of what the view resamples. Mode 0: Image's 8-bit pixels
// (premultiplied BGRA, sRGB-encoded), for 8-bit images. Mode 1: the image's
// own samples, uploaded as they are (16-bit, half or float; a raw's linear
// RGB), straight alpha.
cbuffer Params : register(b0)
{
    int mode;
    int channels;   // mode 1: 1 grey, 2 grey + alpha, 3 RGB, 4 RGBA
    int encoded;    // mode 1: sRGB-encoded (else linear already)
    int is_signed;  // mode 1: -1..1 for 0..1 (shown with 0 at mid-grey)
};

Texture2D<float4> source : register(t0);

float srgb_decode(float v)
{
    return v <= 0.04045 ? v / 12.92 : pow(max(v + 0.055, 0.0) / 1.055, 2.4);
}
float3 decode3(float3 c)
{
    return float3(srgb_decode(c.r), srgb_decode(c.g), srgb_decode(c.b));
}

// By its bits: compilers assume floats aren't NaN and drop isnan().
bool is_nan(float f)
{
    return (asuint(f) & 0x7fffffff) > 0x7f800000;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    float4 v = source.Load(int3(position.xy, 0));
    // NaN shows as nothing.
    v = float4(is_nan(v.r) ? 0 : v.r, is_nan(v.g) ? 0 : v.g, is_nan(v.b) ? 0 : v.b, is_nan(v.a) ? 0 : v.a);
    if (mode == 0)
    {
        const float4 p = v.bgra;
        return p.a > 0 ? float4(decode3(p.rgb / p.a) * p.a, p.a) : 0;
    }
    float3 rgb = channels >= 3 ? v.rgb : v.rrr;
    float a = channels == 2 ? v.g : channels == 4 ? v.a : 1;
    if (is_signed)
        rgb = (max(rgb, -1) + 1) / 2;
    rgb = encoded ? decode3(saturate(rgb)) : rgb;
    a = saturate(a);
    return float4(rgb * a, a);
}
