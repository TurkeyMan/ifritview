// For each chunk k (from 1) and each way into it, the way out of chunk k - 1
// by its guess g: the true decode from there, alongside k's own guesses,
// until it stands where one of them does with the same component due. From
// there that guess is the true decode. Mostly that's within a few dozen
// samples; but where the image is flat (clipped highlights: the same short
// code over and over) a decode a bit or two off reads valid codes too and
// can run beside the true one for thousands of samples without meeting it.
// So past the chunk's end it goes on into the next chunks, against their
// guesses, until it meets one: the guess met, its chunk, and how many
// samples the true decode and that guess had each decoded by then. ~0 when
// it never meets one (a corrupt stream; resolve flags it).
#define LJPEG_BITS
#include "ljpeg.hlsli"

RWByteAddressBuffer guesses : register(u2);
RWByteAddressBuffer ways : register(u3);  // per chunk and way in: guess met, its chunk, true samples, the guess's

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x + components;  // chunk 0 has one way in, its start
    if (i >= chunks * components)
        return;
    const uint k = i / components, g = i % components;
    const uint3 from = guesses.Load3(((k - 1) * components + g) * 12);
    uint tp = from.x, tc = from.y, a = 0;
    int met = -1, diff;
    uint j = k, sp[4], sc[4], b[4], exit_pos[4];
    for (; j < chunks && met < 0; ++j)
    {
        for (uint h = 0; h < 4; ++h)
        {
            sp[h] = j * chunk_bits;
            sc[h] = h;
            b[h] = 0;
            exit_pos[h] = h < components ? guesses.Load((j * components + h) * 12) : 0;
        }
        const uint end = min(total_bits, (j + 1) * chunk_bits);
        while (tp < end)
        {
            for (uint h2 = 0; h2 < components && met < 0; ++h2)
            {
                while (sp[h2] < tp && sp[h2] < exit_pos[h2])
                {
                    next_symbol(sp[h2], sc[h2], diff);
                    sc[h2] = following(sc[h2]);
                    ++b[h2];
                }
                if (sp[h2] == tp && sc[h2] == tc)
                    met = int(h2);
            }
            if (met >= 0)
                break;
            next_symbol(tp, tc, diff);
            tc = following(tc);
            ++a;
        }
        if (met >= 0)
            break;
    }
    ways.Store4(i * 16, met >= 0 ? uint4(uint(met), j, a, b[met]) : uint4(0xFFFFFFFF, 0, 0, 0));
}
