// The ways (sync) followed from each stream's start, in order, a thread a
// stream: each chunk's true way in (the exit of the guess before it that's
// true), where its samples go and how many. A way that met a guess of a
// later chunk takes the chunks it ran over with it: the samples are all
// decoded from its way in, and those chunks have none of their own. A chunk
// is a couple of loads, and one store whatever it comes to.
// Word 0 of the plan (cleared before): 1 if a stream's samples ran out
// before it did, or a way was never worked out (a corrupt stream: the result
// is wrong).
#include "ljpeg.hlsli"

RWByteAddressBuffer guesses : register(u0);
RWByteAddressBuffer ways : register(u1);
RWByteAddressBuffer plan : register(u2);  // 4 words, then per chunk: entry, component, first sample, samples
RWByteAddressBuffer meta : register(u3);  // ljpeg.hlsli's: the streams, then the tables

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint p = id.x;
    if (p >= parts)
        return;
    const uint4 part = meta.Load4(p * PART_WORDS * 4);
    const uint first = part.z, last = part.z + part.w, base = p * samples;
    const uint held = meta.Load((p * PART_WORDS + 8) * 4);  // its frame's, fewer in a strip's last restart interval
    // The stream's first chunk starts at its start, with component 0: its guess 0 is true.
    uint at = min(guesses.Load(first * components * 12 + 8), held), s = 0, next = first + 1;
    plan.Store4(16 + first * 16, uint4(part.x, 0, base, at));
    bool failed = false;
    for (uint k = first + 1; k < last; ++k)
    {
        uint4 e = uint4(0, 0, base + at, 0);  // none of its own: run over, or past the last sample
        if (k == next && at < held)
        {
            const uint2 out_of = guesses.Load2(((k - 1) * components + s) * 12);
            const uint4 w = ways.Load4((k * components + s) * 16);
            uint n = 0;
            if (w.x == 0xFFFFFFFEu)
            {
                // Met none to the stream's end: the rest is this chunk's.
                n = w.z;
                next = last;
            }
            else if (w.x < components)
            {
                n = w.z + guesses.Load((w.y * components + w.x) * 12 + 8) - w.w;
                s = w.x;
                next = w.y + 1;
            }
            else
            {
                failed = true;
                next = last;
            }
            n = min(n, held - at);
            e = uint4(out_of.x, out_of.y, base + at, n);
            at += n;
        }
        plan.Store4(16 + k * 16, e);
    }
    if (failed || at < held)
        plan.Store(0, 1);
}
