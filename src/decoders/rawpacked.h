#pragma once

// Raws stored uncompressed, as LibRaw's packed_dng_load_raw,
// unpacked_load_raw and packed_load_raw read them: samples of `bits` bits
// one after another, most significant bit first, rows `row_bits` apart, the
// bytes read in groups of `group` taken as little-endian words (1: as they
// come; 2: 16-bit little-endian samples). Each is shifted right by `shift`
// and put through `curve` if there is one; `swap`: each pair of columns the
// other way round. The GPU unpacks it (unpack.hlsl), unpack() the same here
// on every core. Read in place in the file, nothing copied.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rawpacked
{

struct Prepared
{
    const uint8_t* file = nullptr;
    size_t offset = 0;  // the first row's, in `file`
    size_t bytes = 0;   // all the rows', whole groups
    uint32_t row_bits = 0;
    int bits = 16, group = 1, shift = 0;
    bool swap = false;
    std::vector<uint16_t> curve;  // 65536 entries, or none
    int readout_w = 0, readout_h = 0;
};

// The readout, readout_w x readout_h, into `out`.
void unpack(const Prepared& p, std::vector<uint16_t>& out);

}  // namespace rawpacked
