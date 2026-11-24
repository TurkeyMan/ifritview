// Headless regression of the real display shaders, including their image
// measurements. --check-volume requires a headless GPU (software Vulkan is fine).
#include "gpu_view.h"
#include "image.h"
#include "half.h"
#include "decoders/icc.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
using V = std::array<float, 3>;
V rgb_of(float L, float a, float b)
{
    const float l = L + .3963377774f * a + .2158037573f * b;
    const float m = L - .1055613458f * a - .0638541728f * b;
    const float n = L - .0894841775f * a - 1.291485548f * b;
    return {4.0767416621f*l*l*l - 3.3077115913f*m*m*m + .2309699292f*n*n*n,
            -1.2684380046f*l*l*l + 2.6097574011f*m*m*m - .3413193965f*n*n*n,
            -.0041960863f*l*l*l - .7034186147f*m*m*m + 1.707614701f*n*n*n};
}
V ok_of(V v)
{
    const float l = std::cbrt(.4122214708f*v[0]+.5363325363f*v[1]+.0514459929f*v[2]);
    const float m = std::cbrt(.2119034982f*v[0]+.6806995451f*v[1]+.1073969566f*v[2]);
    const float n = std::cbrt(.0883024619f*v[0]+.2817188376f*v[1]+.6299787005f*v[2]);
    return {.2104542553f*l+.793617785f*m-.0040720468f*n,
            1.9779984951f*l-2.428592205f*m+.4505937099f*n,
            .0259040371f*l+.7827717662f*m-.808675766f*n};
}
}

int check_volume()
{
    auto device = rap::gpu::device::headless();
    if (!device)
    {
        std::fprintf(stderr, "check-volume: no headless GPU\n");
        return 1;
    }
    // Linear P3 and BT.2020 primaries expressed in BT.709: signed samples
    // must survive decoding, measurement and composition, even below white.
    const float primaries[6][3] = {
        {1.224745f, -0.042058f, -0.019642f}, {-0.224904f, 1.042081f, -0.078655f}, {0, 0, 1.098537f},
        {1.660491f, -0.124550f, -0.018151f}, {-0.587641f, 1.132900f, -0.100579f}, {-0.072850f, -0.008349f, 1.118730f}
    };
    GpuView model_view(device);
    int failures = model_view.check_display_model(), samples = 0;
    float worst = 0;
    // The bypass is based on colorants, not a profile's untrusted label.
    for (const auto& space : {ColorSpace::srgb(), ColorSpace::display_p3(), ColorSpace::rec2020()})
    {
        const auto profile = icc_profile(space);
        if (icc_bt709_primaries(profile.data(), profile.size()) != space.is_srgb())
        {
            std::fprintf(stderr, "check-volume: profile gamut classification failed: %s\n", space.name.c_str());
            ++failures;
        }
    }
    if (icc_bt709_primaries(nullptr, 0))
        ++failures;
    for (int fixture = 0; fixture < 20; ++fixture)
    {
        const int width = fixture == 16 ? 2 : fixture == 17 ? 720 : fixture >= 18 ? 256 : fixture >= 14 ? 4096 : 256;
        const int height = fixture == 17 ? 16 : (fixture == 6 || fixture == 13 || fixture >= 18) ? 32 : 1;
        auto image = std::make_shared<Image>();
        image->width = width, image->height = height;
        image->source = std::make_shared<SourceData>(fixture >= 18 ? SourceData::Sample::U16 : SourceData::Sample::F32,
                                                     3, width, height, fixture != 13 && fixture != 19);
        auto& source = *image->source;
        float hue = 0, edge = 0;
        if (fixture == 19)
        {
            FileMetadata metadata;
            metadata.icc = icc_profile(ColorSpace::srgb());
            image->stored = std::make_shared<const FileMetadata>(std::move(metadata));
        }
        if (fixture >= 7 && fixture <= 12)
        {
            const auto o = ok_of({primaries[fixture - 7][0], primaries[fixture - 7][1], primaries[fixture - 7][2]});
            hue = std::atan2(o[2], o[1]);
            float lo = 0, hi = .6f;
            for (int j = 0; j < 24; ++j)
            {
                const float c = (lo + hi) / 2;
                const auto rgb = rgb_of(.6f, c * std::cos(hue), c * std::sin(hue));
                if (*std::min_element(rgb.begin(), rgb.end()) >= 0 && *std::max_element(rgb.begin(), rgb.end()) <= 1)
                    lo = c;
                else
                    hi = c;
            }
            edge = lo;
        }
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                float rgb[3];
                const float t = (x + 0.0f) / (width - 1);
                for (int c = 0; c < 3; ++c)
                    rgb[c] = (fixture == 6 || fixture == 13 || fixture >= 18) ? ((y / 4) & (1 << c) ? t : 0)
                        : fixture < 6 ? primaries[fixture][c] * t : 0;
                if (fixture >= 7 && fixture <= 12)
                {
                    // Constant Oklab hue/lightness, from just inside to 25%
                    // outside: the region the old u'v' measurement misfit.
                    const float c = edge * (.99f + .26f * x / (width - 1));
                    const auto v = rgb_of(.6f, c * std::cos(hue), c * std::sin(hue));
                    std::copy(v.begin(), v.end(), rgb);
                }
                if (fixture == 14 || fixture == 15)
                {
                    // A single coloured texel on a >2048-wide image: the
                    // former counted level would average it into the grey.
                    for (int c = 0; c < 3; ++c)
                        rgb[c] = x == 2049 ? primaries[5][c] * .6f : .2f;
                }
                if (fixture == 16)
                {
                    // The high-lightness yellow whose fixed-L projection
                    // cannot meet the boundary; include a brighter neutral
                    // so the image's luminance measure retains its span.
                    const V v = x == 0 ? V{1.4f, 1.4f, 1.4f} : V{1.086207f, 1.090669f, .01187434f};
                    std::copy(v.begin(), v.end(), rgb);
                }
                if (fixture == 17)
                {
                    // Bright colours near each hue's cusp, both sides of
                    // the boundary, including lightness beyond white.
                    const float h = x * 6.2831853f / width;
                    float cusp_l = .5f, cusp_c = 0;
                    for (int k = 1; k < 64; ++k)
                    {
                        float lo = 0, hi = .6f;
                        for (int j = 0; j < 20; ++j)
                        {
                            const float c = (lo + hi) * .5f;
                            const V v = rgb_of(k / 64.f, c * std::cos(h), c * std::sin(h));
                            if (*std::min_element(v.begin(), v.end()) >= 0 && *std::max_element(v.begin(), v.end()) <= 1)
                                lo = c;
                            else
                                hi = c;
                        }
                        if (lo > cusp_c)
                            cusp_c = lo, cusp_l = k / 64.f;
                    }
                    const float L = y < 12 ? cusp_l * (.96f + y * .02f) : .98f + (y - 12) * .05f;
                    float lo = 0, hi = .6f;
                    for (int j = 0; j < 24; ++j)
                    {
                        const float c = (lo + hi) * .5f;
                        const V v = rgb_of(std::min(L, .99f), c * std::cos(h), c * std::sin(h));
                        if (*std::min_element(v.begin(), v.end()) >= 0 && *std::max_element(v.begin(), v.end()) <= 1)
                            lo = c;
                        else
                            hi = c;
                    }
                    const float c = lo * (y % 3 == 0 ? .99f : y % 3 == 1 ? 1.01f : 1.4f);
                    const V v = rgb_of(L, c * std::cos(h), c * std::sin(h));
                    std::copy(v.begin(), v.end(), rgb);
                    if (x == 0)
                        rgb[0] = rgb[1] = rgb[2] = 1.4f;
                }
                if (fixture >= 18)
                {
                    uint16_t samples[3];
                    for (int c = 0; c < 3; ++c)
                        samples[c] = uint16_t(std::lround(rgb[c] * 65535));
                    std::memcpy(source.pixel(x, y), samples, sizeof samples);
                }
                else
                    std::memcpy(source.pixel(x, y), rgb, sizeof rgb);
            }
        source.measure_peak();
        GpuView view(device);
        GpuView::Frame frame;
        frame.width = width, frame.height = height;
        frame.front = FrontBuffer::ScRgb16;
        if (fixture == 15)
            frame.detail.sharpness = 100;
        // SDR output peak, with linear readback, independent of the host display.
        std::vector<uint16_t> baseline;
        for (int probe : {4, 1, 2, 0, 1})
        {
            if (probe == 4 && fixture != 6 && fixture != 13 && fixture < 18)
                continue;
            if (probe == 2 && fixture >= 18)
                continue;  // known BT.709 integers deliberately never measure a volume
            frame.volume_probe = probe;
            frame.count_histogram = probe == 0;  // exercise the measured-state transition too
            rap::gpu::texture_ptr out;
            rap::gpu::ticket ready;
            rap::frame_color colour;
            std::string error;
            if (!view.render(image, 1, frame, out, ready, colour, error))
            {
                std::fprintf(stderr, "check-volume: %s\n", error.c_str());
                return 1;
            }
            ready.wait();
            std::vector<uint16_t> pixels(width * height * 4);
            device->readback(out, std::as_writable_bytes(std::span(pixels)), width * 8).wait();
            if (device->lost())
            {
                std::fprintf(stderr, "check-volume: GPU lost during readback\n");
                return 1;
            }
            if (probe == 4)
            {
                baseline = std::move(pixels);
                if (fixture >= 18)
                {
                    // An edit can require a volume even for known SDR
                    // integers. Removing it must restore the exact bypass,
                    // rather than reuse that edit's measured volume.
                    frame.count_histogram = true;
                    if (!view.render(image, 1, frame, out, ready, colour, error))
                        return 1;
                    ready.wait();
                    frame.count_histogram = false;
                    Adjustments adjusted;
                    adjusted.saturation = 100;
                    frame.adjust = build_tables(adjusted);
                    frame.volume_probe = 0;
                    if (!view.render(image, 1, frame, out, ready, colour, error))
                    {
                        std::fprintf(stderr, "check-volume: edit transition: %s\n", error.c_str());
                        return 1;
                    }
                    ready.wait();
                    frame.adjust.reset();
                }
                continue;
            }
            if (fixture >= 7 && fixture <= 12 && probe == 1)
            {
                float last = -1, first = 0;
                for (int x = 0; x < width; ++x)
                {
                    const auto o = ok_of({half_to_float(pixels[x * 4]), half_to_float(pixels[x * 4 + 1]), half_to_float(pixels[x * 4 + 2])});
                    const float c = std::hypot(o[1], o[2]);
                    if (x == 0)
                        first = c;
                    if (c < last - 1e-3f || std::abs(std::remainder(std::atan2(o[2], o[1]) - hue, 6.2831853f)) > .02f)
                        ++failures;
                    last = c;
                }
                if (!(last > first + 1e-4f))
                {
                    std::fprintf(stderr, "check-volume: fixture %d collapsed chroma ramp %.7g..%.7g\n", fixture, first, last);
                    ++failures;
                }
            }
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    for (int c = 0; c < 3; ++c)
                    {
                        const float got = half_to_float(pixels[(y * width + x) * 4 + c]);
                        // Mode 2 writes the signed per-channel overshoot itself:
                        // a tiny >1 error cannot disappear in half-float rounding.
                        const float outside = probe == 2 ? got : std::max(-got, got - 1);
                        worst = std::max(worst, outside);
                        ++samples;
                        if (!std::isfinite(got) || outside > 2e-5f)
                        {
                            if (++failures <= 10)
                                std::fprintf(stderr, "check-volume: fixture %d probe %d x %d channel %d outside: %.7g\n", fixture, probe, x, c, got);
                        }
                        if (probe != 2 && (fixture == 6 || fixture == 13 || fixture >= 18))
                        {
                            float expected = source.value(source.pixel(x, y), c);
                            if (fixture == 13 || fixture == 19)
                                expected = expected <= .04045f ? expected / 12.92f : std::pow((expected + .055f) / 1.055f, 2.4f);
                            // Compare the very same GPU decode/storage, bit
                            // for bit, rather than assuming its float-to-half
                            // rounding agrees with a CPU helper.
                            if (pixels[(y * width + x) * 4 + c] != baseline[(y * width + x) * 4 + c])
                            {
                                if (++failures <= 10)
                                    std::fprintf(stderr, "check-volume: SDR fixture %d x %d ch %d expected %.7g got %.7g\n", fixture, x, c, expected, got);
                            }
                        }
                    }
        }
    }
    std::printf("volume: %s, %d channel samples, %d failures, pre-final/display overshoot %.7g\n", failures ? "FAILED" : "ok", samples, failures, worst);
    return failures ? 1 : 0;
}
