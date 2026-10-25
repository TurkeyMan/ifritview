// An image's 8-bit pixels from its own samples (docs/gpu-pipeline.md):
// Image::retone_buffer() and render_source() step for step, as src/gpu.cpp's
// GLSL does, written once in HLSL (shader model 5) for Direct3D 11 (FXC) and
// Vulkan (SPIR-V). Unpremultiplied out: the premultiply is image.cpp's pack().
//
// Built once per kind of input, with one of
//   RAW (a raw's linear RGB, 16-bit unorm), and TONE when it has a curve;
//   LINEAR (a linear source: half or float);
//   SIGNED (signed 16-bit, snorm); otherwise unsigned 16-bit or float as encoded.

cbuffer Params : register(b0)
{
    int channels;     // of the source: 1 grey, 2 grey + alpha, 3 RGB, 4 RGBA
    float exposure;   // 2^ev
    float gain;       // raw without a tone curve: auto gain × exposure
    float unused;
};

Texture2D<float4> source : register(t0);
#if defined(TONE)
Texture2D<float> tone : register(t1);  // 4096 values, 64 × 64
#endif

float srgb_encode(float v)
{
    v = saturate(v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}
float byte_of(float v) { return floor(v * 255.0 + 0.5); }
// To the nearest half, as image.cpp looks floats up by their nearest half.
float as_half(float v) { return f16tof32(f32tof16(v)); }

float4 main(float4 position : SV_Position) : SV_Target
{
    const int3 at = int3(position.xy, 0);
#if defined(RAW)
    const float3 s = source.Load(at).rgb * 65535.0;  // the 16-bit values
#  if defined(TONE)
    const float3 c = s * (exposure / 65535.0);
    const float lum = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    const uint i = min(4095u, uint(lum * 4095.0 + 0.5));
    const float o = tone.Load(int3(i % 64u, i / 64u, 0));
    const float ratio = lum > 1e-6 ? o / lum : 0.0;
    const int3 k = clamp(int3(c * ratio * 4095.0 + 0.5), 0, 4095);
    const float3 rgb = float3(byte_of(srgb_encode(k.r / 4095.0)), byte_of(srgb_encode(k.g / 4095.0)),
                              byte_of(srgb_encode(k.b / 4095.0)));
#  else
    const float3 rgb = float3(byte_of(srgb_encode(s.r / 65535.0 * gain)), byte_of(srgb_encode(s.g / 65535.0 * gain)),
                              byte_of(srgb_encode(s.b / 65535.0 * gain)));
#  endif
    return float4(rgb, 255.0) / 255.0;
#else
    float4 v = source.Load(at);
#  if defined(SIGNED)
    v = max(-1.0, v);
#  endif
    const float4 px = channels >= 3 ? v : float4(v.r, v.r, v.r, v.g);
    const bool has_alpha = channels == 2 || channels == 4;
    const float a = channels == 2 ? v.g : v.a;
#  if defined(LINEAR)
    // Exposed, to the nearest half, then encoded; NaN shows as nothing.
    float3 rgb;
    [unroll] for (int c = 0; c < 3; ++c)
    {
        const float h = as_half(px[c] * exposure);
        rgb[c] = isnan(h) ? 0.0 : byte_of(srgb_encode(h));
    }
    const float ha = as_half(a);
    const float alpha = has_alpha ? (isnan(ha) ? 0.0 : byte_of(saturate(ha))) : 255.0;
#  else
    float3 c3 = px.rgb;
#    if defined(SIGNED)
    c3 = (c3 + 1.0) / 2.0;
#    endif
    const float3 rgb = float3(byte_of(saturate(c3.r)), byte_of(saturate(c3.g)), byte_of(saturate(c3.b)));
    const float alpha = has_alpha ? byte_of(saturate(a)) : 255.0;
#  endif
    return float4(rgb, alpha) / 255.0;
#endif
}
