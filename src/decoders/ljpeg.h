#pragma once

// Lossless JPEG (ITU-T T.81 process 14, SOF3), as cameras store their sensor
// data (Canon's CR2, DNG's tiles): every sample a Huffman-coded difference
// from its left (or, first in its row, upper) neighbour. The bitstream is one
// serial stream, yet it's decoded on several threads: Huffman codes
// resynchronise, so each thread starts mid-stream wherever its share begins,
// and once its decode meets the true one on a code boundary they agree from
// there on (ljpeg.cpp has the passes).
//
// Not taken (false, for the caller to decode another way): predictors other
// than the left neighbour, point transforms, restart intervals, components
// sampled other than 1 x 1 (Canon's sRAW).

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
    std::vector<uint16_t> samples;  // `height` rows of width * components
};

// The stream as the decoding passes take it, here or on the GPU: the
// entropy-coded data without its stuffed zero bytes (then 8 zero bytes, so
// any bit can be read 64 at a time), and each component's Huffman table as a
// lookup on the next 16 bits: the code's length in the high byte (0: no such
// code), its symbol (the difference's size in bits) in the low one.
struct Prepared
{
    int width = 0, height = 0, components = 0, precision = 0;  // as Image's
    std::vector<uint8_t> bits;
    uint64_t total_bits = 0;
    std::vector<uint16_t> lookup[4];  // 65536 entries each, for components 0..components-1
};

// `data` starts at the SOI marker. `threads` 0: as many as there are cores.
bool prepare(const uint8_t* data, size_t size, Prepared& out, std::string& error, unsigned threads = 0);
bool decode(const Prepared& stream, Image& out, std::string& error, unsigned threads = 0);
bool decode(const uint8_t* data, size_t size, Image& out, std::string& error, unsigned threads = 0);  // both

}  // namespace ljpeg
