// The 99.9th percentile of stats.hlsl's luminance histogram: the peak the
// display mapping works to (a few hot pixels don't count); the 99th; and
// from those and the log average, a raw's automatic exposure (exposure.hlsli).
#define COMPUTE
#include "view.hlsli"

RWByteAddressBuffer clips : register(u2);  // a raw's: clip.hlsl's (develop.hlsli's layout)
#include "clip_layout.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    const uint total = stats.Load(kStatsCount * 4);
    uint seen = 0;
    float peak = 0, p99 = 0;
    for (int bin = 1023; bin >= 0 && total; --bin)
    {
        seen += stats.Load((kStatsLuminance + bin) * 4);
        const float top = exp2(float(bin + 1) * 32 / 1024 - 20);
        if (peak == 0 && seen * 1000 >= total)
            peak = top;
        if (seen * 100 >= total)
        {
            p99 = top;
            break;
        }
    }
    stats.Store(kStatsP99 * 4, asuint(min(p99, asfloat(stats.Load(kStatsMaximum * 4)))));
    // A bin's top edge can be past the brightest pixel itself.
    stats.Store(kStatsPeak * 4, asuint(min(peak, asfloat(stats.Load(kStatsMaximum * 4)))));

    // A raw's exposure (auto_gain(); exposure.hlsli's model): anchored on
    // the camera's, a stop at most from it toward the key its scene's light
    // calls for, by the raw's white in cd/m^2 (`white_luminance`; 0 when its
    // ISO, shutter or aperture is unknown, and the key middle grey).
    float gain = 1, average = 0, adapting = 0, key = kMiddleGrey;
    if (anchor > 0 && total)
    {
        // Over every pixel of the image, black ones at the floor too: a
        // scene mostly black is dark, not just its few lit pixels.
        const uint opaque = max(stats.Load(kStatsOpaque * 4), total);
        float sum = float(opaque - total) * -14;
        for (int b = 0; b < 1024; ++b)
            sum += stats.Load((kStatsLuminance + b) * 4) * (max(float(b) + 0.5, 192.0) * 32 / 1024 - 20);  // log2, from 2^-14
        average = exp2(sum / opaque);
        // The develop scaled its values by the white it clips at; the
        // light's measured at the stated white.
        const float scale = asfloat(clips.Load(kClipWhite * 4));
        adapting = adapting_luminance(average, scale > 0 && scale <= 1 ? scale : 1.0f, white_luminance);
        key = display_key(adapting);
        gain = exposure_gain(anchor, average, p99, key);
    }
    stats.Store(kStatsAverage * 4, asuint(average));
    stats.Store(kStatsAdapting * 4, asuint(adapting));
    stats.Store(kStatsKey * 4, asuint(key));
    stats.Store(kStatsAutoGain * 4, asuint(gain));
}
