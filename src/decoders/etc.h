#pragma once
// Ericsson Texture Compression, the mobile GPUs' block formats: ETC1, ETC2
// (RGB, RGB with punch-through alpha, RGBA with an EAC alpha block) and EAC
// R11/RG11 (one and two 11-bit channels, unsigned or signed), for the
// formats that carry them (PKM, KTX, KTX2...).

#include "../image.h"

#include <cstddef>
#include <cstdint>
#include <string>

enum class Etc
{
    ETC1,
    ETC2_RGB,
    ETC2_RGBA1,  // punch-through: each pixel opaque or transparent
    ETC2_RGBA,   // ETC2 colour with an EAC alpha block
    EAC_R11,     // one channel, shown grey
    EAC_R11S,
    EAC_RG11,  // two channels (normal maps), shown as red and green
    EAC_RG11S,
};

const char* etc_name(Etc format);  // "ETC2 RGBA"
size_t etc_block_bytes(Etc format);
// The bytes of a w × h surface: whole 4 × 4 blocks, partial ones rounded up.
size_t etc_size(Etc format, int w, int h);
// What the blocks hold, for Image::pixel_format: "RGB 8 bpc, 4 bpp, ETC1
// block compression".
PixelFormat etc_pixel_format(Etc format);

// Decodes a w × h surface into `out` (allocated here), premultiplied as
// Image keeps it. `premultiplied`: the colour was stored multiplied by
// alpha already. Sets has_alpha when any pixel isn't opaque. False, with
// `error`, when `size` is short of the surface.
bool decode_etc(Etc format, const uint8_t* data, size_t size, int w, int h, Image& out, std::string& error,
                bool premultiplied = false);
