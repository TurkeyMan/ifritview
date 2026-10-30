#pragma once

// Panasonic's RW2 encoding 8 (LibRaw's panasonicC8_load_raw: the GH6, GH7,
// G9 II and their kin): the readout in a few vertical stripes, each one long
// stream of Huffman codes, its bits taken from each byte's lowest up. A
// stripe's rows go in pairs, a pair in groups of four samples (a 2 x 2),
// each sample predicted from the same place in the group before, the first
// group's from the pair above's first; held to 0..`top`, then through the
// camera's curve. The stripes are few, so each is decoded on every core
// (chunked.h: the codes resynchronise), its differences summed after.
//
// Decoded into the readout as LibRaw lays it out, raw_width x raw_height.
// Not taken: codes longer than 16 bits (LibRaw's slower lookup), a stripe of
// odd size.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pana8
{

// The tags LibRaw read (its pana8_tags_t), named for what they do.
struct Header
{
    uint32_t curve_shift[6] = {};   // 0x39: each segment of the curve, how its steps go (LibRaw's tag39)
    uint16_t curve_start[6] = {};   // 0x3A: where each segment starts
    uint16_t top = 0;               // 0x3B: samples held to it, the curve too
    uint16_t initial[4] = {};       // 0x3C..0x3F: the first group's predictions
    uint16_t code_length[17] = {};  // 0x40: each symbol's code, its length (low 5 bits)
    uint16_t code[17] = {};         // and its bits
    uint16_t shift[17] = {};        // 0x41: bits a symbol's difference drops at the bottom
    int stripes = 0;                // 0x42, then each one's
    int64_t offset[5] = {};         // where it starts in the file
    uint16_t left[5] = {};          // its first column
    uint32_t bits[5] = {};          // how long it is
    uint16_t width[5] = {}, height[5] = {};
};

struct Prepared
{
    int readout_w = 0, readout_h = 0;
    struct Stripe
    {
        const uint8_t* data = nullptr;  // in the file
        size_t bytes = 0;               // there to the file's end, at most as many as LibRaw reads (whole 8s)
        uint64_t bits = 0;              // as LibRaw reads it: whole 8-byte words
        int left = 0, width = 0, height = 0;
    };
    std::vector<Stripe> stripes;
    std::vector<uint8_t> symbol;  // the symbol whose code the next 16 bits start with
    uint32_t coeff[18] = {};      // each symbol's shift (bits 24..28), code length (16..20) and code, as LibRaw packs them
    int top = 0;
    uint16_t initial[4] = {};
    std::vector<uint16_t> curve;  // 65536 entries; none: as they are
};

// False (and why) for what this doesn't decode exactly as LibRaw does.
bool prepare(const uint8_t* file, size_t size, const Header& h, int readout_w, int readout_h, Prepared& p, std::string& error);
// Into `readout` (readout_w x readout_h). `threads` 0: as many as there are cores.
bool decode(const Prepared& p, std::vector<uint16_t>& readout, std::string& error, unsigned threads = 0);

}  // namespace pana8
