// A raw's readout stored uncompressed (RawMosaic::Packed; gpu_view.cpp's
// unpack_stream) unpacked into samples as LibRaw's loaders read it, 16 bits
// each, two to a word as the ljpeg passes leave theirs (mosaic and
// ljpeg_black take them from there). A word a thread.

cbuffer Unpack : register(b0)
{
    uint readout_w;  // the readout, as LibRaw lays it out
    uint readout_h;
    uint row_bits;   // from one row's start to the next
    uint bits;       // a sample's, 1 to 16
    uint group;      // bytes taken as one little-endian word (1: as they come)
    uint shift;      // taken off each sample, before the curve
    uint swap;       // 1: each pair of columns the other way round
    uint curved;     // 1: each sample through `curve`
    uint lead;       // where the first row starts in `stored`
    uint words;      // values': (readout_w * readout_h + 1) / 2
    uint2 unused;
};

RWByteAddressBuffer stored : register(u0);  // the rows' bytes as the file has them, from the word they start in
RWByteAddressBuffer curve : register(u1);   // 65536 entries, 16 bits each
RWByteAddressBuffer values : register(u2);  // the samples

// The rows' q-th byte as the loaders take it: within each group, from its last.
uint byte_at(uint q)
{
    const uint k = q % group;
    const uint a = lead + q - k + (group - 1 - k);
    return (stored.Load(a & ~3u) >> ((a & 3u) * 8)) & 0xFF;
}

uint sample_at(uint i)
{
    const uint y = i / readout_w, x = i - y * readout_w;
    const uint b = y * row_bits + (x ^ swap) * bits;  // its first bit, most significant first
    const uint q = b >> 3;
    const uint three = byte_at(q) << 16 | byte_at(q + 1) << 8 | byte_at(q + 2);
    uint v = (three >> (24 - (b & 7) - bits)) & ((1u << bits) - 1);
    v >>= shift;
    if (curved != 0)
        v = (curve.Load((v >> 1) * 4) >> ((v & 1) * 16)) & 0xFFFF;
    return v;
}

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint k = id.y * 65536 + id.x;  // 256 groups across
    if (k >= words)
        return;
    const uint n = readout_w * readout_h;
    const uint low = sample_at(2 * k), high = 2 * k + 1 < n ? sample_at(2 * k + 1) : 0;
    values.Store(k * 4, low | high << 16);
}
