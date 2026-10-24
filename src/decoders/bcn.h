#pragma once
// Block-compressed GPU textures (S3TC/DXTC and their successors): BC1-BC3
// (DXT1-5), BC4/BC5 (ATI1/ATI2, one and two channels), BC6H (HDR) and BC7,
// for the formats that carry them (DDS, VTF, BLP...).

#include "../image.h"

#include <cstddef>
#include <cstdint>
#include <string>

enum class Bcn
{
    BC1,  // DXT1, with its 1-bit alpha
    BC2,  // DXT3: explicit 4-bit alpha
    BC3,  // DXT5: interpolated alpha
    BC4,  // ATI1: one channel, shown grey
    BC4S,
    BC5,  // ATI2: two channels (normal maps), shown as red and green
    BC5S,
    BC6H,  // HDR, shown clipped to the displayable range
    BC6HS,
    BC7,
};

const char* bcn_name(Bcn format);  // "BC1 (DXT1)"
size_t bcn_block_bytes(Bcn format);
// The bytes of a w × h surface: whole 4 × 4 blocks, partial ones rounded up.
size_t bcn_size(Bcn format, int w, int h);

// Decodes a w × h surface into `out` (allocated here), premultiplied as
// Image keeps it. `premultiplied`: the colour was stored multiplied by
// alpha already (DXT2, DXT4). Sets has_alpha when any pixel isn't opaque.
// False, with `error`, when `size` is short of the surface.
bool decode_bcn(Bcn format, const uint8_t* data, size_t size, int w, int h, Image& out, std::string& error,
                bool premultiplied = false);
