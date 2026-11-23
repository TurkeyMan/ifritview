// A masked area of the readout (`rect`, already inside it) summed per
// colour, as LibRaw's crop_masked_pixels does: the sums, how many, and how
// many were zero (levels: sum R G B G2, count R G B G2, zeros); and for the
// readout's noise, each photosite's squared difference from the one of its
// colour two rows down (inside the area; at most 2047 apart, so a group's
// fit in a word), summed into 64 bits, and how many (clip_layout.hlsli's
// kLevelsSquares, kLevelsDifferences). A group's sums first, then one
// atomic add each.
#include "ljpeg.hlsli"
#include "clip_layout.hlsli"

RWByteAddressBuffer values : register(u0);
RWByteAddressBuffer levels : register(u1);

groupshared uint group_sum[4], group_count[4], group_zero, group_square[4], group_pairs[4];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    if (index < 4)
    {
        group_sum[index] = 0;
        group_count[index] = 0;
        group_square[index] = 0;
        group_pairs[index] = 0;
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
        InterlockedAdd(group_count[c], 1u);
        if (v == 0)
            InterlockedAdd(group_zero, 1u);
        if (y + 2 < uint(rect.z))
        {
            const int d = clamp(int(v) - int(SAMPLE(values, readout_index(x, y + 2))), -2047, 2047);
            InterlockedAdd(group_square[c], uint(d * d));
            InterlockedAdd(group_pairs[c], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < 4)
    {
        levels.InterlockedAdd(index * 4, group_sum[index]);
        levels.InterlockedAdd(16 + index * 4, group_count[index]);
        uint before;
        levels.InterlockedAdd((kLevelsSquares + 2 * index) * 4, group_square[index], before);
        if (before + group_square[index] < before)  // carried
            levels.InterlockedAdd((kLevelsSquares + 2 * index + 1) * 4, 1u);
        levels.InterlockedAdd((kLevelsDifferences + index) * 4, group_pairs[index]);
    }
    if (index == 0)
        levels.InterlockedAdd(32, group_zero);
}
