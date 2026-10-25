// A masked area of the readout (`rect`, already inside it) summed per
// colour, as LibRaw's crop_masked_pixels does: the sums, how many, and how
// many were zero (levels: sum R G B G2, count R G B G2, zeros). A group's
// sums first, then one atomic add each.
#include "ljpeg.hlsli"

RWByteAddressBuffer values : register(u0);
RWByteAddressBuffer levels : register(u1);

groupshared uint group_sum[4], group_count[4], group_zero;

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index < 4)
    {
        group_sum[index] = 0;
        group_count[index] = 0;
    }
    if (index == 0)
        group_zero = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint y = uint(rect.x) + id.y, x = uint(rect.y) + id.x;
    if (y < uint(rect.z) && x < uint(rect.w))
    {
        const uint c = (filters >> ((((y << 1) & 14) | (x & 1)) << 1)) & 3;
        const uint i = readout_index(x, y);
        const uint v = SAMPLE(values, i);
        InterlockedAdd(group_sum[c], v);
        InterlockedAdd(group_count[c], 1);
        if (v == 0)
            InterlockedAdd(group_zero, 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < 4)
    {
        levels.InterlockedAdd(index * 4, group_sum[index]);
        levels.InterlockedAdd(16 + index * 4, group_count[index]);
    }
    if (index == 0)
        levels.InterlockedAdd(32, group_zero);
}
