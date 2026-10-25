// Decoding a raw's lossless JPEG on the GPU (gpu_view.cpp's decode_stream;
// ljpeg.h has the method, ljpeg.cpp the same passes on the CPU). The stream
// is cut into chunks; every pass but the last few takes a chunk a thread.
//
//   guess    each chunk decoded from its start as if a code began there,
//            once for each component that could be due
//   sync     for each way into a chunk (out of the chunk before, by each of
//            its guesses), which of its own guesses the true decode meets
//   resolve  those chained from the start: each chunk's true way in, and
//            where its samples go
//   decode   each chunk from its true way in: the differences, in place
//   columns, rows  the differences summed into samples
//   black    the masked areas summed, for the black level
//
// Samples are 16 bits, two to a word; rows have an even number of them, so
// a row's words are its own.

cbuffer Ljpeg : register(b0)
{
    uint total_bits;  // in the stream
    uint chunk_bits;
    uint chunks;
    uint components;
    uint samples;     // all of them: row * rows
    uint row;         // a row's: the frame's width * components
    uint rows;
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
    uint pad0;
    uint pad1;
};

// Where the readout's (x, y) is among the samples: Canon's stream fills each
// slice top to bottom, slice after slice.
uint readout_index(uint x, uint y)
{
    if (slice_n == 0)
        return y * readout_w + x;
    const uint i = min(x / slice_w, slice_n);
    const uint w = i < slice_n ? slice_w : slice_last;
    return i * slice_w * readout_h + y * w + (x - i * slice_w);
}

#define SAMPLE(buffer, i) ((buffer.Load(((i) >> 1) * 4) >> (((i) & 1) * 16)) & 0xFFFF)

#ifdef LJPEG_BITS
RWByteAddressBuffer stream : register(u0);  // big-endian bytes, then padding
RWByteAddressBuffer lookup : register(u1);  // per component, 65536 words: code length << 8 | size

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

// One sample's difference at `pos` with component `c`'s table. False for a
// code that doesn't exist: a guess takes it as a one-bit symbol and goes on.
bool next_symbol(inout uint pos, uint c, out int diff)
{
    const uint v = peek(pos);
    const uint e = lookup.Load((c * 65536 + (v >> 16)) * 4);
    const uint len = e >> 8, size = e & 0xFF;
    diff = 0;
    if (len == 0)
    {
        pos += 1;
        return false;
    }
    pos += len;
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
