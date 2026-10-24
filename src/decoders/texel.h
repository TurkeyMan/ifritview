#pragma once
// Texel layouts the texture containers (DDS, KTX, VTF) share, decoded one
// way whatever the container.

#include "../image.h"

#include <cstddef>
#include <cstdint>
#include <string>

// More than 8 bits a channel: kept as the image's source data, so 16-bit
// samples aren't lost and floats past 1 get the exposure control.
struct WideTexels
{
    enum class Kind
    {
        Unorm16,
        Snorm16,
        Half,
        Float,
        B10G11R11,  // unsigned 11/11/10-bit floats in a 32-bit word
        E5B9G9R9,   // three 9-bit mantissas with a shared exponent
    };
    Kind kind;
    int channels;  // R, RG, RGB or RGBA; 3 for the packed kinds

    bool packed() const { return kind == Kind::B10G11R11 || kind == Kind::E5B9G9R9; }
    int sample_bytes() const { return packed() || kind == Kind::Float ? 4 : 2; }  // what a big-endian file swaps
    int pixel_bytes() const { return packed() ? 4 : sample_bytes() * channels; }
    bool floating() const { return kind != Kind::Unorm16 && kind != Kind::Snorm16; }
};

// `h` rows of `w` texels from `d` (`size` bytes), each row padded to
// `row_align` bytes, samples big-endian when `big_endian`, colour multiplied
// by alpha when `premultiplied`. Two channels (RG) are red and green, blue
// left empty. False, with `error`, when the data is short.
bool decode_wide(const WideTexels& f, const uint8_t* d, size_t size, int w, int h, size_t row_align, bool big_endian, bool premultiplied,
                 Image& out, std::string& error);
