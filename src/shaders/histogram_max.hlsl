// The tallest histogram bin between the ends (bins 1..254 of every
// channel), into word 1024, for the panel's scale.
RWByteAddressBuffer bins : register(u0);

[numthreads(256, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    uint tallest = 0;
    if (index >= 1 && index <= 254)
        [unroll] for (uint ch = 0; ch < 4; ++ch)
            tallest = max(tallest, bins.Load((ch * 256 + index) * 4));
    bins.InterlockedMax(1024 * 4, tallest);
}
