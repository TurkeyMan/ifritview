#pragma once

// Lossless JPEG (ITU-T T.81 process 14, SOF3), as cameras store their sensor
// data (Canon's CR2, DNG's tiles): every sample a Huffman-coded difference
// from a prediction, its left neighbour's (of its component) mostly. The
// bitstream is one serial stream, yet it's decoded on several threads:
// Huffman codes resynchronise, so each thread starts mid-stream wherever its
// share begins, and once its decode meets the true one on a code boundary
// they agree from there on (ljpeg.cpp has the passes). A DNG's tiles are
// streams of their own, decoded side by side.
//
// Restart intervals of whole rows predicted from the left (a phone's DNG)
// are streams of their own too, as LibRaw reads them.
//
// Not taken (false, for the caller to decode another way): point
// transforms, other restart intervals, components sampled other than 1 x 1
// (Canon's sRAW). Predictors other than the left neighbour (rows predicted
// from the row above) are decoded on the CPU only.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ljpeg
{

struct Image
{
    int width = 0, height = 0;      // the frame's X and Y: samples per component per row, rows
    int components = 0;             // interleaved in every row
    int precision = 0;              // bits per sample
    std::vector<uint16_t> samples;  // `height` rows of width * components; a stream's after another's
};

// A Huffman table as the decoding passes take it, here or on the GPU (laid
// out as words for it): a code up to kFast bits long looked up on the next
// kFast bits (its length in the high byte, 0: a longer code or none; its
// symbol, the difference's size in bits, in the low one); a longer one found
// by its length as the standard decodes (F.2.2.3): the codes `len` bits long
// run up to maxcode[len] (-1: none), their symbols at symbols[code + delta[len]].
struct Table
{
    static constexpr int kFast = 10;
    uint16_t fast[1 << kFast] = {};
    int32_t maxcode[17] = {};
    int32_t delta[17] = {};
    uint8_t symbols[32] = {};
};
static_assert(sizeof(Table) == 2216, "ljpeg.hlsli's layout");

// The streams as the decoding passes take them: the entropy-coded data
// without its stuffed zero bytes (then 8 zero bytes, so any bit can be read
// 64 at a time), each component's table.
struct Prepared
{
    int width = 0, height = 0, components = 0, precision = 0;  // each stream's frame, as Image's
    // 1: each sample predicted from its left; 2..7 as the standard numbers
    // them (the row above in it): decoded on the CPU only.
    int predictor = 1;
    std::vector<uint8_t> bits;  // the streams one after another, each padded
    struct Part
    {
        uint64_t start = 0, bits = 0;  // where in `bits` (in bits, a byte's start), how long
        uint32_t table[4] = {};        // components 0..components-1's, in `tables`
        int rows = 0;                  // the frame's rows it holds: `height`, fewer in the last of a strip's restart intervals
    };
    // One stream; a DNG's tiles, in its order; a strip's restart intervals
    // (whole rows, `height` each), each a stream of its own.
    std::vector<Part> parts;
    std::vector<Table> tables;
    // A row's first samples continue those `step` rows above; the first
    // `step` rows' from `initial` (per row and component).
    int step = 1;
    uint16_t initial[2][4] = {};
    // Nikon's NEF (no JPEG framing, nothing stuffed): one table for both
    // columns' samples, a difference's bits as Nikon codes them (its tree's
    // symbol a length and a shift).
    bool nikon = false;
    // Every sample through it once decoded (65536 entries; none: as they
    // are): a Nikon's curve, a DNG's linearisation.
    std::vector<uint16_t> curve;
};

// `data` starts at the SOI marker. `threads` 0: as many as there are cores.
bool prepare(const uint8_t* data, size_t size, Prepared& out, std::string& error, unsigned threads = 0);
// A DNG's tiles (or its one strip): each a lossless JPEG at `offsets` in
// `file`, read to the marker that ends it, every frame alike.
bool prepare_tiles(const uint8_t* file, size_t size, const std::vector<uint64_t>& offsets, Prepared& out, std::string& error,
                   unsigned threads = 0);
// A Nikon NEF's compressed data (`data`: from where it starts): `columns`
// samples a row (the readout's width, two columns interleaved), `rows` of
// them, coded by `tree` (16 counts, then the symbols), the first two rows'
// first two samples `start`, and its curve (indices clamped to it).
bool prepare_nikon(const uint8_t* data, size_t size, int columns, int rows, const uint8_t tree[32], const uint16_t start[2][2],
                   const uint16_t* curve, size_t curve_size, Prepared& out, std::string& error);
// A Pentax PEF's (pentax_load_raw's): as a NEF's, `columns` samples a row,
// `rows` of them, each row's first two continuing those two rows up (the
// first two rows' from 0), the differences as a JPEG's, coded by `n` codes:
// `codes[i]` its bits from the top of 12, `lengths[i]` of them, its symbol
// i. False for codes that aren't a JPEG table's (canonical).
bool prepare_pentax(const uint8_t* data, size_t size, int columns, int rows, int precision, const uint16_t* codes, const uint8_t* lengths, int n,
                    Prepared& out, std::string& error);
// Whether prepare and decode take this stream (its headers only: quick),
// and its predictor (the GPU's decode takes 1 only).
bool takes(const uint8_t* data, size_t size, int* predictor = nullptr);
bool decode(const Prepared& stream, Image& out, std::string& error, unsigned threads = 0);
bool decode(const uint8_t* data, size_t size, Image& out, std::string& error, unsigned threads = 0);  // both

}  // namespace ljpeg
