#pragma once

// Canon's CR3 (CRX): the readout as four planes, one a colour of the 2 x 2
// pattern, each cut into tiles, each tile's plane into subbands coded on
// their own: an adaptive Golomb-Rice code, each sample predicted from its
// line and the line above, runs of repeats coded apart. Lossless RAW has a
// subband a plane, the samples themselves; cRAW three levels of a 5/3
// wavelet, quantised (by a step a subband, or with version 0x200 a step for
// every 8 x 2 samples, coded at the tile's head). As LibRaw's crxLoadRaw
// decodes it, bit for bit; here every subband's code is decoded on its own
// thread (they're independent), then each tile's plane's wavelet, also a
// thread each, into its plane. A code's samples depend on all before them,
// so a lossless R5's four planes are four threads at most, and nothing of
// the decode is the GPU's: it gets the planes, puts them together and
// measures the black (gpu_view's assemble_planes).
//
// Not taken: a plane rather than four (8-bit), encodings 1 and 3 (samples
// signed, or the four planes a colour transform of the 2 x 2), a tile
// without data.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crx
{

// The raw track's header (its CMP1 box) as LibRaw's crxParseImageHeader
// reads it, and where its data lies.
struct Header
{
    int version = 0;                // 0x100, or 0x200 (quantisation steps a tile)
    int width = 0, height = 0;      // the readout's
    int tile_w = 0, tile_h = 0;     // the readout's
    int bits = 0;                   // a sample's
    int planes = 0, cfa = 0;        // planes, and how they lie in the 2 x 2 (0 R G / G B ... 3 B G / G R)
    int encoding = 0, levels = 0;   // encType; the wavelet's levels (0: lossless)
    uint32_t header_size = 0;       // the tiles' headers', at the data's start
    uint64_t offset = 0, size = 0;  // the data's, in the file
};

struct Subband
{
    int width = 0, height = 0;
    uint64_t offset = 0, size = 0;  // its code's, in the file (0 bytes: all zero)
    bool partial = false;           // each line's step coded before it (supportsPartial)
    int q = 4;                      // the step, version 0x100's
    int32_t step_base = 0;          // version 0x200's: the tile's steps scaled by
    uint32_t step_mult = 0;
    int row_start = 0, row_end = 0, col_start = 0, col_end = 0, level_shift = 0;  // where it lies against the steps
};

struct Tile
{
    int width = 0, height = 0;  // a plane's
    int x = 0, y = 0;           // its corner in the plane
    int flags = 0;              // tiles on its right 1, left 2, below 4, above 8
    uint64_t steps_offset = 0;  // its quantisation steps' code (version 0x200), in the file
    uint32_t steps_size = 0;    // 0: none
    struct Plane
    {
        std::vector<Subband> bands;  // the coarsest's LL first, then each level's HL, LH, HH
        bool partial = false;
        int rounded = 0;  // lossless with its least bits dropped: their mask
    } planes[4];
};

struct Prepared
{
    Header header;
    int plane_w = 0, plane_h = 0;
    int tile_w = 0, tile_h = 0;  // a plane's
    std::vector<Tile> tiles;     // row by row
    const uint8_t* file = nullptr;
    size_t size = 0;
};

// The tiles and subbands from the headers at the data's start. False (and
// why) for a variant this doesn't decode, or anything amiss.
bool prepare(const uint8_t* file, size_t size, const Header& h, Prepared& p, std::string& error);
// The readout as `planes`: each of the 2 x 2's places a plane (row by row,
// then column), width / 2 x height / 2, one after another; (x, y) is plane
// (y & 1) * 2 + (x & 1)'s (x / 2, y / 2). Every subband a thread, then every
// tile's plane's wavelet (lossless: its one subband straight in).
bool decode(const Prepared& p, std::vector<uint16_t>& planes, std::string& error);
// The readout (width x height) from its planes, on every core.
void readout(const Prepared& p, const std::vector<uint16_t>& planes, std::vector<uint16_t>& out);

}  // namespace crx
