// Each row's first sample of each component: the one above's plus its
// difference, from half the range at the top. In one group, a run of rows a
// thread: each sums its run, thread 0 chains the runs, each writes its own.
#include "ljpeg.hlsli"

RWByteAddressBuffer values : register(u0);

#define THREADS 256

groupshared uint run_sum[THREADS * 4], run_start[THREADS * 4];

uint first_of(uint r, uint c)
{
    const uint i = r * row + c;
    return SAMPLE(values, i);
}

[numthreads(THREADS, 1, 1)]
void main(uint3 id : SV_GroupThreadID)
{
    const uint t = id.x;
    const uint per = (rows + THREADS - 1) / THREADS;
    const uint first = t * per, last = min(rows, first + per);
    for (uint c = 0; c < components; ++c)
    {
        uint sum = 0;
        for (uint r = first; r < last; ++r)
            sum += first_of(r, c);
        run_sum[t * 4 + c] = sum;
    }
    GroupMemoryBarrierWithGroupSync();
    if (t == 0)
        for (uint c2 = 0; c2 < components; ++c2)
        {
            uint at = 1u << (precision - 1);
            for (uint r = 0; r < THREADS; ++r)
            {
                run_start[r * 4 + c2] = at;
                at += run_sum[r * 4 + c2];
            }
        }
    GroupMemoryBarrierWithGroupSync();
    uint v[4];
    for (uint c3 = 0; c3 < 4; ++c3)
        v[c3] = c3 < components ? run_start[t * 4 + c3] : 0;
    for (uint r = first; r < last; ++r)
    {
        // The row's first `components` samples, whole words (rows start even).
        for (uint c4 = 0; c4 < components; c4 += 2)
        {
            const uint at = (r * row + c4) >> 1;
            const uint w = values.Load(at * 4);
            v[c4] += w & 0xFFFF;
            uint hi = w >> 16;
            if (c4 + 1 < components)
            {
                v[c4 + 1] += hi;
                hi = v[c4 + 1] & 0xFFFF;
            }
            values.Store(at * 4, (v[c4] & 0xFFFF) | hi << 16);
        }
    }
}
