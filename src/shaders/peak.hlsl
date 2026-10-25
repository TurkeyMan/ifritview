// The 99.9th percentile of stats.hlsl's luminance histogram: the peak the
// display mapping works to (a few hot pixels don't count).
#define COMPUTE
#include "view.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    const uint total = stats.Load(kStatsCount * 4);
    uint seen = 0;
    float peak = 0;
    for (int bin = 1023; bin >= 0 && total; --bin)
    {
        seen += stats.Load((kStatsLuminance + bin) * 4);
        if (seen * 1000 >= total)
        {
            peak = exp2(float(bin + 1) * 32 / 1024 - 20);
            break;
        }
    }
    // A bin's top edge can be past the brightest pixel itself.
    stats.Store(kStatsPeak * 4, asuint(min(peak, asfloat(stats.Load(kStatsMaximum * 4)))));
}
