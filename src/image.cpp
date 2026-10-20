#include "image.h"

#include <utility>

namespace {

inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (a != 255) {
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

inline uint32_t pack_premultiplied(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

}  // namespace

void Image::allocate(int w, int h) {
    width = w;
    height = h;
    stride = w * 4;
    pixels.assign(size_t(stride) * h, 0);
}

void Image::from_rgb8(const uint8_t* src, int src_stride) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + size_t(y) * src_stride;
        uint32_t* d = row(y);
        for (int x = 0; x < width; ++x, s += 3) d[x] = pack_premultiplied(s[0], s[1], s[2], 255);
    }
}

void Image::from_bgrx8(const uint8_t* src, int src_stride) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + size_t(y) * src_stride;
        uint32_t* d = row(y);
        for (int x = 0; x < width; ++x, s += 4) d[x] = pack_premultiplied(s[2], s[1], s[0], 255);
    }
}

void Image::from_rgba8(const uint8_t* src, int src_stride, bool premultiplied) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + size_t(y) * src_stride;
        uint32_t* d = row(y);
        if (premultiplied)
            for (int x = 0; x < width; ++x, s += 4) d[x] = pack_premultiplied(s[0], s[1], s[2], s[3]);
        else
            for (int x = 0; x < width; ++x, s += 4) d[x] = pack(s[0], s[1], s[2], s[3]);
    }
}

void Image::from_bgra8(const uint8_t* src, int src_stride, bool premultiplied) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + size_t(y) * src_stride;
        uint32_t* d = row(y);
        if (premultiplied)
            for (int x = 0; x < width; ++x, s += 4) d[x] = pack_premultiplied(s[2], s[1], s[0], s[3]);
        else
            for (int x = 0; x < width; ++x, s += 4) d[x] = pack(s[2], s[1], s[0], s[3]);
    }
}

void Image::from_rgb16(const uint16_t* src, int src_stride) {
    for (int y = 0; y < height; ++y) {
        const uint16_t* s = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(src) + size_t(y) * src_stride);
        uint32_t* d = row(y);
        for (int x = 0; x < width; ++x, s += 3) d[x] = pack_premultiplied(s[0] >> 8, s[1] >> 8, s[2] >> 8, 255);
    }
}

void Image::from_rgba16(const uint16_t* src, int src_stride) {
    for (int y = 0; y < height; ++y) {
        const uint16_t* s = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(src) + size_t(y) * src_stride);
        uint32_t* d = row(y);
        for (int x = 0; x < width; ++x, s += 4) d[x] = pack(s[0] >> 8, s[1] >> 8, s[2] >> 8, s[3] >> 8);
    }
}

void Image::apply_orientation(int o) {
    if (o < 2 || o > 8) return;
    const int W = width, H = height;
    const bool transposed = o >= 5;
    Image dst;
    dst.allocate(transposed ? H : W, transposed ? W : H);
    for (int y = 0; y < dst.height; ++y) {
        uint32_t* d = dst.row(y);
        for (int x = 0; x < dst.width; ++x) {
            int sx, sy;
            switch (o) {
                case 2: sx = W - 1 - x; sy = y; break;              // mirror horizontal
                case 3: sx = W - 1 - x; sy = H - 1 - y; break;      // rotate 180
                case 4: sx = x; sy = H - 1 - y; break;              // mirror vertical
                case 5: sx = y; sy = x; break;                      // transpose
                case 6: sx = y; sy = H - 1 - x; break;              // rotate 90 clockwise
                case 7: sx = W - 1 - y; sy = H - 1 - x; break;      // transverse
                default: sx = W - 1 - y; sy = x; break;             // 8: rotate 90 counter-clockwise
            }
            d[x] = row(sy)[sx];
        }
    }
    width = dst.width;
    height = dst.height;
    stride = dst.stride;
    pixels = std::move(dst.pixels);
}
