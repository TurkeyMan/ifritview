// The 99.9th percentile of stats.hlsl's luminance histogram: the peak the
// display mapping works to (a few hot pixels don't count); the 99th, which
// auto exposure puts at white; and from those and the median, a raw's
// automatic scale (kStatsAutoGain).
#define COMPUTE
#include "view.hlsli"

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

    // The median, at its bin's middle.
    float p50 = 0;
    seen = 0;
    for (int b = 0; b < 1024 && total; ++b)
    {
        seen += stats.Load((kStatsLuminance + b) * 4);
        if (seen * 2 >= total)
        {
            p50 = exp2((float(b) + 0.5) * 32 / 1024 - 20);
            break;
        }
    }
    // A raw's scale: its 99th percentile at white, as LibRaw's
    // auto-brightening has it, but never brighter than the camera's own JPEG
    // (its median there; a night scene's few lights would otherwise lift the
    // dark to grey), and never more than 2 stops up (no JPEG, or a bad one).
    float gain = p99 > 0 ? 1 / p99 : 1;
    if (preview_mid > 0 && p50 > 0)
        gain = min(gain, preview_mid / p50);
    stats.Store(kStatsAutoGain * 4, asuint(min(gain, 4.0)));
}
