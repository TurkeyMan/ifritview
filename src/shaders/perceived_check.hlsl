// --check-volume's compiled GPU regression for the display model's exact
// neutral and isotropic limits. No output clamp can hide a nonfinite model
// intermediate or a brightness search that loses its bracket.
#define PERCEIVED_CHECK
#define COMPUTE
#include "view.hlsli"
#include "detail_frame.hlsli"

Texture2D<float4> cases : register(t0);
RWByteAddressBuffer results : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const float4 sample = cases.Load(int3(id.x, 0, 0));
    const float3 rgb = sample.rgb;
    // The first three cases supply exact D65 neutrals (black, grey, white),
    // independently of the approximate RGB->XYZ matrix's round trip.
    const float3 xyz = id.x < 3 ? rgb.x * float3(.95047, 1, 1.08883) : xyz_of_rgb709(rgb);
    const float target = perceived_lightness(xyz);
    const float3 matched = match_perceived(rgb, target, 1);
    results.Store4(id.x * 48, asuint(float4(target, matched)));

    // With equal colour eigenvalues, every axis is an eigenvector. Its
    // arbitrary angle must not inject NaNs into an otherwise valid covariance.
    results.Store4(id.x * 48 + 16, asuint(float4(g_match_finite ? 1 : 0, g_match_bracket ? 1 : 0,
        volume_hue(float2(sample.a - 1, 0)), 0)));
    results.Store4(id.x * 48 + 32, asuint(float4(detail_colour_angle(.2 * sample.a, 0, .2),
        detail_colour_angle(.4, 0, .2), detail_colour_angle(.2, .1, .2), 0)));
}
