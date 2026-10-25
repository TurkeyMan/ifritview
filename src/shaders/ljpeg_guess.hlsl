// Each chunk decoded from its start as if a code began there, once for each
// component that could be due: where each guess leaves the chunk (the first
// code boundary at or past its end), the component due there, and how many
// samples it decoded.
#define LJPEG_BITS
#include "ljpeg.hlsli"

RWByteAddressBuffer guesses : register(u2);  // per chunk and component: exit, component, count

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint g = id.x;
    if (g >= chunks * components)
        return;
    const uint k = g / components;
    uint pos = k * chunk_bits, c = g % components, n = 0;
    const uint end = min(total_bits, pos + chunk_bits);
    int diff;
    while (pos < end)
    {
        next_symbol(pos, c, diff);
        c = following(c);
        ++n;
    }
    guesses.Store3(g * 12, uint3(pos, c, n));
}
