#pragma once

// A DNG's readout in tiles that LibRaw decodes one after another on one
// core, decoded here a tile a thread, each as LibRaw has it: a lossy DNG's
// JPEG tiles (Compression 34892: libjpeg's decode, as LibRaw's, its 8-bit
// samples through each colour's curve), floating-point tiles deflated
// (Compression 8, the floating-point predictor undone) or stored as they
// are, widened to floats as the DNG SDK does. The tiles fill the readout
// a row of them after another; past its edges they're cut. Read in place
// in the file, nothing copied.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dngtiles
{

enum class Format
{
    lossy,     // baseline JPEG
    deflated,  // floats, zlib's
    floats,    // floats as they are
};

struct Prepared
{
    Format format = Format::lossy;
    const uint8_t* file = nullptr;
    size_t size = 0;
    std::vector<uint64_t> offsets, lengths;  // each tile's data: where, how long (lossy: to the file's end)
    unsigned tile_w = 0, tile_h = 0, across = 1;
    unsigned width = 0, height = 0, samples = 1;  // what the tiles fill, its samples a photosite
    // Floats: bytes a sample (2: IEEE halves, 3: the DNG SDK's 24 bits, 4),
    // the file's byte order (stored as they are), and the predictor's
    // columns a sample (deflated: 1; 2, 4 for FloatingPointX2, X4).
    int bytes = 4, factor = 1;
    bool big_endian = false;
    std::vector<uint16_t> curves;  // lossy: 256 levels each colour
};

// A lossy DNG's: `out` width x height x samples, zero where no tile reaches.
bool decode(const Prepared& p, std::vector<uint16_t>& out, std::string& error);
// Floats: `out` as decode's; `largest` the greatest of every tile's samples
// in its rows inside the readout, all its columns (LibRaw's fmaximum).
bool decode(const Prepared& p, std::vector<float>& out, float& largest, std::string& error);

}  // namespace dngtiles
