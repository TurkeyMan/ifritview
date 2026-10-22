#pragma once
// Fewer bits per pixel for the formats that store them so (16-bit and
// palettised BMP and Targa): per-channel reduction and a 256-colour palette,
// each optionally with Floyd–Steinberg error diffusion.

#include <cstdint>
#include <vector>

// Straight RGBA8, row-major. Each channel is cut to its bit count (0 for
// alpha means "drop it, opaque") and written back expanded to 8 bits, so
// the result packs exactly and shows as it will save. Only colour is
// dithered; alpha is rounded, so cut-out edges stay clean.
void reduce_channels(std::vector<uint8_t>& rgba, int width, int height, int rbits, int gbits, int bbits, int abits, bool dither);

// A palette of up to 256 colours (median cut) and an index per pixel.
// Alpha is ignored: palettised BMP and Targa store opaque colours.
struct Palettised
{
    std::vector<uint8_t> palette;  // RGB triples
    std::vector<uint8_t> indices;  // one per pixel
};
Palettised palettise(const std::vector<uint8_t>& rgba, int width, int height, bool dither);
