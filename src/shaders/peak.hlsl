// The 99.9th percentile of stats.hlsl's luminance histogram: the peak the
// display mapping works to (a few hot pixels don't count); and the 99th,
// which auto exposure puts at white.
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
}
