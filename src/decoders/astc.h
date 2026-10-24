#pragma once
// Adaptive Scalable Texture Compression: 128-bit blocks over a footprint of
// 4 × 4 to 12 × 12 pixels, LDR or HDR, for the formats that carry it (.astc,
// KTX, KTX2...). 3D footprints aren't covered.

#include "../image.h"

#include <cstddef>
#include <cstdint>
#include <string>

enum class AstcProfile
{
    Auto,  // HDR if any block uses HDR endpoints, LDR otherwise (KTX 1, .astc)
    LDR,
    sRGB,  // LDR with sRGB endpoint expansion, as the _SRGB formats decode
    HDR,   // shown clipped to the displayable range
};

// The footprints the format defines: 4x4, 5x4, 5x5 ... 12x10, 12x12.
bool astc_footprint_valid(int block_w, int block_h);
// The bytes of a w × h surface: whole blocks, partial ones rounded up.
size_t astc_size(int block_w, int block_h, int w, int h);
// What the blocks hold, for Image::pixel_format: "RGBA 8 bpc, ASTC 6x6
// block compression" (float16 when HDR).
PixelFormat astc_pixel_format(int block_w, int block_h, bool hdr);

// Decodes a w × h surface into `out` (allocated here), premultiplied as
// Image keeps it. `premultiplied`: the colour was stored multiplied by alpha
// already. Blocks that fail to decode show magenta, the format's error
// colour. Sets has_alpha when any pixel isn't opaque, and bits_per_sample
// (16 for HDR). False, with `error`, when `size` is short of the surface.
bool decode_astc(const uint8_t* data, size_t size, int block_w, int block_h, int w, int h, AstcProfile profile, Image& out,
                 std::string& error, bool premultiplied = false);
