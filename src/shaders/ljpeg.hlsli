// Decoding a raw's lossless JPEG on the GPU (gpu_view.cpp's decode_stream;
// ljpeg.h has the method, ljpeg.cpp the same passes on the CPU). The
// streams (one; a DNG's tiles) are cut into chunks, each in one stream;
// every pass but the last few takes a chunk a thread.
//
//   guess    each chunk decoded from its start as if a code began there,
//            once for each component that could be due
//   sync     for each way into a chunk (out of the chunk before, by each of
//            its guesses), which of its own guesses the true decode meets
//   resolve  those chained from each stream's start: each chunk's true way
//            in, and where its samples go
//   decode   each chunk from its true way in: the differences, in place
//   columns, rows  the differences summed into samples
//   black    the masked areas summed, for the black level
//
// Samples are 16 bits, two to a word, a stream's after another's; rows have
// an even number of them, so a row's words are its own.

cbuffer Ljpeg : register(b0)
{
    uint parts;       // streams: one, or a DNG's tiles
    uint chunk_bits;
    uint chunks;      // all the streams'
    uint components;
    uint samples;     // a stream's: row * rows
    uint row;         // a row's: the frame's width * components
    uint rows;        // a stream's
    uint precision;
    uint readout_w;   // the readout the samples fill, as LibRaw lays it out
    uint readout_h;
    uint slice_n;     // Canon's slices: how many, each one's width, the last one's (0: rows as they come)
    uint slice_w;
    uint slice_last;
    uint left;        // the active area's corner in the readout
    uint top;
    uint filters;     // LibRaw's: the colour at (row, col) of the readout
    int4 rect;        // black: the masked area summed, top, left, bottom, right
    uint mosaic_w;    // the active area's size
    uint mosaic_h;
    uint scheme;      // bit 0: Nikon's (a difference's bits as Nikon codes them); bits 8..15: rows a row's first samples continue from
    uint curved;      // 1: each sample through the curve (65536 entries) as it's written
    uint4 initial;    // the first rows' first samples: row 0's components 0, 1 | row 1's 0, 1 (16 bits each), then 2, 3 the same
    uint tile_w;      // a DNG's tiles: each one's width and height, how many across (0: none)
    uint tile_h;
    uint tiles_across;
    uint unused;
};

uint start_of(uint r, uint c)  // initial[], row r (< step), component c
{
    const uint w = initial[(c >> 1) * 2 + r];
    return (c & 1) ? w >> 16 : w & 0xFFFF;
}

// Where the readout's (x, y) is among the samples: a DNG's tiles one after
// another, each filled a row after another; Canon's stream fills each slice
// top to bottom, slice after slice (as RawMosaic::Stream::index).
uint readout_index(uint x, uint y)
{
    if (tile_w != 0)
    {
        const uint tx = x / tile_w, ty = y / tile_h;
        return (ty * tiles_across + tx) * samples + (y - ty * tile_h) * tile_w + (x - tx * tile_w);
    }
    if (slice_n == 0)
        return y * readout_w + x;
    const uint i = min(x / slice_w, slice_n);
    const uint w = i < slice_n ? slice_w : slice_last;
    return i * slice_w * readout_h + y * w + (x - i * slice_w);
}

#define SAMPLE(buffer, i) ((buffer.Load(((i) >> 1) * 4) >> (((i) & 1) * 16)) & 0xFFFF)

// Per stream, 8 words: its first bit, the bit after its last, its first
// chunk, how many chunks, then each component's table (its first word).
#define PART_WORDS 8

#ifdef LJPEG_BITS
RWByteAddressBuffer stream : register(u0);  // big-endian bytes, each stream's padded
RWByteAddressBuffer meta : register(u1);    // the streams (PART_WORDS each), then the tables (ljpeg::Table's words)

// ljpeg::Table in words: the lookup on the next 10 bits (two 16-bit entries
// a word), then maxcode[17], delta[17], symbols[32] (bytes).
#define FAST_BITS 10
#define MAXCODE 512
#define DELTA 529
#define SYMBOLS 546

uint swap_bytes(uint x)
{
    return (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24);
}

// The 32 bits from bit `pos` on.
uint peek(uint pos)
{
    const uint at = (pos >> 5) * 4, shift = pos & 31;
    const uint a = swap_bytes(stream.Load(at));
    return shift ? (a << shift) | (swap_bytes(stream.Load(at + 4)) >> (32 - shift)) : a;
}

// Stream p's first bit, end, first chunk, chunks.
uint4 part_at(uint p)
{
    return meta.Load4(p * PART_WORDS * 4);
}

// The stream chunk k is in: the last whose first chunk isn't past it.
uint part_of(uint k)
{
    uint lo = 0, hi = parts - 1;
    while (lo < hi)
    {
        const uint mid = (lo + hi + 1) >> 1;
        if (meta.Load((mid * PART_WORDS + 2) * 4) <= k)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

// Stream p's tables, for components 0..3.
uint4 tables_of(uint p)
{
    return meta.Load4((p * PART_WORDS + 4) * 4);
}

uint table_for(uint4 t, uint c)
{
    return c == 0 ? t.x : c == 1 ? t.y : c == 2 ? t.z : t.w;
}

// One sample's difference at `pos` with the table at word `t`. False for a
// code that doesn't exist: a guess takes it as a one-bit symbol and goes on.
bool next_symbol(inout uint pos, uint t, out int diff)
{
    const uint v = peek(pos);
    const uint f = v >> (32 - FAST_BITS);
    const uint e = (meta.Load((t + (f >> 1)) * 4) >> ((f & 1) * 16)) & 0xFFFF;
    uint len = e >> 8, size = e & 0xFF;
    diff = 0;
    if (len == 0)
    {
        // Longer than the lookup: found by its length.
        for (uint l = FAST_BITS + 1; l <= 16; ++l)
        {
            const int code = int(v >> (32 - l));
            if (code <= int(meta.Load((t + MAXCODE + l) * 4)))
            {
                const uint i = uint(code + int(meta.Load((t + DELTA + l) * 4)));
                size = (meta.Load((t + SYMBOLS + (i >> 2)) * 4) >> ((i & 3) * 8)) & 0xFF;
                len = l;
                break;
            }
        }
        if (len == 0)
        {
            pos += 1;
            return false;
        }
    }
    pos += len;
    if (scheme & 1)
    {
        // Nikon's: a length and a shift; that many bits less the shift,
        // shifted back up with a one under them.
        const uint bits = size & 15, shift = size >> 4, n = bits > shift ? bits - shift : 0;
        uint x = 0;
        if (n)
        {
            x = (v << len) >> (32 - n);
            pos += n;
        }
        diff = int(((x << 1) + 1) << shift >> 1);
        if (bits > 0 && (diff & int(1u << (bits - 1))) == 0)
            diff -= int(1u << bits) - (shift == 0 ? 1 : 0);
        return true;
    }
    if (size == 16)
        diff = -32768;
    else if (size != 0)
    {
        const uint x = (v << len) >> (32 - size);
        pos += size;
        diff = x < (1u << (size - 1)) ? int(x) - int((1u << size) - 1) : int(x);
    }
    return true;
}

uint following(uint c)
{
    return c + 1 == components ? 0 : c + 1;
}
#endif
