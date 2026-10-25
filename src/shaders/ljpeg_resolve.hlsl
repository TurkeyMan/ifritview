// The ways (sync) chained from the start, in one group: each thread
// composes its run of chunks for every way into the run (which guess of the
// last chunk is true, and the samples, for each), thread 0 chains the runs,
// and each thread walks its run again writing every chunk's plan: its true
// way in (the true guess before it's exit), where its samples go, how many.
// Word 0 of the plan: 1 if some chunk's true decode met none of its guesses
// (the result is wrong; the CPU decodes it instead).
#include "ljpeg.hlsli"

RWByteAddressBuffer guesses : register(u0);
RWByteAddressBuffer ways : register(u1);
RWByteAddressBuffer plan : register(u2);  // 4 words, then per chunk: entry, component, first sample, samples

#define THREADS 256

groupshared uint run_state[THREADS * 4], run_samples[THREADS * 4];
groupshared uint start_state[THREADS], start_sample[THREADS];
groupshared uint failed;

// Into chunk k (from 1) with the guess `s` of the chunk before true: the
// guess of k that's true then, and k's samples.
uint step(uint k, uint s, out uint n, inout uint fail)
{
    const uint2 w = ways.Load2((k * components + s) * 8);
    uint h = w.x;
    if (h >= components)
    {
        fail = 1;
        h = 0;
    }
    n = guesses.Load((k * components + h) * 12 + 8) + w.y;
    return h;
}

[numthreads(THREADS, 1, 1)]
void main(uint3 id : SV_GroupThreadID)
{
    const uint t = id.x;
    if (t == 0)
        failed = 0;
    const uint per = (chunks + THREADS - 1) / THREADS;
    const uint first = t * per, last = min(chunks, first + per);
    uint fail = 0;
    for (uint s0 = 0; s0 < components; ++s0)
    {
        uint s = s0, total = 0, n;
        for (uint k = first; k < last; ++k)
        {
            if (k == 0)
            {
                s = 0;
                total += guesses.Load(8);
            }
            else
            {
                s = step(k, s, n, fail);
                total += n;
            }
        }
        run_state[t * 4 + s0] = s;
        run_samples[t * 4 + s0] = total;
    }
    if (fail)
        InterlockedOr(failed, 1);
    GroupMemoryBarrierWithGroupSync();
    if (t == 0)
    {
        uint s = 0, at = 0;
        for (uint r = 0; r < THREADS; ++r)
        {
            start_state[r] = s;
            start_sample[r] = at;
            at += run_samples[r * 4 + s];
            s = run_state[r * 4 + s];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint s = start_state[t], at = start_sample[t], n;
    for (uint k = first; k < last; ++k)
    {
        uint entry = 0, entry_c = 0;
        if (k == 0)
        {
            s = 0;
            n = guesses.Load(8);
        }
        else
        {
            const uint2 out_of = guesses.Load2(((k - 1) * components + s) * 12);
            entry = out_of.x;
            entry_c = out_of.y;
            s = step(k, s, n, fail);
        }
        plan.Store4(16 + k * 16, uint4(entry, entry_c, at, n));
        at += n;
    }
    if (t == 0)
        plan.Store(0, failed);
}
