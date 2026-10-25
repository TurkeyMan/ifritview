#pragma once

// Raws compressed in small blocks, each decoded on its own: Sony's
// compressed ARW (LibRaw's sony_arw2_load_raw: 16 samples of one colour in
// 16 bytes, the block's least and greatest and 7-bit steps between) and
// Panasonic's RW2 (panasonic_load_raw: 14 samples in 16 bytes, predicted
// within the block; its encoding 5, and panasonicC6/C7_load_raw's: samples
// packed whole or near enough). No block depends on another, so they're
// decoded a thread a pair of blocks, here on every core or on the GPU
// (raw_blocks.hlsl does the same).
//
// Decoded into the readout as LibRaw lays it out, raw_width x raw_height;
// only `rows` rows from the top are written (the rest LibRaw leaves as it
// finds them), and Sony's last columns when the width isn't whole blocks.
// Not taken: Panasonic's encoding 8 (panasonicC8_load_raw: a few long
// Huffman-coded stripes).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rawblocks
{

enum class Format
{
    sony,        // sony_arw2_load_raw: a block 16 samples, every other column, two blocks 32 columns; then the curve
    panasonic,   // panasonic_load_raw (not encoding 5): 14 samples, predicted, its bits read from the block's top
    packed,      // panasonic_load_raw's encoding 5, panasonicC7_load_raw: samples 12 bits (10 a block) or 14 (9)
    panasonic6,  // panasonicC6_load_raw: 12 bits (14 a block) or 14 (11), in steps of a base
};

constexpr uint32_t kBuffer = 0x4000;  // panasonic_load_raw reads 16 KB at a time

// LibRaw's layout of a raw (from its identify), and the blocks where they lie
// in the file: read in place, here and on the GPU, nothing copied.
struct Prepared
{
    Format format = Format::sony;
    int bits = 12;  // a sample's (Panasonic's 12 or 14)
    int per = 16;   // samples a block
    int readout_w = 0, readout_h = 0;
    int rows = 0;        // rows written, from the top (Sony: `height`; C6, C7: whole 16s of raw_height)
    int row_blocks = 0;  // Sony's: blocks a row (a row raw_width bytes)
    uint32_t blocks = 0;
    // Panasonic's 16 KB buffer (encodings 4 and 5): each 16 KB of the file
    // goes into it `rotation` bytes in, wrapping round (load_flags, 0x2008).
    bool rotated = false;
    uint32_t rotation = 0;
    const uint8_t* data = nullptr;  // the file's from where the readout starts
    uint64_t size = 0;              // bytes there to the file's end
    std::vector<uint16_t> curve;    // Sony's: 4096 entries, a block's 11-bit values (doubled) into the readout's
    // Pairs of blocks, what a thread decodes (Sony's in a row, the same columns).
    uint32_t units() const { return format == Format::sony ? uint32_t(rows) * ((row_blocks + 1) / 2) : (blocks + 1) / 2; }
    // The bytes of the file the blocks are read from, from `data` on.
    uint64_t extent() const;
};

// From what LibRaw's identify found: `p`'s format, bits, readout size and
// rows, rotation set, `curve` Sony's (0x10000 entries). False (and why) for a
// layout this doesn't decode exactly as LibRaw does.
bool prepare(const uint8_t* file, size_t size, uint64_t offset, const uint16_t* curve, Prepared& p, std::string& error);
// Into `readout` (raw_width x raw_height, unwritten samples 0). `threads` 0:
// as many as there are cores.
void decode(const Prepared& p, std::vector<uint16_t>& readout, unsigned threads = 0);
// Whether LibRaw writes the readout's (x, y) (the rest it leaves as it finds it).
bool written(const Prepared& p, int x, int y);
// Whether the GPU takes it: the readout at most 128 MB, the file's part 256.
bool fits_gpu(const Prepared& p);

}  // namespace rawblocks
