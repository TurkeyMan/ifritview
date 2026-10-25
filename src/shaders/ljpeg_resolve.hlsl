// The ways (sync) followed from the start, in order: each chunk's true way
// in (the exit of the guess before it that's true), where its samples go and
// how many. A way that met a guess of a later chunk takes the chunks it ran
// over with it: the samples are all decoded from its way in, and those
// chunks have none of their own. One thread: a chunk is a couple of loads.
// Word 0 of the plan: 1 if the true decode met none of the guesses (a
// corrupt stream: the result is wrong).
#include "ljpeg.hlsli"

RWByteAddressBuffer guesses : register(u0);
RWByteAddressBuffer ways : register(u1);
RWByteAddressBuffer plan : register(u2);  // 4 words, then per chunk: entry, component, first sample, samples

[numthreads(1, 1, 1)]
void main()
{
    uint failed = 0;
    // Chunk 0 starts at the start, with component 0: its guess 0 is true.
    uint at = guesses.Load(8), s = 0, k = 1;
    plan.Store4(16, uint4(0, 0, 0, at));
    while (k < chunks)
    {
        const uint2 out_of = guesses.Load2(((k - 1) * components + s) * 12);
        if (at >= samples)
        {
            plan.Store4(16 + k * 16, uint4(out_of.x, out_of.y, at, 0));  // past the last sample: padding
            ++k;
            continue;
        }
        const uint4 w = ways.Load4((k * components + s) * 16);
        if (w.x >= components)
        {
            failed = 1;
            for (; k < chunks; ++k)
                plan.Store4(16 + k * 16, uint4(0, 0, at, 0));
            break;
        }
        const uint n = w.z + guesses.Load((w.y * components + w.x) * 12 + 8) - w.w;
        plan.Store4(16 + k * 16, uint4(out_of.x, out_of.y, at, n));
        at += n;
        for (uint m = k + 1; m <= w.y; ++m)
            plan.Store4(16 + m * 16, uint4(0, 0, at, 0));  // run over: its samples are k's
        s = w.x;
        k = w.y + 1;
    }
    plan.Store(0, failed);
}
