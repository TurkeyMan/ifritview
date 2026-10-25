// The panel's scale, per channel: words 1024..1027 the tallest bin between
// the ends (1..254), 1028..1031 the count over those bins (for their mean).
// A few spike bins (pure black under saturated colours, a flat sky) would
// otherwise flatten everything else: histogram.hlsli scales to a multiple
// of the mean and lets spikes run off the top.
RWByteAddressBuffer bins : register(u0);

[numthreads(256, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    if (index >= 1 && index <= 254)
        [unroll] for (uint ch = 0; ch < 4; ++ch)
        {
            const uint n = bins.Load((ch * 256 + index) * 4);
            bins.InterlockedMax((1024 + ch) * 4, n);
            bins.InterlockedAdd((1028 + ch) * 4, n);
        }
}
