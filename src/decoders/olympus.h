#pragma once

// Olympus's and OM System's compressed ORF, as LibRaw's olympus_load_raw
// reads it: every photosite an adaptive Golomb-like code (its sign, a run of
// zeros for its high bits, as many low bits as the codes two photosites back
// say), predicted from those two columns back and two rows up (the same
// colour), as the maker notes' tags 0x640..0x653 have it.
//
// One bitstream for the whole readout, nothing restarting it and nothing to
// say where a row starts, so its bits are read on one thread. Not as
// ljpeg.h reads a Huffman stream, from guessed starts: a code's width
// comes from the codes two before it and from its column (a row's first
// two are fixed), and a guess doesn't know its column; even in step with
// the true decode it would count columns otherwise and part from it again
// at the next row. What the codes say doesn't depend on the photosites
// predicted from them, and each row starts afresh but for where its bits
// begin: the predictions (a row from the one two up) go on other threads
// behind the reading, a row each in turn, a few columns behind the row two
// up. Twice as fast as LibRaw's, all told.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace olympus
{

struct Prepared
{
    const uint8_t* data = nullptr;  // the codes, past the header
    size_t size = 0;
    int width = 0, height = 0;      // the readout's
    unsigned tag[14] = {};          // the maker notes' 0x640..0x653
};

// From the readout at `offset` in `file` (its header checked) and the tags.
// False (and why) for one that isn't, or one this doesn't read as LibRaw does.
bool prepare(const uint8_t* file, size_t size, uint64_t offset, int width, int height, const unsigned tag[14], Prepared& p, std::string& error);
// Into `readout` (width x height). False for a code LibRaw stops at.
bool decode(const Prepared& p, std::vector<uint16_t>& readout, std::string& error);

}  // namespace olympus
