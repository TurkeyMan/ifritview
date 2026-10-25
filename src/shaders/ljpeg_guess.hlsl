// Each chunk decoded from its start as if a code began there, once for each
// component that could be due: where each guess leaves the chunk (the first
// code boundary at or past its end, or its stream's), the component due
// there, and how many samples it decoded.
#define LJPEG_BITS
#include "ljpeg.hlsli"

RWByteAddressBuffer guesses : register(u2);  // per chunk and component: exit, component, count

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint g = id.x;
    if (g >= chunks * components)
        return;
    const uint k = g / components, p = part_of(k);
    const uint4 part = part_at(p);
    const uint4 t = tables_of(p);
    uint pos = part.x + (k - part.z) * chunk_bits, c = g % components, n = 0;
    const uint end = min(part.y, pos + chunk_bits);
    int diff;
    while (pos < end)
    {
        next_symbol(pos, table_for(t, c), diff);
        c = following(c);
        ++n;
    }
    guesses.Store3(g * 12, uint3(pos, c, n));
}
