// Each row's first sample of each component: that of the row `step` above
// (1; Nikon's 2: the same colours) plus its difference, the first rows from
// where the stream says they start. A group a stream, a run of rows a thread
// for each of the `step` chains: each sums its run, thread 0 chains the
// runs, each writes its own.
#include "ljpeg.hlsli"

RWByteAddressBuffer values : register(u0);

#define THREADS 256

groupshared uint run_sum[THREADS * 4], run_start[THREADS * 4];

uint first_of(uint base, uint r, uint c)  // base: the stream's first sample
{
    const uint i = base + r * row + c;
    return SAMPLE(values, i);
}

[numthreads(THREADS, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 id : SV_GroupThreadID)
{
    const uint base = group.x * samples;
    const uint t = id.x, step = max(1u, (scheme >> 8) & 0xFF);
    for (uint chain = 0; chain < step; ++chain)
    {
        // This chain's rows: chain, chain + step, ...
        const uint count = rows > chain ? (rows - chain + step - 1) / step : 0;
        const uint per = (count + THREADS - 1) / THREADS;
        const uint first = t * per, last = min(count, first + per);
        for (uint c = 0; c < components; ++c)
        {
            uint sum = 0;
            for (uint k = first; k < last; ++k)
                sum += first_of(base, chain + k * step, c);
            run_sum[t * 4 + c] = sum;
        }
        GroupMemoryBarrierWithGroupSync();
        if (t == 0)
            for (uint c2 = 0; c2 < components; ++c2)
            {
                uint at = start_of(chain, c2);
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
        for (uint k2 = first; k2 < last; ++k2)
        {
            const uint r = chain + k2 * step;
            // The row's first `components` samples, whole words (rows start even).
            for (uint c4 = 0; c4 < components; c4 += 2)
            {
                const uint at = (base + r * row + c4) >> 1;
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
        GroupMemoryBarrierWithGroupSync();  // the shared sums again for the next chain
    }
}
