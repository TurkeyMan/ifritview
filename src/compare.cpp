#include "compare.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

ImageDifference compare_images(const Image& a, const Image& b)
{
    ImageDifference d;
    if (a.width != b.width || a.height != b.height || a.width <= 0 || a.height <= 0)
        return d;
    d.width = a.width;
    d.height = a.height;
    d.errors.resize(size_t(a.width) * a.height);
    double sum = 0, squares = 0;
    size_t changed = 0;
    uint8_t* out = d.errors.data();
    for (int y = 0; y < a.height; ++y)
    {
        const uint8_t* pa = a.pixels.data() + size_t(y) * a.stride;
        const uint8_t* pb = b.pixels.data() + size_t(y) * b.stride;
        for (int x = 0; x < a.width; ++x, pa += 4, pb += 4)
        {
            uint32_t ua, ub;
            std::memcpy(&ua, pa, 4);
            std::memcpy(&ub, pb, 4);
            int worst = 0;
            for (int shift = 0; shift < 32; shift += 8)
            {
                const int e = std::abs(int(ua >> shift & 255) - int(ub >> shift & 255));
                worst = std::max(worst, e);
                sum += e;
                if (shift < 24)  // colour, not alpha
                    squares += double(e) * e;
            }
            d.max_error = std::max(d.max_error, worst);
            changed += worst > 2;
            *out++ = uint8_t(worst);
        }
    }
    const double pixels = double(a.width) * a.height;
    d.mean_error = sum / (pixels * 4);
    const double mse = squares / (pixels * 3);
    d.psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : std::numeric_limits<double>::infinity();
    d.changed = changed / pixels;
    return d;
}

std::shared_ptr<Image> difference_image(const Image& a, const ImageDifference& d, int gain)
{
    if (d.width != a.width || d.height != a.height || d.errors.empty())
        return nullptr;
    // An amplified error (0..255) to a colour: black, red, yellow, white.
    std::array<uint32_t, 256> heat;
    for (int e = 0; e < 256; ++e)
    {
        const int v = std::min(255, e * gain);
        const int r = std::min(255, v * 3), g = std::clamp(v * 3 - 255, 0, 255), b = std::clamp(v * 3 - 510, 0, 255);
        heat[size_t(e)] = uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
    }
    auto out = std::make_shared<Image>();
    out->width = a.width;
    out->height = a.height;
    out->stride = a.width * 4;
    out->pixels.resize(size_t(out->stride) * out->height);
    out->format = "Difference";
    const uint8_t* err = d.errors.data();
    for (int y = 0; y < a.height; ++y)
    {
        const uint8_t* pa = a.pixels.data() + size_t(y) * a.stride;
        uint32_t* row = reinterpret_cast<uint32_t*>(out->pixels.data() + size_t(y) * out->stride);
        for (int x = 0; x < a.width; ++x, pa += 4)
        {
            uint32_t ua;
            std::memcpy(&ua, pa, 4);
            // The original's luminance, dimmed, so the error sits on the picture.
            const uint32_t grey = (((ua >> 16 & 255) * 54 + (ua >> 8 & 255) * 183 + (ua & 255) * 19) >> 8) / 4;
            const uint32_t h = heat[*err++];
            const auto add = [grey](uint32_t c) { return std::min<uint32_t>(255, c + grey); };
            row[x] = 0xff000000u | add(h >> 16 & 255) << 16 | add(h >> 8 & 255) << 8 | add(h & 255);
        }
    }
    out->build_mips();
    return out;
}
