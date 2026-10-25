// For each chunk k (from 1) and each way into it, the way out of chunk k - 1
// by its guess g: the true decode from there, alongside k's own guesses,
// until it stands where one of them does with the same component due. From
// there that guess is the true decode, so the chunk's samples are that
// guess's, less what it decoded before the meeting, plus what the true one
// did. ~0 when none meets inside the chunk (resolve flags it).
#define LJPEG_BITS
#include "ljpeg.hlsli"

RWByteAddressBuffer guesses : register(u2);
RWByteAddressBuffer ways : register(u3);  // per chunk and way in: the guess met, the samples it's out by

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x + components;  // chunk 0 has one way in, its start
    if (i >= chunks * components)
        return;
    const uint k = i / components, g = i % components;
    const uint3 from = guesses.Load3(((k - 1) * components + g) * 12);
    uint tp = from.x, tc = from.y, a = 0;
    uint sp[4], sc[4], b[4], exit_pos[4];
    for (uint h = 0; h < 4; ++h)
    {
        sp[h] = k * chunk_bits;
        sc[h] = h;
        b[h] = 0;
        exit_pos[h] = h < components ? guesses.Load((k * components + h) * 12) : 0;
    }
    const uint end = min(total_bits, (k + 1) * chunk_bits);
    int met = -1, diff;
    while (met < 0 && tp < end)
    {
        for (uint h = 0; h < components && met < 0; ++h)
        {
            while (sp[h] < tp && sp[h] < exit_pos[h])
            {
                next_symbol(sp[h], sc[h], diff);
                sc[h] = following(sc[h]);
                ++b[h];
            }
            if (sp[h] == tp && sc[h] == tc)
                met = int(h);
        }
        if (met >= 0)
            break;
        next_symbol(tp, tc, diff);
        tc = following(tc);
        ++a;
    }
    ways.Store2(i * 8, met >= 0 ? uint2(uint(met), a - b[met]) : uint2(0xFFFFFFFF, 0));
}
