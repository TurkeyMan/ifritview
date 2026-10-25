// A raw compressed in blocks decoded on the GPU (gpu_view.cpp's
// decode_blocks; rawblocks.cpp has the same on the CPU, and the formats):
// a thread a pair of blocks, into the readout as LibRaw lays it out, two
// samples a word (a pair's samples are whole words).

cbuffer Blocks : register(b0)
{
    uint format;      // 0 Sony's, 1 Panasonic's, 2 packed (Panasonic's encoding 5, C7), 3 Panasonic's C6
    uint bits;        // a sample's: 12 or 14
    uint per;         // samples a block
    uint units;       // pairs of blocks
    uint blocks;
    uint readout_w;
    uint row_blocks;  // Sony's: blocks a row
    uint span;        // threads a row of groups (the dispatch is two-dimensional when big)
    uint rotated;     // Panasonic's 16 KB buffer (encodings 4 and 5): each 16 KB of the file `rotation` bytes into it
    uint rotation;
    uint size;        // the file's bytes from where the readout starts
    uint unused;
};

RWByteAddressBuffer data : register(u0);    // the file from where the readout starts, then padding
RWByteAddressBuffer curve : register(u1);   // Sony's: 4096 entries, a word each
RWByteAddressBuffer values : register(u2);  // the readout

static const uint kBuffer = 0x4000;

uint file_byte(uint at)
{
    return (data.Load(at & ~3u) >> ((at & 3) * 8)) & 0xFF;
}

// 16 bytes from `at`, wherever it is.
uint4 load16(uint at)
{
    const uint a = at & ~3u, s = (at & 3) * 8;
    uint w[5];
    for (uint i = 0; i < 5; ++i)
        w[i] = data.Load(a + 4 * i);
    uint4 r;
    for (uint k = 0; k < 4; ++k)
        r[k] = s ? (w[k] >> s) | (w[k + 1] << (32 - s)) : w[k];
    return r;
}

// A byte of panasonic_load_raw's buffer, `at` bytes into the stream: each
// 16 KB of the file goes in `rotation` bytes on, the rest at its start; where
// the file ends first, the buffer keeps what it had (zero to begin with).
uint buffer_byte(uint at)
{
    uint c = at / kBuffer;
    const uint o = at % kBuffer;
    uint f = c * kBuffer + (o >= rotation ? o - rotation : o + kBuffer - rotation);
    while (f >= size)
    {
        if (c == 0)
            return 0;
        --c;
        f -= kBuffer;
    }
    return file_byte(f);
}

// Block b's 16 bytes.
uint4 load_block(uint b)
{
    if (format == 0)
        return load16((b / row_blocks) * readout_w + (b % row_blocks) * 16);
    const uint at = b * 16;
    if (rotated == 0)
        return load16(at);
    const uint c = at / kBuffer, o = at % kBuffer;
    if (o >= rotation || o + 16 <= rotation)
    {
        const uint f = c * kBuffer + (o >= rotation ? o - rotation : o + kBuffer - rotation);
        if (f + 16 <= size)
            return load16(f);
    }
    uint4 r = 0;
    for (uint i = 0; i < 16; ++i)
        r[i >> 2] |= buffer_byte(at + i) << ((i & 3) * 8);
    return r;
}

uint byte_of(uint4 b, uint i)
{
    return (b[i >> 2] >> ((i & 3) * 8)) & 0xFF;
}

uint le16(uint4 b, uint i)  // bytes i and i + 1 (i + 1 past the block reads 0)
{
    return byte_of(b, i) | (i < 15 ? byte_of(b, i + 1) << 8 : 0);
}

// The block's bits [q, q + n) as a 128-bit little-endian number, n <= 16.
uint bits_at(uint4 b, uint q, uint n)
{
    const uint w = q >> 5, s = q & 31;
    uint v = b[w] >> s;
    if (s + n > 32 && w < 3)
        v |= b[w + 1] << (32 - s);
    return v & ((1u << n) - 1);
}

void sony(uint4 b, inout uint s[32], uint k)
{
    const uint val = b.x;
    const int mx = int(val & 0x7ff), mn = int((val >> 11) & 0x7ff);
    const uint imax = (val >> 22) & 0x0f, imin = (val >> 26) & 0x0f;
    int sh = 0;
    while (sh < 4 && (int(0x80) << sh) <= mx - mn)
        ++sh;
    uint bit = 30;
    for (uint i = 0; i < 16; ++i)
    {
        int v;
        if (i == imax)
            v = mx;
        else if (i == imin)
            v = mn;
        else
        {
            v = min(int(((le16(b, bit >> 3) >> (bit & 7)) & 0x7f) << sh) + mn, 0x7ff);
            bit += 7;
        }
        s[2 * i + k] = curve.Load(uint(v) * 8) & 0xFFFF;  // curve[v << 1]
    }
}

void panasonic(uint4 b, inout uint s[32], uint at)
{
    uint q = 128;
    int pred[2] = {0, 0}, nonz[2] = {0, 0};
    int sh = 0;
    for (uint i = 0; i < 14; ++i)
    {
        if (i % 3 == 2)
        {
            q -= 2;
            sh = int(4u >> (3u - bits_at(b, q, 2)));
        }
        const uint p = i & 1;
        if (nonz[p] != 0)
        {
            q -= 8;
            const int j = int(bits_at(b, q, 8));
            if (j != 0)
            {
                pred[p] -= int(0x80) << sh;
                if (pred[p] < 0 || sh == 4)
                    pred[p] &= int(~(~0u << sh));
                pred[p] += j << sh;
            }
        }
        else
        {
            q -= 8;
            nonz[p] = int(bits_at(b, q, 8));
            if (nonz[p] != 0 || i > 11)
            {
                q -= 4;
                pred[p] = nonz[p] << 4 | int(bits_at(b, q, 4));
            }
        }
        s[at + i] = uint(pred[p]) & 0xFFFF;
    }
}

void packed(uint4 b, inout uint s[32], uint at)
{
    if (bits == 12)
    {
        for (uint k = 0; k < 5; ++k)
        {
            const uint p0 = byte_of(b, 3 * k), p1 = byte_of(b, 3 * k + 1), p2 = byte_of(b, 3 * k + 2);
            s[at + 2 * k] = ((p1 & 0xF) << 8) + p0;
            s[at + 2 * k + 1] = 16 * p2 + (p1 >> 4);
        }
    }
    else
    {
        for (uint k = 0; k < 2; ++k)
        {
            uint p[7];
            for (uint i = 0; i < 7; ++i)
                p[i] = byte_of(b, 7 * k + i);
            s[at + 4 * k] = p[0] + ((p[1] & 0x3F) << 8);
            s[at + 4 * k + 1] = (p[1] >> 6) + 4 * p[2] + ((p[3] & 0xF) << 10);
            s[at + 4 * k + 2] = (p[3] >> 4) + 16 * p[4] + ((p[5] & 3) << 12);
            s[at + 4 * k + 3] = ((p[5] & 0xFC) >> 2) + (p[6] << 6);
        }
        s[at + 8] = byte_of(b, 14) + ((byte_of(b, 15) & 0x3F) << 8);
    }
}

void panasonic6(uint4 b, inout uint s[32], uint at)
{
    uint w[16];
    for (uint i = 0; i < 16; ++i)
        w[i] = byte_of(b, 15 - i);
    uint v[18];
    uint n;
    if (bits == 12)
    {
        v[0] = (w[0] << 4) | (w[1] >> 4);
        v[1] = (((w[1] & 0xf) << 8) | w[2]) & 0xfff;
        v[2] = (w[3] >> 6) & 0x3;
        v[3] = ((w[3] & 0x3f) << 2) | (w[4] >> 6);
        v[4] = ((w[4] & 0x3f) << 2) | (w[5] >> 6);
        v[5] = ((w[5] & 0x3f) << 2) | (w[6] >> 6);
        v[6] = (w[6] >> 4) & 0x3;
        v[7] = ((w[6] & 0xf) << 4) | (w[7] >> 4);
        v[8] = ((w[7] & 0xf) << 4) | (w[8] >> 4);
        v[9] = ((w[8] & 0xf) << 4) | (w[9] >> 4);
        v[10] = (w[9] >> 2) & 0x3;
        v[11] = ((w[9] & 0x3) << 6) | (w[10] >> 2);
        v[12] = ((w[10] & 0x3) << 6) | (w[11] >> 2);
        v[13] = ((w[11] & 0x3) << 6) | (w[12] >> 2);
        v[14] = w[12] & 0x3;
        v[15] = w[13];
        v[16] = w[14];
        v[17] = w[15];
        n = 14;
    }
    else
    {
        v[0] = (w[0] << 6) | (w[1] >> 2);
        v[1] = (((w[1] & 0x3) << 12) | (w[2] << 4) | (w[3] >> 4)) & 0x3fff;
        v[2] = (w[3] >> 2) & 0x3;
        v[3] = ((w[3] & 0x3) << 8) | w[4];
        v[4] = (w[5] << 2) | (w[6] >> 6);
        v[5] = ((w[6] & 0x3f) << 4) | (w[7] >> 4);
        v[6] = (w[7] >> 2) & 0x3;
        v[7] = ((w[7] & 0x3) << 8) | w[8];
        v[8] = ((w[9] << 2) & 0x3fc) | (w[10] >> 6);
        v[9] = ((w[10] << 4) | (w[11] >> 4)) & 0x3ff;
        v[10] = (w[11] >> 2) & 0x3;
        v[11] = ((w[11] & 0x3) << 8) | w[12];
        v[12] = (((w[13] << 2) & 0x3fc) | w[14] >> 6) & 0x3ff;
        v[13] = ((w[14] << 4) | (w[15] >> 4)) & 0x3ff;
        v[14] = v[15] = v[16] = v[17] = 0;
        n = 11;
    }
    const uint base0 = bits == 12 ? 0x80 : 0x200, base_compare = bits == 12 ? 0x800 : 0x2000;
    const uint spix_compare = bits == 12 ? 0x3fff : 0xffff, mask = bits == 12 ? 0xfff : 0x3fff;
    uint oddeven[2] = {0, 0}, nonzero[2] = {0, 0};
    uint pmul = 0, pixel_base = 0, k = 0;
    for (uint pix = 0; pix < n; ++pix)
    {
        if (pix % 3 == 2)
        {
            uint base = v[k++];
            if (base == 3)
                base = 4;
            pixel_base = base0 << base;
            pmul = 1u << base;
        }
        uint e = v[k++];
        const uint p = pix % 2;
        if (oddeven[p] != 0)
        {
            e *= pmul;
            if (pixel_base < base_compare && nonzero[p] > pixel_base)
                e += nonzero[p] - pixel_base;
            nonzero[p] = e;
        }
        else
        {
            oddeven[p] = e;
            if (e != 0)
                nonzero[p] = e;
            else
                e = nonzero[p];
        }
        const uint spix = e - 0xf;
        s[at + pix] = spix <= spix_compare ? spix & spix_compare : uint(asint(e + 0x7ffffff1u) >> 31) & mask;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint u = id.y * span + id.x;
    if (u >= units)
        return;
    uint s[32];
    for (uint i = 0; i < 32; ++i)
        s[i] = 0;
    if (format == 0)
    {
        // Blocks 2j and 2j + 1 of a row: columns 32j on, even and odd.
        const uint row_units = (row_blocks + 1) / 2, row = u / row_units, j = u % row_units;
        const uint b = row * row_blocks + 2 * j;
        for (uint k = 0; k < 2; ++k)
            if (2 * j + k < row_blocks && int(32 * j + k) < int(readout_w) - 30)
                sony(load_block(b + k), s, k);
        const uint first = (row * readout_w + 32 * j) / 2;
        for (uint i = 0; i < 16 && 32 * j + 2 * i < readout_w; ++i)
            values.Store((first + i) * 4, s[2 * i] | s[2 * i + 1] << 16);
        return;
    }
    const uint b = 2 * u, n = b + 1 < blocks ? 2 : 1;
    for (uint k = 0; k < n; ++k)
    {
        const uint4 d = load_block(b + k);
        if (format == 1)
            panasonic(d, s, k * per);
        else if (format == 2)
            packed(d, s, k * per);
        else
            panasonic6(d, s, k * per);
    }
    // The pair's samples start at an even index (2u blocks of `per`): whole words.
    const uint first = u * per;
    for (uint i = 0; i < (n * per + 1) / 2; ++i)
        values.Store((first + i) * 4, s[2 * i] | s[2 * i + 1] << 16);
}
