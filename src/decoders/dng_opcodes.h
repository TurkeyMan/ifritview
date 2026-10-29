#pragma once
// A raw's own corrections for its lens and sensor, for the develop to
// apply: a DNG's opcode lists, as the DNG SDK applies them (the lens
// shading phones store as gain maps over each colour of the mosaic,
// OpcodeList2; the vignetting and per-colour gain maps applied once it has
// three colours, OpcodeList3), and a Phase One's corrections of its
// readout (its black per row and column, its curves, defects and flat
// fields, phase_one.h). LibRaw applies none of a DNG's. What's here is
// what the GPU reads (develop.hlsl's `corrections`).

#include "../bytes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace dng
{

// develop.hlsl's layout, 32-bit words:
//   0  gain maps over the mosaic (OpcodeList2, a Phase One's flat fields)
//   1  gain maps over R, G, B (OpcodeList3)
//   2  1: a radial vignette (OpcodeList3's FixVignetteRadial); 2: lensfun's (pa), divided out
//   3  1: the readout's own corrections (words 40..47)
//   4..11  its k0..k4, centre x, y (photosites) and 1 / the farthest corner's distance squared
//      (lensfun's: k1..k3, and the scale to its coordinates, squared)
//   12 the warp's model: 0 WarpRectilinear, 1 lensfun's (lens_database.h)
//   16 planes WarpRectilinear corrects (0: none, 1: all alike, 3: each its own)
//   17..19 its centre x, y (photosites) and the farthest corner's distance
//   20..37 each plane's kr0..kr3, kt0, kt1
//      (lensfun's: 19 the scale to its coordinates, 20 1 / the autoscale,
//      21..24 the distortion as powers of r, 25..27 and 28..30 red's and
//      blue's TCA: v, c, b)
//   40..47 the readout's (Readout's; 0 where it has none): where each row's
//      black offsets start, where each column's, the split column and row,
//      where the curves start, where each row's defects start, where the
//      bad columns' bits start, its black
//   48 on: each map's 16 words (top, left, bottom, right, plane, planes, row pitch, column
//      pitch, points down, across, planes in the map, where its gains start; spacing
//      down, across, origin down, across as floats), list 2's first, then the gains,
//      then the readout's
struct Corrections
{
    std::vector<uint32_t> words;
    std::string summary;  // what was found, for the trace and the info panel
    bool empty() const { return words.empty(); }
    bool warps() const { return words.size() > 16 && words[16] != 0; }
};

// A float as a word holds it.
inline uint32_t bits(float f)
{
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

// A gain map, as the develop interpolates it (develop.hlsl's map_gain):
// over the photosites of `area` (top, left, bottom, right, in the active
// area; bottom and right past it), every pitch-th row and column from its
// corner, the planes from `plane` on; its points `spacing` apart from
// `origin`, in photosites from the area's corner (its first photosite's
// centre at 0.5), down then across, `map_planes` gains a point;
// bilinear between them, held at the edges.
struct GainMap
{
    uint32_t area[4] = {};
    uint32_t plane = 0, planes = 1;
    uint32_t pitch[2] = {1, 1};   // rows, columns
    uint32_t points[2] = {};      // down, across
    uint32_t map_planes = 1;
    float spacing[2] = {}, origin[2] = {};  // down, across
    std::vector<float> gains;
};

// A sensor's corrections of its readout, made before the gain maps over the
// mosaic, at each photosite of the active area: its black (beyond the
// mosaic's, which stays on), less offsets
// per row (left and right of the split column) and per column (above and
// below the split row), held at 0; curves, each quarter its own (split as
// the offsets are); and defects, a photosite the rounded mean of its
// neighbours'. After the gain maps, bad columns estimated from those
// around (develop.hlsl's corrected()).
struct Readout
{
    int black = 0;
    std::vector<int16_t> row_black, column_black;  // two a row, two a column; or none
    int split[2] = {};                              // the column, the row
    std::vector<uint16_t> curves;                   // 65536 a quarter: above left, above right, below left, below right; or none
    std::vector<std::vector<uint16_t>> defects;     // each row's defects' columns; or none
    std::vector<uint8_t> bad_columns;               // a column's: 1 bad, 3 bad and on its own (none of the others within 4); or none
};

// `c`'s words (its first 48, or none yet) with the gain maps and the
// readout's corrections after them.
void lay_out(Corrections& c, const std::vector<GainMap>& mosaic, const std::vector<GainMap>& rgb, const Readout* readout = nullptr);

// `own`'s words (a raw's corrections of its sensor, or none) with lensfun's
// `lens` (lenses::corrections: words 2 and 4..37 alone, or none) in place
// of any it had; none at all when that leaves nothing.
std::vector<uint32_t> with_lens(std::vector<uint32_t> own, const std::vector<uint32_t>& lens);

// Each opcode of the opcode list at `list`, `size` bytes (big-endian
// whatever the file's order): f(its id, flags, parameters and their size),
// until f says to stop (false) or the list ends.
template <typename F>
void each_opcode(const uint8_t* list, size_t size, F f)
{
    const uint32_t count = size >= 4 ? be32(list) : 0;
    for (size_t i = 0, at = 4; i < count && at + 16 <= size; ++i)
    {
        const uint32_t id = be32(list + at), flags = be32(list + at + 8), bytes = be32(list + at + 12);
        if (bytes > size - at - 16 || !f(id, flags, list + at + 16, size_t(bytes)))
            return;
        at += 16 + bytes;
    }
}

// The opcode lists of the raw image `width` x `height` (as stored) in the
// DNG `file`, in the coordinates of its active area (`active_w` x
// `active_h`); empty when there are none it knows how to apply.
Corrections corrections(const uint8_t* file, size_t size, int width, int height, int active_w, int active_h);

}  // namespace dng
