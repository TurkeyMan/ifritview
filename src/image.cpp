#include "image.h"

#include <glib.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
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

namespace {

inline double srgb_encode(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
}

}  // namespace

void Image::retone(double ev) {
    if (!linear) return;
    const double exposure = std::pow(2.0, ev);
    if (linear->tone.empty()) {
        // 16-bit linear -> gain -> clip -> sRGB transfer -> 8-bit, via one table.
        static std::vector<uint8_t> lut(65536);
        const double gain = linear->auto_gain * exposure;
        for (int i = 0; i < 65536; ++i) lut[i] = uint8_t(std::lround(srgb_encode(i / 65535.0 * gain) * 255.0));
        const uint16_t* s = linear->rgb.data();
        for (int y = 0; y < height; ++y) {
            uint32_t* d = row(y);
            for (int x = 0; x < width; ++x, s += 3) d[x] = pack_premultiplied(lut[s[0]], lut[s[1]], lut[s[2]], 255);
        }
        return;
    }
    // Camera-matched curve: it maps scene luminance to display luminance;
    // the pixel's colour is scaled by the same ratio so hue is kept.
    static std::vector<uint8_t> encode(4096);
    for (int i = 0; i < 4096; ++i) encode[i] = uint8_t(std::lround(srgb_encode(i / 4095.0) * 255.0));
    const std::vector<float>& tone = linear->tone;
    const uint16_t* s = linear->rgb.data();
    const float scale = float(exposure / 65535.0);
    for (int y = 0; y < height; ++y) {
        uint32_t* d = row(y);
        for (int x = 0; x < width; ++x, s += 3) {
            const float r = s[0] * scale, g = s[1] * scale, b = s[2] * scale;
            const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            const float out = tone[std::min(4095, int(lum * 4095.0f + 0.5f))];
            const float ratio = lum > 1e-6f ? out / lum : 0.0f;
            auto enc = [&](float v) { return encode[std::clamp(int(v * ratio * 4095.0f + 0.5f), 0, 4095)]; };
            d[x] = pack_premultiplied(enc(r), enc(g), enc(b), 255);
        }
    }
}

std::string Image::location_text() const {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.5f° %c, %.5f° %c", std::fabs(latitude), latitude < 0 ? 'S' : 'N',
                  std::fabs(longitude), longitude < 0 ? 'W' : 'E');
    return buf;
}

void Image::add(const std::string& group, std::string key, std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    if (key.empty() || value.empty()) return;
    for (MetadataGroup& g : metadata)
        if (g.name == group) {
            g.items.push_back({std::move(key), std::move(value)});
            return;
        }
    metadata.push_back({group, {{std::move(key), std::move(value)}}});
}

Image Image::crop(int x, int y, int w, int h) const {
    x = std::clamp(x, 0, width);
    y = std::clamp(y, 0, height);
    w = std::clamp(w, 0, width - x);
    h = std::clamp(h, 0, height - y);
    Image out;
    out.allocate(w, h);
    out.has_alpha = has_alpha;
    out.bits_per_sample = bits_per_sample;
    out.format = format;
    out.info = info;
    out.metadata = metadata;
    out.exposure_bias = exposure_bias;
    out.xmp = xmp;
    if (final_width > 0 && width > 0 && height > 0) {
        out.final_width = std::max(1, int(std::lround(double(final_width) * w / width)));
        out.final_height = std::max(1, int(std::lround(double(final_height) * h / height)));
    }
    out.has_location = has_location;
    out.latitude = latitude;
    out.longitude = longitude;
    out.place = place;
    for (int yy = 0; yy < h; ++yy) std::copy_n(row(y + yy) + x, w, out.row(yy));
    if (linear) {
        auto l = std::make_shared<LinearData>();
        l->auto_gain = linear->auto_gain;
        l->tone = linear->tone;
        l->rgb.resize(size_t(w) * h * 3);
        for (int yy = 0; yy < h; ++yy)
            std::copy_n(linear->rgb.data() + (size_t(y + yy) * width + x) * 3, size_t(w) * 3, l->rgb.data() + size_t(yy) * w * 3);
        out.linear = std::move(l);
    }
    return out;
}

namespace {

// Maps destination (x, y) to the source pixel for an EXIF orientation.
template <typename T>
void reorient(const T* src, int W, int H, int channels, int o, T* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        T* d = dst + size_t(y) * dw * channels;
        for (int x = 0; x < dw; ++x) {
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
            const T* sp = src + (size_t(sy) * W + sx) * channels;
            for (int c = 0; c < channels; ++c) d[x * channels + c] = sp[c];
        }
    }
}

}  // namespace

void Image::apply_orientation(int o) {
    if (o < 2 || o > 8) return;
    if (g_getenv("IFRIT_TRACE")) std::fprintf(stderr, "apply_orientation(%d) on %dx%d\n", o, width, height);
    const int W = width, H = height;
    const bool transposed = o >= 5;
    const int dw = transposed ? H : W, dh = transposed ? W : H;
    std::vector<uint8_t> out(size_t(dw) * dh * 4);
    reorient(reinterpret_cast<const uint32_t*>(pixels.data()), W, H, 1, o, reinterpret_cast<uint32_t*>(out.data()), dw, dh);
    pixels = std::move(out);
    if (linear) {
        auto l = std::make_shared<LinearData>();
        l->auto_gain = linear->auto_gain;
        l->tone = linear->tone;
        l->rgb.resize(linear->rgb.size());
        reorient(linear->rgb.data(), W, H, 3, o, l->rgb.data(), dw, dh);
        linear = std::move(l);
    }
    width = dw;
    height = dh;
    stride = dw * 4;
    if (transposed) std::swap(final_width, final_height);
}
