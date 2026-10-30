#pragma once

// Fujifilm's compressed RAF (LibRaw's fuji_compressed_load_raw): the readout
// cut into strips 768 photosites wide, each coded on its own, six rows (a
// "line") at a time. Every sample is predicted from the rows above of its
// colour and coded as an adaptive Golomb-like difference, its parameter
// from the gradient around it (each strip's statistics its own); a lossy
// file quantises the differences by a step a line. As LibRaw decodes it,
// bit for bit; a strip's samples depend on all before them, so a thread a
// strip, here on every core (an X-T3 has 9, a GFX100S 16). Nothing of it is
// the GPU's: a few long serial codes are what its threads do worst.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fuji
{

// The compressed header as LibRaw's parse_fuji_compressed_header leaves it,
// and the sensor's pattern.
struct Header
{
    int bits = 0;            // 12, 14 or 16
    bool lossless = false;
    bool xtrans = false;     // raw_type 16: the 6 x 6 X-Trans; else Bayer
    int block_width = 0;     // a strip's, 768
    int blocks = 0;          // strips across
    int lines = 0;           // six rows each
    int readout_w = 0, readout_h = 0;
    uint64_t offset = 0;     // the strip sizes', past the header
    int colour[6][6] = {};   // the filter at (row, col) of the readout: 0 R, 1 G, 2 B (a Bayer's 2 x 2 repeated)
};

struct Prepared
{
    Header header;
    std::vector<uint64_t> offsets, sizes;  // each strip's code, in the file
    const uint8_t* q_bases = nullptr;      // a lossy file's steps, a line a byte, a strip's after another's (rows of `q_stride`)
    int q_stride = 0;
    const uint8_t* file = nullptr;
    size_t size = 0;
};

// The strips from their sizes at the data's start. False (and why) for
// anything amiss.
bool prepare(const uint8_t* file, size_t size, const Header& h, Prepared& p, std::string& error);
// Into `readout` (readout_w x readout_h), a strip a thread.
void decode(const Prepared& p, std::vector<uint16_t>& readout);

}  // namespace fuji
