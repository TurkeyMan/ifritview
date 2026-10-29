#pragma once
// A Phase One's corrections of its readout, as LibRaw makes them before it
// develops one (phase_one_subtract_black, then phase_one_correct), for the
// develop to make instead (dng_opcodes.h's layout): its black less offsets
// per row and column, held at 0; its curves (a polynomial, quarter
// multipliers and linearisations, composed into a table a quarter); its
// defective photosites and columns; and its flat fields, gain maps over all
// photosites (luma) or the red and the blue (chroma).

#include "dng_opcodes.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace phase_one
{

// What LibRaw knows of the file that the corrections need.
struct Sensor
{
    const uint8_t* file = nullptr;
    size_t size = 0;
    size_t meta = 0, meta_length = 0;              // the corrections' tags (LibRaw's meta_offset, meta_length)
    int raw_w = 0, raw_h = 0;                      // the readout
    int left = 0, top = 0, width = 0, height = 0;  // the active area in it
    int split[2] = {};                             // the column and the row its quarters meet at (ph1.split_col, split_row)
    int black = 0;                                 // ph1.t_black
    float temperature = 0;                         // ph1.tag_210
    // Black offsets (LibRaw's ph1_cblack, ph1_rblack): per row of the
    // readout, left and right of the split column; per column, above and
    // below the split row. Null: none.
    const int16_t (*row_black)[2] = nullptr;
    const int16_t (*column_black)[2] = nullptr;
    int filter[4] = {};  // LibRaw's colour at the active area's (row & 1, col & 1): 0 R, 1 G, 2 B, 3 the other G
    // LibRaw's cubic_spline: the table of the curve through `n` points.
    std::function<void(const int* x, const int* y, int n, uint16_t table[65536])> spline;
};

// The corrections, in the active area's photosites; a summary of what
// they are, and of any not made.
dng::Corrections corrections(const Sensor& s);

}  // namespace phase_one
