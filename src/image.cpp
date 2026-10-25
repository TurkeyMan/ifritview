#include "image.h"

#include "i18n.h"

#include <stdexcept>

#include "half.h"

#include <glib.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace
{

inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    if (a != 255)
    {
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

inline uint32_t pack_premultiplied(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

}  // namespace

void Image::allocate(int w, int h)
{
    if (!fits(w, h))
        throw std::length_error("the image is too large (" + std::to_string(w) + " × " + std::to_string(h) + ")");
    width = w;
    height = h;
    stride = w * 4;
    pixels.resize(0);
    pixels.resize(size_t(stride) * h);  // not zeroed: every decoder writes every pixel
}

void Image::from_rgb8(const uint8_t* src, int src_stride)
{
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* s = src + size_t(y) * src_stride;
        uint32_t* d = row(y);
        for (int x = 0; x < width; ++x, s += 3)
            d[x] = pack_premultiplied(s[0], s[1], s[2], 255);
    }
}

void Image::from_bgrx8(const uint8_t* src, int src_stride)
{
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* s = src + size_t(y) * src_stride;
        uint32_t* d = row(y);
        for (int x = 0; x < width; ++x, s += 4)
            d[x] = pack_premultiplied(s[2], s[1], s[0], 255);
    }
}

void Image::from_rgba8(const uint8_t* src, int src_stride, bool premultiplied)
{
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* s = src + size_t(y) * src_stride;
        uint32_t* d = row(y);
        if (premultiplied)
            for (int x = 0; x < width; ++x, s += 4)
                d[x] = pack_premultiplied(s[0], s[1], s[2], s[3]);
        else
            for (int x = 0; x < width; ++x, s += 4)
                d[x] = pack(s[0], s[1], s[2], s[3]);
    }
}

void Image::from_bgra8(const uint8_t* src, int src_stride, bool premultiplied)
{
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* s = src + size_t(y) * src_stride;
        uint32_t* d = row(y);
        if (premultiplied)
            for (int x = 0; x < width; ++x, s += 4)
                d[x] = pack_premultiplied(s[2], s[1], s[0], s[3]);
        else
            for (int x = 0; x < width; ++x, s += 4)
                d[x] = pack(s[2], s[1], s[0], s[3]);
    }
}

void Image::from_rgb16(const uint16_t* src, int src_stride)
{
    auto deep = std::make_shared<SourceData>(SourceData::Sample::U16, 3, width, height, false);
    for (int y = 0; y < height; ++y)
        std::memcpy(deep->pixel(0, y), reinterpret_cast<const uint8_t*>(src) + size_t(y) * src_stride, size_t(width) * 6);
    set_source(std::move(deep));
}

void Image::from_rgba16(const uint16_t* src, int src_stride)
{
    auto deep = std::make_shared<SourceData>(SourceData::Sample::U16, 4, width, height, false);
    for (int y = 0; y < height; ++y)
        std::memcpy(deep->pixel(0, y), reinterpret_cast<const uint8_t*>(src) + size_t(y) * src_stride, size_t(width) * 8);
    set_source(std::move(deep));
}

SourceData::SourceData(Sample s, int c, int w, int h, bool lin)
    : sample(s), channels(c), width(w), height(h), linear(lin)
{
    data.resize(size_t(w) * h * pixel_bytes());
}

float SourceData::value(const uint8_t* px, int c) const
{
    const uint8_t* s = px + size_t(c) * sample_bytes();
    switch (sample)
    {
        case Sample::U16:
        {
            uint16_t v;
            std::memcpy(&v, s, 2);
            return v / 65535.0f;
        }
        case Sample::I16:
        {
            int16_t v;
            std::memcpy(&v, s, 2);
            return std::max(-1.0f, v / 32767.0f);
        }
        case Sample::F16:
        {
            uint16_t v;
            std::memcpy(&v, s, 2);
            return half_to_float(v);
        }
        case Sample::F32:
        {
            float v;
            std::memcpy(&v, s, 4);
            return v;
        }
    }
    return 0;
}

void SourceData::measure_peak()
{
    peak = 0;
    const int colours = channels >= 3 ? 3 : 1;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < colours; ++c)
            {
                const float v = value(pixel(x, y), c);
                if (v > peak && v <= 3.4e38f)  // not infinity (nor NaN)
                    peak = v;
            }
}

void Image::set_source(std::shared_ptr<SourceData> src)
{
    if (width != src->width || height != src->height)
        allocate(src->width, src->height);
    if (src->linear)
    {
        src->measure_peak();
        if (src->peak > 1.0f)
        {
            char range[64];
            std::snprintf(range, sizeof range, "%.3g× white (%+.1f EV)", double(src->peak), std::log2(double(src->peak)));
            add("Image", "Brightest", range);
        }
    }
    has_alpha = src->has_alpha();
    source = std::move(src);
    pixels = render_source(0);
    mips.clear();
}

namespace
{

inline double srgb_encode(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
}

}  // namespace

void Image::retone(double ev)
{
    if (!linear && !(source && source->linear))
        return;
    if (ev != 0)
        break_transform(_("the exposure was changed"));
    pixels = retone_buffer(ev);
    mips.clear();
}

// Repeated 2x2 averages of premultiplied ARGB, down to about 1024 px on the
// long side. Cheap (a third of one pass over the image) and done on the
// loader thread, so displaying a big image never filters the full buffer.
void Image::build_mips()
{
    mips.clear();
    if (pixels.size() < size_t(width) * height * 4)
        return;  // none yet (a raw the GPU develops)
    const uint8_t* src = pixels.data();
    int w = width, h = height;
    while (std::max(w, h) > 1024 && w >= 2 && h >= 2)
    {
        const int nw = w / 2, nh = h / 2;
        Mip m{nw, nh, PixelBuffer(size_t(nw) * nh * 4)};
        for (int y = 0; y < nh; ++y)
        {
            const uint8_t* r0 = src + size_t(2 * y) * w * 4;
            const uint8_t* r1 = r0 + size_t(w) * 4;
            uint8_t* d = m.pixels.data() + size_t(y) * nw * 4;
            for (int x = 0; x < nw; ++x, d += 4, r0 += 8, r1 += 8)
                for (int c = 0; c < 4; ++c)
                    d[c] = uint8_t((r0[c] + r0[c + 4] + r1[c] + r1[c + 4] + 2) >> 2);
        }
        mips.push_back(std::move(m));
        src = mips.back().pixels.data();
        w = nw;
        h = nh;
    }
}

// The source's samples to 8-bit premultiplied pixels. Linear ones take the
// exposure, are clipped and get the sRGB curve, through a table over every
// half float (the output has 8 bits; a float is looked up by its nearest
// half once exposed, so values past a half's range still come down); the
// rest are as they're encoded, signed ones with 0 at mid-grey.
PixelBuffer Image::render_source(double ev) const
{
    PixelBuffer out(size_t(width) * height * 4);
    const SourceData& s = *source;
    if (s.width != width || s.height != height)
        return out;
    std::vector<uint8_t> colour, alpha;
    const bool halves = s.sample == SourceData::Sample::F16;
    const float exposure = float(std::pow(2.0, ev));
    if (s.linear)
    {
        const float table_exposure = halves ? exposure : 1.0f;  // floats are exposed before the lookup
        colour.resize(65536);
        alpha.resize(65536);
        for (uint32_t i = 0; i < 65536; ++i)
        {
            const float v = half_to_float(uint16_t(i));
            const bool number = v == v;  // NaN shows black
            colour[i] = number ? uint8_t(std::lround(srgb_encode(v * table_exposure) * 255.0)) : 0;
            alpha[i] = number ? uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)) : 0;
        }
    }
    const bool is_signed = s.sample == SourceData::Sample::I16;
    const auto encoded = [&](const uint8_t* px, int c, bool is_alpha) -> uint32_t
    {
        if (s.linear)
        {
            uint16_t h;
            if (halves)
                std::memcpy(&h, px + size_t(c) * 2, 2);
            else
                h = float_to_half(is_alpha ? s.value(px, c) : s.value(px, c) * exposure);
            return is_alpha ? alpha[h] : colour[h];
        }
        float v = s.value(px, c);
        if (is_signed && !is_alpha)
            v = (v + 1) / 2;
        return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    for (int y = 0; y < height; ++y)
    {
        uint32_t* d = reinterpret_cast<uint32_t*>(out.data() + size_t(y) * stride);
        for (int x = 0; x < width; ++x)
        {
            const uint8_t* px = s.pixel(x, y);
            uint32_t r, g, b, a = 255;
            if (s.channels >= 3)
            {
                r = encoded(px, 0, false);
                g = encoded(px, 1, false);
                b = encoded(px, 2, false);
            }
            else
                r = g = b = encoded(px, 0, false);
            if (s.has_alpha())
                a = encoded(px, s.channels - 1, true);
            d[x] = pack(r, g, b, a);
        }
    }
    return out;
}

PixelBuffer Image::retone_buffer(double ev) const
{
    PixelBuffer out(size_t(width) * height * 4);  // the pixels themselves may have been let go (pixels_stale)
    const bool from_source = !linear && source && source->linear;
    if ((!linear && !from_source) || out.empty())
        return out;
    auto row_of = [&](int y) { return reinterpret_cast<uint32_t*>(out.data() + size_t(y) * stride); };
    const double exposure = std::pow(2.0, ev);
    if (from_source)
        return render_source(ev);
    if (linear->tone.empty())
    {
        // 16-bit linear -> gain -> clip -> sRGB transfer -> 8-bit, via one table.
        std::vector<uint8_t> lut(65536);
        const double gain = linear->auto_gain * exposure;
        for (int i = 0; i < 65536; ++i)
            lut[i] = uint8_t(std::lround(srgb_encode(i / 65535.0 * gain) * 255.0));
        const uint16_t* s = linear->rgb.data();
        for (int y = 0; y < height; ++y)
        {
            uint32_t* d = row_of(y);
            for (int x = 0; x < width; ++x, s += 3)
                d[x] = pack_premultiplied(lut[s[0]], lut[s[1]], lut[s[2]], 255);
        }
        return out;
    }
    // Camera-matched curve: it maps scene luminance to display luminance;
    // the pixel's colour is scaled by the same ratio so hue is kept.
    std::vector<uint8_t> encode(4096);
    for (int i = 0; i < 4096; ++i)
        encode[i] = uint8_t(std::lround(srgb_encode(i / 4095.0) * 255.0));
    const std::vector<float>& tone = linear->tone;
    const uint16_t* s = linear->rgb.data();
    const float scale = float(exposure / 65535.0);
    for (int y = 0; y < height; ++y)
    {
        uint32_t* d = row_of(y);
        for (int x = 0; x < width; ++x, s += 3)
        {
            const float r = s[0] * scale, g = s[1] * scale, b = s[2] * scale;
            const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            const float o = tone[std::min(4095, int(lum * 4095.0f + 0.5f))];
            const float ratio = lum > 1e-6f ? o / lum : 0.0f;
            auto enc = [&](float v) { return encode[std::clamp(int(v * ratio * 4095.0f + 0.5f), 0, 4095)]; };
            d[x] = pack_premultiplied(enc(r), enc(g), enc(b), 255);
        }
    }
    return out;
}

namespace
{
constexpr char kPartMark = '\x1f';  // ASCII unit separator: in no real path
}

std::string part_key(const std::string& path, ImagePart part)
{
    if (part == ImagePart{})
        return path;
    return path + kPartMark + std::to_string(part.page) + "." + std::to_string(part.level);
}

std::string key_file(const std::string& key)
{
    const size_t mark = key.find(kPartMark);
    return mark == std::string::npos ? key : key.substr(0, mark);
}

ImagePart key_part(const std::string& key)
{
    ImagePart part;
    const size_t mark = key.find(kPartMark);
    if (mark != std::string::npos)
        std::sscanf(key.c_str() + mark + 1, "%d.%d", &part.page, &part.level);
    return part;
}

std::string key_text(const std::string& key)
{
    if (key.find(kPartMark) == std::string::npos)
        return key;
    const ImagePart part = key_part(key);
    return key_file(key) + " [" + std::to_string(part.page) + "." + std::to_string(part.level) + "]";
}

std::string Image::location_text() const
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.5f° %c, %.5f° %c", std::fabs(latitude), latitude < 0 ? 'S' : 'N', std::fabs(longitude), longitude < 0 ? 'W' : 'E');
    return buf;
}

void Image::add(const std::string& group, std::string key, std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    if (key.empty() || value.empty())
        return;
    for (MetadataGroup& g : metadata)
        if (g.name == group)
        {
            g.items.push_back({std::move(key), std::move(value)});
            return;
        }
    metadata.push_back({group, {{std::move(key), std::move(value)}}});
}

Image Image::crop(int x, int y, int w, int h) const
{
    x = std::clamp(x, 0, width);
    y = std::clamp(y, 0, height);
    w = std::clamp(w, 0, width - x);
    h = std::clamp(h, 0, height - y);
    Image out;
    out.transform = transform;
    if (out.transform.width == 0)
        out.transform = {width, height, 1, 0, 0, width, height, transform.broken};
    out.transform.x += x;  // within the crop before
    out.transform.y += y;
    out.transform.w = w;
    out.transform.h = h;
    if (mosaic)
    {
        // A raw the GPU develops crops as it develops: the crop joins the
        // camera's framing, in the data's terms (before the turn).
        auto m = std::make_shared<RawMosaic>(*mosaic);
        int fx = x, fy = y, fw = w, fh = h;
        orient_rect(inverse_orientation(m->orientation), width, height, fx, fy, fw, fh);
        m->crop[0] += fx, m->crop[1] += fy, m->crop[2] = fw, m->crop[3] = fh;
        out.mosaic = std::move(m);
    }
    if (pixels_stale)
    {
        // None yet (the GPU's develop makes them): only its size.
        out.width = w, out.height = h, out.stride = w * 4;
        out.pixels_stale = true;
    }
    else
        out.allocate(w, h);
    out.has_alpha = has_alpha;
    out.bits_per_sample = bits_per_sample;
    out.format = format;
    out.pixel_format = pixel_format;
    out.color_space = color_space;
    out.declared_space = declared_space;
    out.info = info;
    out.encoded_as = encoded_as;  // still the file's pixels: lossless JPEG crops start from it
    out.encoding = encoding;
    out.metadata = metadata;
    out.exposure_bias = exposure_bias;
    out.is_raw = is_raw;
    out.xmp = xmp;
    out.stage = stage;
    out.exposure_edit = exposure_edit;
    out.pending_edits = pending_edits;
    if (is_preview() && width > 0 && height > 0)
        out.pending_edits.push_back({1, double(x) / width, double(y) / height, double(w) / width, double(h) / height});
    if (final_width > 0 && width > 0 && height > 0)
    {
        out.final_width = std::max(1, int(std::lround(double(final_width) * w / width)));
        out.final_height = std::max(1, int(std::lround(double(final_height) * h / height)));
    }
    out.has_location = has_location;
    out.latitude = latitude;
    out.longitude = longitude;
    out.place = place;
    out.stored = stored;
    out.edited = true;
    if (!pixels_stale)
        for (int yy = 0; yy < h; ++yy)
            std::copy_n(row(y + yy) + x, w, out.row(yy));
    if (linear)
    {
        auto l = std::make_shared<LinearData>();
        l->auto_gain = linear->auto_gain;
        l->tone = linear->tone;
        l->rgb.resize(size_t(w) * h * 3);
        for (int yy = 0; yy < h; ++yy)
            std::copy_n(linear->rgb.data() + (size_t(y + yy) * width + x) * 3, size_t(w) * 3, l->rgb.data() + size_t(yy) * w * 3);
        out.linear = std::move(l);
    }
    if (source)
    {
        auto deep = std::make_shared<SourceData>(source->sample, source->channels, w, h, source->linear);
        for (int yy = 0; yy < h; ++yy)
            std::memcpy(deep->pixel(0, yy), source->pixel(x, y + yy), size_t(w) * source->pixel_bytes());
        deep->peak = source->peak;  // still the same image's range, whatever part is kept
        out.source = std::move(deep);
    }
    return out;
}

namespace
{

// Maps destination (x, y) to the source pixel for an EXIF orientation.
template <typename T>
void reorient(const T* src, int W, int H, int channels, int o, T* dst, int dw, int dh)
{
    for (int y = 0; y < dh; ++y)
    {
        T* d = dst + size_t(y) * dw * channels;
        for (int x = 0; x < dw; ++x)
        {
            int sx, sy;
            switch (o)
            {
                case 2:
                    sx = W - 1 - x;
                    sy = y;
                    break;  // mirror horizontal
                case 3:
                    sx = W - 1 - x;
                    sy = H - 1 - y;
                    break;  // rotate 180
                case 4:
                    sx = x;
                    sy = H - 1 - y;
                    break;  // mirror vertical
                case 5:
                    sx = y;
                    sy = x;
                    break;  // transpose
                case 6:
                    sx = y;
                    sy = H - 1 - x;
                    break;  // rotate 90 clockwise
                case 7:
                    sx = W - 1 - y;
                    sy = H - 1 - x;
                    break;  // transverse
                default:
                    sx = W - 1 - y;
                    sy = x;
                    break;  // 8: rotate 90 counter-clockwise
            }
            const T* sp = src + (size_t(sy) * W + sx) * channels;
            for (int c = 0; c < channels; ++c)
                d[x * channels + c] = sp[c];
        }
    }
}

}  // namespace

void Image::adopt(Image&& f)
{
    for (const PendingEdit& e : pending_edits)
    {
        if (e.orientation != 1)
            f.apply_orientation(e.orientation);
        else if (e.w > 0 && e.h > 0)
            f = f.crop(int(e.x * f.width), int(e.y * f.height), int(e.w * f.width), int(e.h * f.height));
    }
    pending_edits.clear();
    ++generation;  // new data in place: what the GPU kept of the old is stale
    width = f.width;
    height = f.height;
    stride = f.stride;
    pixels = std::move(f.pixels);
    has_alpha = f.has_alpha;
    bits_per_sample = f.bits_per_sample;
    format = std::move(f.format);
    pixel_format = f.pixel_format;
    if (!f.color_space.empty())
        color_space = std::move(f.color_space);
    if (!f.info.empty())
        info = std::move(f.info);
    linear = std::move(f.linear);
    source = std::move(f.source);
    mosaic = std::move(f.mosaic);
    pixels_stale = f.pixels_stale;
    if (f.stored)
        stored = std::move(f.stored);
    mips = std::move(f.mips);  // empty if pending edits rewrote f's pixels
    animation = std::move(f.animation);
    if (f.exposure_bias != 0)
        exposure_bias = f.exposure_bias;
    if (f.has_location)
    {
        has_location = true;
        latitude = f.latitude;
        longitude = f.longitude;
    }
    // The final decode knows things the preview didn't (Camera, Raw...);
    // groups it brings replace ours of the same name, the rest stay.
    for (MetadataGroup& g : f.metadata)
    {
        bool replaced = false;
        for (MetadataGroup& mine : metadata)
            if (mine.name == g.name)
            {
                mine = std::move(g);
                replaced = true;
                break;
            }
        if (!replaced)
            metadata.push_back(std::move(g));
    }
    is_raw = is_raw || f.is_raw;
    is_video = is_video || f.is_video;
    has_audio = has_audio || f.has_audio;
    final_width = final_height = 0;
    stage = Stage::Final;
    if (tonable() && exposure_edit != 0)
        retone(exposure_edit);
}

namespace
{

// An orientation (EXIF 1..8) as the matrix it applies to pixel positions,
// about the image's centre, with y down.
struct Matrix
{
    int a, b, c, d;  // x' = a x + b y, y' = c x + d y
    bool operator==(const Matrix&) const = default;
};

Matrix matrix_of(int o)
{
    switch (o)
    {
        case 2: return {-1, 0, 0, 1};   // mirror horizontal
        case 3: return {-1, 0, 0, -1};  // rotate 180
        case 4: return {1, 0, 0, -1};   // mirror vertical
        case 5: return {0, 1, 1, 0};    // transpose
        case 6: return {0, -1, 1, 0};   // rotate 90 clockwise
        case 7: return {0, -1, -1, 0};  // transverse
        case 8: return {0, 1, -1, 0};   // rotate 90 counter-clockwise
        default: return {1, 0, 0, 1};
    }
}

int orientation_of(const Matrix& m)
{
    for (int o = 1; o <= 8; ++o)
        if (matrix_of(o) == m)
            return o;
    return 1;
}

}  // namespace

// What undoes `o`: its matrix transposed (they're all rotations and flips).
int inverse_orientation(int o)
{
    const Matrix m = matrix_of(o);
    return orientation_of({m.a, m.c, m.b, m.d});
}

// `after` done to an image already turned by `before`.
int compose_orientation(int before, int after)
{
    const Matrix p = matrix_of(after), q = matrix_of(before);
    return orientation_of({p.a * q.a + p.b * q.c, p.a * q.b + p.b * q.d, p.c * q.a + p.d * q.c, p.c * q.b + p.d * q.d});
}

// A rectangle in a W × H image, where it lands when the image is turned by `o`.
void orient_rect(int o, int W, int H, int& x, int& y, int& w, int& h)
{
    const int x0 = x, y0 = y, w0 = w, h0 = h;
    switch (o)
    {
        case 2: x = W - x0 - w0; break;
        case 3: x = W - x0 - w0, y = H - y0 - h0; break;
        case 4: y = H - y0 - h0; break;
        case 5: x = y0, y = x0, w = h0, h = w0; break;
        case 6: x = H - y0 - h0, y = x0, w = h0, h = w0; break;
        case 7: x = H - y0 - h0, y = W - x0 - w0, w = h0, h = w0; break;
        case 8: x = y0, y = W - x0 - w0, w = h0, h = w0; break;
        default: break;
    }
}

void Image::apply_camera_orientation(int o)
{
    const bool was_edited = edited;
    const Transform was = transform;
    const auto pending = pending_edits;
    apply_orientation(o);
    edited = was_edited;
    transform = was;
    pending_edits = pending;
}

void Image::apply_orientation(int o)
{
    if (o < 2 || o > 8)
        return;
    if (is_preview())
        pending_edits.push_back({o, 0, 0, 0, 0});
    mips.clear();
    vector.reset();  // its renderings would be the right way up
    if (mosaic)
    {
        // A raw the GPU develops turns as it develops: the turn joins the
        // camera's, and the pixels (if any) are made again when asked for.
        auto m = std::make_shared<RawMosaic>(*mosaic);
        m->orientation = compose_orientation(m->orientation, o);
        mosaic = std::move(m);
        PixelBuffer().swap(pixels);
        pixels_stale = true;
    }
    edited = true;
    // The crop turns with the image; the turn joins the one before.
    Transform& t = transform;
    if (t.width == 0)
        t = {width, height, 1, 0, 0, width, height, t.broken};
    const bool turned = matrix_of(t.orientation).a == 0;  // the turned image's size, before this
    orient_rect(o, turned ? t.height : t.width, turned ? t.width : t.height, t.x, t.y, t.w, t.h);
    t.orientation = compose_orientation(t.orientation, o);
    const int W = width, H = height;
    const bool transposed = o >= 5;
    const int dw = transposed ? H : W, dh = transposed ? W : H;
    if (pixels.size() >= size_t(W) * H * 4)  // none while a raw waits for the GPU (pixels_stale)
    {
        PixelBuffer out(size_t(dw) * dh * 4);
        reorient(reinterpret_cast<const uint32_t*>(pixels.data()), W, H, 1, o, reinterpret_cast<uint32_t*>(out.data()), dw, dh);
        pixels = std::move(out);
    }
    if (linear)
    {
        auto l = std::make_shared<LinearData>();
        l->auto_gain = linear->auto_gain;
        l->tone = linear->tone;
        l->rgb.resize(linear->rgb.size());
        reorient(linear->rgb.data(), W, H, 3, o, l->rgb.data(), dw, dh);
        linear = std::move(l);
    }
    if (source)
    {
        auto deep = std::make_shared<SourceData>(*source);
        deep->width = dw;
        deep->height = dh;
        reorient(source->data.data(), W, H, int(source->pixel_bytes()), o, deep->data.data(), dw, dh);
        source = std::move(deep);
    }
    width = dw;
    height = dh;
    stride = dw * 4;
    if (transposed)
        std::swap(final_width, final_height);
}
