#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A decoded image in cairo's ARGB32 layout: one native-endian uint32 per
// pixel, alpha premultiplied, so the buffer can be wrapped by a cairo image
// surface without a copy.
struct Image {
    int width = 0;
    int height = 0;
    int stride = 0;              // bytes per row, always width * 4
    std::vector<uint8_t> pixels;
    bool has_alpha = false;
    int bits_per_sample = 8;     // depth of the source file, for the status bar
    std::string format;          // "JPEG", "Canon RAW", ...
    std::string info;            // extra detail for the status bar, may be empty

    void allocate(int w, int h);
    uint32_t* row(int y) { return reinterpret_cast<uint32_t*>(pixels.data() + size_t(y) * stride); }
    const uint32_t* row(int y) const { return reinterpret_cast<const uint32_t*>(pixels.data() + size_t(y) * stride); }

    // Converters from common decoder outputs into the ARGB32 buffer. The
    // image must already be allocated to the source dimensions. src_stride is
    // in bytes. The source may alias the image's own buffer when it has four
    // bytes per pixel.
    void from_rgb8(const uint8_t* src, int src_stride);
    void from_bgrx8(const uint8_t* src, int src_stride);
    void from_rgba8(const uint8_t* src, int src_stride, bool premultiplied = false);
    void from_bgra8(const uint8_t* src, int src_stride, bool premultiplied = false);
    void from_rgb16(const uint16_t* src, int src_stride);
    void from_rgba16(const uint16_t* src, int src_stride);

    // Rotate/flip in place per an EXIF orientation value 1..8.
    void apply_orientation(int exif_orientation);
};
