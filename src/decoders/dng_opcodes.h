#pragma once
// A DNG's own corrections for its lens and sensor (its opcode lists),
// for the develop to apply as the DNG SDK does: the lens shading phones
// store as gain maps over each colour of the mosaic (OpcodeList2), and the
// vignetting and per-colour gain maps applied once it has three colours
// (OpcodeList3). LibRaw applies none of them. What's here is what the GPU
// reads (develop.hlsl's `corrections`); geometry (WarpRectilinear) isn't
// among them yet.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dng
{

// develop.hlsl's layout, 32-bit words:
//   0  gain maps over the mosaic (OpcodeList2)
//   1  gain maps over R, G, B (OpcodeList3)
//   2  1: a radial vignette (OpcodeList3's FixVignetteRadial)
//   4..11  its k0..k4, centre x, y (photosites) and 1 / the farthest corner's distance squared
//   16 on: each map's 16 words (top, left, bottom, right, plane, planes, row pitch, column
//      pitch, points down, across, planes in the map, where its gains start; spacing
//      down, across, origin down, across as floats), list 2's first, then the gains
struct Corrections
{
    std::vector<uint32_t> words;
    std::string summary;  // what was found, for the trace and the info panel
    bool empty() const { return words.empty(); }
};

// The opcode lists of the raw image `width` x `height` (as stored) in the
// DNG `file`, in the coordinates of its active area (`active_w` x
// `active_h`); empty when there are none it knows how to apply.
Corrections corrections(const uint8_t* file, size_t size, int width, int height, int active_w, int active_h);

}  // namespace dng
