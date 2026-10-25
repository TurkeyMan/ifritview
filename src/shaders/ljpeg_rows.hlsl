// A row a group: each sample its left neighbour's (of its component) plus
// its difference, the row's first samples already summed (columns). Each
// thread sums its run of the row's words per component, thread 0 chains the
// runs, each writes its run's running sums.
#include "ljpeg.hlsli"

RWByteAddressBuffer values : register(u0);

#define THREADS 256

groupshared uint run_sum[THREADS * 4], run_start[THREADS * 4];

[numthreads(THREADS, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 id : SV_GroupThreadID)
{
    const uint r = group.x, t = id.x;
    if (r >= rows)
        return;
    const uint words = row >> 1, base = r * words;
    const uint per = (words + THREADS - 1) / THREADS;
    const uint first = min(words, t * per), last = min(words, first + per);
    // Word w holds samples 2w and 2w + 1: components (2w) % C and (2w + 1) % C.
    uint sum[4] = {0, 0, 0, 0};
    for (uint w = first; w < last; ++w)
    {
        const uint v = values.Load((base + w) * 4);
        sum[(2 * w) % components] += v & 0xFFFF;
        sum[(2 * w + 1) % components] += v >> 16;
    }
    for (uint c = 0; c < 4; ++c)
        run_sum[t * 4 + c] = sum[c];
    GroupMemoryBarrierWithGroupSync();
    if (t == 0)
        for (uint c2 = 0; c2 < components; ++c2)
        {
            uint at = 0;
            for (uint k = 0; k < THREADS; ++k)
            {
                run_start[k * 4 + c2] = at;
                at += run_sum[k * 4 + c2];
            }
        }
    GroupMemoryBarrierWithGroupSync();
    uint v4[4];
    for (uint c3 = 0; c3 < 4; ++c3)
        v4[c3] = run_start[t * 4 + c3];
    for (uint w2 = first; w2 < last; ++w2)
    {
        const uint v = values.Load((base + w2) * 4);
        const uint a = (2 * w2) % components, b = (2 * w2 + 1) % components;
        v4[a] += v & 0xFFFF;
        const uint lo = v4[a] & 0xFFFF;
        v4[b] += v >> 16;
        values.Store((base + w2) * 4, lo | (v4[b] & 0xFFFF) << 16);
    }
}
