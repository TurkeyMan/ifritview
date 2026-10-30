#pragma once

// Lossless JPEG (ITU-T T.81 process 14, SOF3), as cameras store their sensor
// data (Canon's CR2, DNG's tiles): every sample a Huffman-coded difference
// from a prediction, its left neighbour's (of its component) mostly. The
// bitstream is one serial stream, yet it's decoded on several threads:
// Huffman codes resynchronise, so each thread starts mid-stream wherever its
// share begins, and once its decode meets the true one on a code boundary
// they agree from there on (chunked.h has the passes). A DNG's tiles are
// streams of their own, decoded side by side.
//
// Restart intervals of whole rows predicted from the left (a phone's DNG)
// are streams of their own too, as LibRaw reads them.
//
// Sony's YCbCr (its ARW's medium and small sizes) has its luma sampled 2 x
// 1 or 2 x 2, each MCU's codes the luma's then the two colours', summed as
// LibRaw's LjpegDecompressor does: on the CPU only, and only when asked.
//
// Not taken (false, for the caller to decode another way): point
// transforms, other restart intervals, other components sampled other than
// 1 x 1 (Canon's sRAW). Predictors other than the left neighbour (rows
// predicted from the row above) are decoded on the CPU only.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ljpeg
{

struct Image
{
    int width = 0, height = 0;      // the frame's X and Y: samples per component per row, rows
    int components = 0;             // interleaved in every row (Sony's YCbCr: Y, Cb, Cr at every pixel)
    int precision = 0;              // bits per sample
    std::vector<uint16_t> samples;  // `height` rows of width * components; a stream's after another's
};

constexpr int kCodes = 6;  // an MCU's at most (Sony's 2 x 2 luma, then Cb and Cr)

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
    // Sony's YCbCr: its luma's samples an MCU (2: 2 x 1, 4: 2 x 2), each a
    // code of its own before the colours'; `components` then counts an MCU's
    // codes, and `width` and `height` are in MCUs. 1: every component 1 x 1.
    int luma = 1;
    // 1: each sample predicted from its left; 2..7 as the standard numbers
    // them (the row above in it): decoded on the CPU only.
    int predictor = 1;
    std::vector<uint8_t> bits;  // the streams one after another, each padded
    struct Part
    {
        uint64_t start = 0, bits = 0;  // where in `bits` (in bits, a byte's start), how long
        uint32_t table[kCodes] = {};   // components 0..components-1's (an MCU's codes'), in `tables`
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
// `file`, read to the marker that ends it, every frame alike. `ycc`: Sony's
// YCbCr taken.
bool prepare_tiles(const uint8_t* file, size_t size, const std::vector<uint64_t>& offsets, Prepared& out, std::string& error,
                   unsigned threads = 0, bool ycc = false);
// A Nikon NEF's compressed data (`data`: from where it starts): `columns`
// samples a row (the readout's width, two columns interleaved), `rows` of
// them, coded by `tree` (16 counts, then the symbols), the first two rows'
// first two samples `start`, and its curve (indices clamped to it).
bool prepare_nikon(const uint8_t* data, size_t size, int columns, int rows, const uint8_t tree[32], const uint16_t start[2][2],
                   const uint16_t* curve, size_t curve_size, Prepared& out, std::string& error);
// Whether prepare and decode take this stream (its headers only: quick),
// and its predictor (the GPU's decode takes 1 only).
bool takes(const uint8_t* data, size_t size, int* predictor = nullptr);
bool decode(const Prepared& stream, Image& out, std::string& error, unsigned threads = 0);
bool decode(const uint8_t* data, size_t size, Image& out, std::string& error, unsigned threads = 0);  // both

}  // namespace ljpeg
