#include "rawblocks.h"

#include "../gpu.h"
#include "../parallel.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace rawblocks
{
namespace
{

uint32_t le16(const uint8_t* p)
{
    return p[0] | uint32_t(p[1]) << 8;
}

uint32_t le32(const uint8_t* p)
{
    return le16(p) | le16(p + 2) << 16;
}

// Sony's: the block's greatest and least, where they are, and the other 14
// as 7-bit steps up from the least (scaled by how far apart the two are),
// each through the curve.
void sony(const uint8_t* b, const uint16_t* curve, uint16_t out[16])
{
    const uint32_t val = le32(b);
    const int max = int(0x7ff & val), min = int(0x7ff & val >> 11), imax = int(0x0f & val >> 22), imin = int(0x0f & val >> 26);
    int sh = 0;
    while (sh < 4 && 0x80 << sh <= max - min)
        ++sh;
    for (int i = 0, bit = 30; i < 16; ++i)
    {
        int v;
        if (i == imax)
            v = max;
        else if (i == imin)
            v = min;
        else
        {
            const int at = bit >> 3;  // its 7 bits never past the block, but LibRaw reads 16: not past the file
            v = std::min(int(((at < 15 ? le16(b + at) : b[15]) >> (bit & 7) & 0x7f) << sh) + min, 0x7ff);
            bit += 7;
        }
        out[i] = curve[v << 1];
    }
}

// Panasonic's: the block a 128-bit little-endian number read from its top
// bit down; each column parity predicted from its last sample, a step's
// scale every three samples.
void panasonic(const uint8_t* b, uint16_t out[14])
{
    uint64_t lo, hi;
    std::memcpy(&lo, b, 8);  // little-endian hosts only (all of ours)
    std::memcpy(&hi, b + 8, 8);
    unsigned q = 128;
    const auto read = [&](unsigned n) -> unsigned
    {
        q -= n;
        const uint64_t v = q >= 64 ? hi >> (q - 64) : q + n > 64 ? lo >> q | hi << (64 - q) : lo >> q;
        return unsigned(v) & ((1u << n) - 1);
    };
    int pred[2] = {0, 0}, nonz[2] = {0, 0}, sh = 0;
    for (int i = 0; i < 14; ++i)
    {
        if (i % 3 == 2)
            sh = 4 >> (3 - read(2));
        if (nonz[i & 1])
        {
            if (const int j = int(read(8)))
            {
                if ((pred[i & 1] -= 0x80 << sh) < 0 || sh == 4)
                    pred[i & 1] &= ~(~0u << sh);
                pred[i & 1] += j << sh;
            }
        }
        else if ((nonz[i & 1] = int(read(8))) || i > 11)
            pred[i & 1] = nonz[i & 1] << 4 | int(read(4));
        out[i] = uint16_t(pred[i & 1]);
    }
}

// Encoding 5's and C7's: the samples packed, 12 or 14 bits each.
void packed(const uint8_t* b, int bits, uint16_t* o)
{
    if (bits == 12)
        for (int k = 0; k < 5; ++k)
        {
            const uint8_t* p = b + 3 * k;
            o[2 * k] = uint16_t(((p[1] & 0xF) << 8) + p[0]);
            o[2 * k + 1] = uint16_t(16 * p[2] + (p[1] >> 4));
        }
    else
        for (int k = 0; k < 2; ++k)
        {
            const uint8_t* p = b + 7 * k;
            o[4 * k] = uint16_t(p[0] + ((p[1] & 0x3F) << 8));
            o[4 * k + 1] = uint16_t((p[1] >> 6) + 4 * p[2] + ((p[3] & 0xF) << 10));
            o[4 * k + 2] = uint16_t((p[3] >> 4) + 16 * p[4] + ((p[5] & 3) << 12));
            o[4 * k + 3] = uint16_t(((p[5] & 0xFC) >> 2) + (p[6] << 6));
            if (k == 1)
                o[8] = uint16_t(b[14] + ((b[15] & 0x3F) << 8));
        }
}

// C6's: the block's fields (big-endian, from its last byte), a base every
// three samples, each column parity's samples steps from its last.
void panasonic6(const uint8_t* b, int bits, uint16_t* o)
{
    const auto w = [&](int i) -> unsigned { return b[15 - i]; };
    unsigned v[18];
    int n;
    if (bits == 12)
    {
        v[0] = (w(0) << 4) | (w(1) >> 4);
        v[1] = (((w(1) & 0xf) << 8) | w(2)) & 0xfff;
        v[2] = (w(3) >> 6) & 0x3;
        v[3] = ((w(3) & 0x3f) << 2) | (w(4) >> 6);
        v[4] = ((w(4) & 0x3f) << 2) | (w(5) >> 6);
        v[5] = ((w(5) & 0x3f) << 2) | (w(6) >> 6);
        v[6] = (w(6) >> 4) & 0x3;
        v[7] = ((w(6) & 0xf) << 4) | (w(7) >> 4);
        v[8] = ((w(7) & 0xf) << 4) | (w(8) >> 4);
        v[9] = ((w(8) & 0xf) << 4) | (w(9) >> 4);
        v[10] = (w(9) >> 2) & 0x3;
        v[11] = ((w(9) & 0x3) << 6) | (w(10) >> 2);
        v[12] = ((w(10) & 0x3) << 6) | (w(11) >> 2);
        v[13] = ((w(11) & 0x3) << 6) | (w(12) >> 2);
        v[14] = w(12) & 0x3;
        v[15] = w(13);
        v[16] = w(14);
        v[17] = w(15);
        n = 14;
    }
    else
    {
        v[0] = (w(0) << 6) | (w(1) >> 2);
        v[1] = (((w(1) & 0x3) << 12) | (w(2) << 4) | (w(3) >> 4)) & 0x3fff;
        v[2] = (w(3) >> 2) & 0x3;
        v[3] = ((w(3) & 0x3) << 8) | w(4);
        v[4] = (w(5) << 2) | (w(6) >> 6);
        v[5] = ((w(6) & 0x3f) << 4) | (w(7) >> 4);
        v[6] = (w(7) >> 2) & 0x3;
        v[7] = ((w(7) & 0x3) << 8) | w(8);
        v[8] = ((w(9) << 2) & 0x3fc) | (w(10) >> 6);
        v[9] = ((w(10) << 4) | (w(11) >> 4)) & 0x3ff;
        v[10] = (w(11) >> 2) & 0x3;
        v[11] = ((w(11) & 0x3) << 8) | w(12);
        v[12] = (((w(13) << 2) & 0x3fc) | w(14) >> 6) & 0x3ff;
        v[13] = ((w(14) << 4) | (w(15) >> 4)) & 0x3ff;
        n = 11;
    }
    const unsigned base0 = bits == 12 ? 0x80 : 0x200, base_compare = bits == 12 ? 0x800 : 0x2000;
    const unsigned spix_compare = bits == 12 ? 0x3fff : 0xffff, mask = bits == 12 ? 0xfff : 0x3fff;
    unsigned oddeven[2] = {0, 0}, nonzero[2] = {0, 0}, pmul = 0, pixel_base = 0;
    for (int pix = 0, k = 0; pix < n; ++pix)
    {
        if (pix % 3 == 2)
        {
            unsigned base = v[k++];
            if (base == 3)
                base = 4;
            pixel_base = base0 << base;
            pmul = 1u << base;
        }
        unsigned e = v[k++];
        if (oddeven[pix % 2])
        {
            e *= pmul;
            if (pixel_base < base_compare && nonzero[pix % 2] > pixel_base)
                e += nonzero[pix % 2] - pixel_base;
            nonzero[pix % 2] = e;
        }
        else
        {
            oddeven[pix % 2] = e;
            if (e)
                nonzero[pix % 2] = e;
            else
                e = nonzero[pix % 2];
        }
        const unsigned spix = e - 0xf;
        o[pix] = uint16_t(spix <= spix_compare ? spix & spix_compare : unsigned(int32_t(e + 0x7ffffff1u) >> 31) & mask);
    }
}

// Where in the file panasonic_load_raw's buffer has the stream's byte `at`
// from: each 16 KB of the file goes in `rotation` bytes on, the rest at its start.
uint64_t file_offset(const Prepared& p, uint64_t at)
{
    const uint32_t o = uint32_t(at % kBuffer), lf = p.rotation;
    return at - o + (o >= lf ? o - lf : o + kBuffer - lf);
}

// A byte of that buffer; where the file ends first, the buffer keeps what
// it had (zero to begin with).
uint8_t buffer_byte(const Prepared& p, uint64_t at)
{
    uint64_t c = at / kBuffer;
    uint64_t f = file_offset(p, at);
    for (; f >= p.size; --c, f -= kBuffer)
        if (c == 0)
            return 0;
    return p.data[f];
}

// Block b's 16 bytes, in place in the file (or gathered into `tmp`, where
// Panasonic's buffer has them from two places).
const uint8_t* block(const Prepared& p, uint32_t b, uint8_t tmp[16])
{
    if (p.format == Format::sony)
        return p.data + uint64_t(b / p.row_blocks) * p.readout_w + uint64_t(b % p.row_blocks) * 16;
    const uint64_t at = uint64_t(b) * 16;
    if (!p.rotated)
        return p.data + at;
    const uint32_t o = uint32_t(at % kBuffer);
    if (o >= p.rotation || o + 16 <= p.rotation)  // its 16 bytes together in the file
    {
        const uint64_t f = file_offset(p, at);
        if (f + 16 <= p.size)
            return p.data + f;
    }
    for (int i = 0; i < 16; ++i)
        tmp[i] = buffer_byte(p, at + i);
    return tmp;
}

}  // namespace

uint64_t Prepared::extent() const
{
    if (format == Format::sony)
        return uint64_t(rows) * readout_w;
    if (!rotated)
        return uint64_t(blocks) * 16;
    return std::min<uint64_t>(size, (uint64_t(blocks) * 16 + kBuffer - 1) / kBuffer * kBuffer);
}

bool prepare(const uint8_t* file, size_t size, uint64_t offset, const uint16_t* curve, Prepared& p, std::string& error)
{
    if (p.readout_w <= 0 || p.readout_h <= 0 || p.rows <= 0 || p.rows > p.readout_h || p.readout_w % 2 || offset >= size)
        return error = "an unexpected layout", false;
    p.data = file + offset;
    p.size = size - offset;
    if (p.format == Format::sony)
    {
        if (!curve || p.readout_w < 32)
            return error = "an unexpected layout", false;
        p.per = 16;
        p.row_blocks = p.readout_w / 16;
        p.blocks = uint32_t(p.row_blocks) * uint32_t(p.rows);
        p.curve.assign(curve, curve + 4096);
    }
    else
    {
        if (p.bits != 12 && p.bits != 14 && p.format != Format::panasonic)
            return error = "a sample size it doesn't use", false;
        p.per = p.format == Format::panasonic ? 14 : p.format == Format::packed ? (p.bits == 12 ? 10 : 9) : (p.bits == 12 ? 14 : 11);
        if (p.readout_w % p.per)
            return error = "rows not whole blocks", false;
        if (p.rotated && p.rotation >= kBuffer)
            return error = "an unexpected layout", false;
        p.blocks = uint32_t(uint64_t(p.readout_w / p.per) * p.rows);
    }
    if (!p.rotated && p.extent() > p.size)
        return error = "the file ends before its data does", false;
    return true;
}

void decode(const Prepared& p, std::vector<uint16_t>& readout, unsigned threads)
{
    readout.assign(size_t(p.readout_w) * p.readout_h, 0);
    if (!threads)
        threads = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t units = p.units();
    threads = std::min<unsigned>(threads, std::max<uint32_t>(1, units / 1024));  // a thread at least 1024 pairs of blocks
    parallel(
        units,
        [&](size_t unit)
        {
            const uint32_t u = uint32_t(unit);
            uint16_t s[16];
            uint8_t tmp[16];
            if (p.format == Format::sony)
            {
                // Blocks 2j and 2j + 1 of a row: columns 32j on, even and odd.
                const uint32_t row_units = uint32_t(p.row_blocks + 1) / 2, row = u / row_units, j = u % row_units;
                const uint32_t b = row * uint32_t(p.row_blocks) + 2 * j;
                uint16_t* o = readout.data() + size_t(row) * p.readout_w + 32 * j;
                const int col = int(32 * j);
                for (int k = 0; k < 2; ++k)
                    if (int(2 * j) + k < p.row_blocks && col + k < p.readout_w - 30)
                    {
                        sony(block(p, b + k, tmp), p.curve.data(), s);
                        for (int i = 0; i < 16; ++i)
                            o[2 * i + k] = s[i];
                    }
                return;
            }
            for (uint32_t b = 2 * u; b < std::min(2 * u + 2, p.blocks); ++b)
            {
                const uint8_t* d = block(p, b, tmp);
                uint16_t* o = readout.data() + size_t(b) * p.per;
                if (p.format == Format::panasonic)
                    panasonic(d, o);
                else if (p.format == Format::packed)
                    packed(d, p.bits, o);
                else
                    panasonic6(d, p.bits, o);
            }
        },
        threads);
}

bool fits_gpu(const Prepared& p)
{
    return uint64_t(p.readout_w) * p.readout_h * 2 <= gpu::kMaxBuffer && p.extent() <= (uint64_t(256) << 20);
}

bool written(const Prepared& p, int x, int y)
{
    if (y >= p.rows || x >= p.readout_w)
        return false;
    if (p.format != Format::sony)
        return true;
    const int j = x / 32, k = x & 1;
    return 2 * j + k < p.row_blocks && 32 * j + k < p.readout_w - 30;
}

}  // namespace rawblocks
