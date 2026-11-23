// The 99.9th percentile of stats.hlsl's luminance histogram: the peak the
// display mapping works to (a few hot pixels don't count); the 99th; and
// from those and the log average, a raw's automatic exposure.
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

    // A raw's exposure (auto_gain()), anchored on the camera's: its metering
    // chose how the raw was exposed, so its middle grey is put where a
    // camera's JPEG puts it (`anchor`, a stop up: and a DNG's
    // BaselineExposure), and the scene moves that a stop at most. Down, to
    // fit its highlights: its 99th percentile at white (the roll-off takes
    // the 1% above). Up, as far as its highlights allow but no further than
    // puts its log average at middle grey, and only for a scene of normal
    // key the camera held down for its highlights: none for one exposed dark
    // (its log average under 1% at the camera's exposure: night, a room by
    // candlelight), a stop by 2%. A night scene's few lights would otherwise
    // lift its dark to grey and its noise with it.
    float gain = 1;
    if (anchor > 0 && total)
    {
        // Over every pixel of the image, black ones at the floor too: a
        // scene mostly black is dark, not just its few lit pixels.
        const uint opaque = max(stats.Load(kStatsOpaque * 4), total);
        float sum = float(opaque - total) * -14;
        for (int b = 0; b < 1024; ++b)
            sum += stats.Load((kStatsLuminance + b) * 4) * (max(float(b) + 0.5, 192.0) * 32 / 1024 - 20);  // log2, from 2^-14
        const float average = exp2(sum / opaque);
        const float bright = p99 > 0 ? 1 / p99 : 1, key = 0.18 / average;
        const float up = clamp(log2(average * anchor / 0.01), 0.0, 1.0);
        gain = anchor * exp2(clamp(log2(min(bright, max(key, anchor)) / anchor), -1.0, up));
    }
    stats.Store(kStatsAutoGain * 4, asuint(gain));
}
