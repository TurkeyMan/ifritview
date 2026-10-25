// Each chunk decoded from its true way in, its differences written where its
// samples go, two to a word: a word it shares with the chunk before or after
// (an odd first or last sample) by an atomic OR into the cleared buffer.
#define LJPEG_BITS
#include "ljpeg.hlsli"

RWByteAddressBuffer plan : register(u2);
RWByteAddressBuffer out_samples : register(u3);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint k = id.x;
    if (k >= chunks)
        return;
    const uint4 p = plan.Load4(16 + k * 16);
    if (p.z >= samples)
        return;
    const uint n = min(p.w, samples - p.z);
    uint pos = p.x, c = p.y, low = 0;
    bool holding = false;
    int diff;
    for (uint j = 0; j < n; ++j)
    {
        next_symbol(pos, c, diff);
        c = following(c);
        const uint i = p.z + j, v = uint(diff) & 0xFFFF;
        if ((i & 1) == 0)
        {
            low = v;
            holding = true;
        }
        else if (holding)
        {
            out_samples.Store((i >> 1) * 4, low | v << 16);
            holding = false;
        }
        else
            out_samples.InterlockedOr((i >> 1) * 4, v << 16);
    }
    if (holding)
        out_samples.InterlockedOr(((p.z + n - 1) >> 1) * 4, low);
}
