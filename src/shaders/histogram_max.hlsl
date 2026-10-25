// Each channel's tallest bin between the ends (bins 1..254), into words
// 1024..1027 (R, G, B, Y), for the panel's scale: the ends hold every
// clipped pixel and would flatten the rest.
RWByteAddressBuffer bins : register(u0);

[numthreads(256, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    if (index >= 1 && index <= 254)
        [unroll] for (uint ch = 0; ch < 4; ++ch)
            bins.InterlockedMax((1024 + ch) * 4, bins.Load((ch * 256 + index) * 4));
}
