// GIMP's XCF: the layer stack composited into one image, as GIMP's
// flattening would. Every version (64-bit pointers from 11), tiles stored
// raw, RLE or zlib, every precision, RGB, grey and indexed. Layers with
// their offsets, visibility, opacity and masks; groups composited on their
// own, then as a layer. Normal is the only mode applied; layer effects
// (GIMP 3's filters), channels, paths and the selection are left out.
// The format: GIMP's devel-docs/xcf.txt.
#include "decoder.h"
#include "icc.h"
#include "../bytes.h"
#include "../half.h"
#include "../i18n.h"
#include "../parallel.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 14 && std::memcmp(h, "gimp xcf ", 9) == 0;
}

// Bounds-checked big-endian reads: past the end reads 0 and marks the file bad.
struct Reader
{
    const uint8_t* d = nullptr;
    size_t n = 0;
    bool wide = false;  // 64-bit pointers (version 11 on)
    bool bad = false;
    bool has(uint64_t at, uint64_t len) const { return at <= n && len <= n - at; }
    uint32_t u32(uint64_t& at)
    {
        if (!has(at, 4))
            return bad = true, 0;
        at += 4;
        return be32(d + at - 4);
    }
    uint64_t ptr(uint64_t& at)
    {
        if (!wide)
            return u32(at);
        const uint64_t hi = u32(at);
        return hi << 32 | u32(at);
    }
    void skip_string(uint64_t& at)
    {
        const uint32_t len = u32(at);
        if (!has(at, len))
            bad = true;
        at += len;
    }
};

// How the samples are stored: bytes each, integer or float, linear or
// perceptual (sRGB's curve; GIMP's "non-linear" and "perceptual" alike).
struct Precision
{
    int bytes = 1;
    bool is_float = false, linear = false;
    std::string name() const
    {
        return std::to_string(bytes * 8) + "-bit " + (is_float ? "float" : "integer") + (linear ? ", linear" : ", perceptual");
    }
};

bool precision(int version, uint32_t p, Precision& out)
{
    if (version < 4)
        return true;  // 8-bit perceptual
    if (version == 4)
    {
        static const Precision v4[] = {{1, false, false}, {2, false, false}, {4, false, true}, {2, true, true}, {4, true, true}};
        return p < 5 && (out = v4[p], true);
    }
    if (version <= 6 && p >= 400 && p <= 550)  // their halves and floats are numbered a step lower
        p += 100;
    const int kind = int(p / 100), curve = int(p % 100);
    static const int bytes[] = {0, 1, 2, 4, 0, 2, 4, 8};
    if (kind < 1 || kind > 7 || !bytes[kind] || (curve != 0 && curve != 50 && curve != 75))
        return false;
    out = {bytes[kind], kind >= 5, curve == 0};
    return true;
}

float srgb_decode(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
float srgb_encode(float v)
{
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
}

// Linear light to perceptual integers, 0..`top`: rounded as encoding
// exactly would, by where each step's midpoint lies in linear light.
struct Encoder
{
    std::vector<float> steps;
    explicit Encoder(int top) : steps(top)
    {
        for (int k = 0; k < top; ++k)
            steps[k] = srgb_decode((k + 0.5f) / top);
    }
    uint32_t operator()(float v) const { return uint32_t(std::upper_bound(steps.begin(), steps.end(), v) - steps.begin()); }
};

struct Layer
{
    uint32_t width = 0, height = 0, type = 0;
    int x = 0, y = 0;
    float opacity = 1;
    bool visible = true, group = false, apply_mask = true, floating = false, perceptual = false;
    bool other_mode = false, effects = false;  // what isn't applied: a mode or compositing option, layer effects
    uint32_t mode = 0;
    // Its composite space and mode (PROP_COMPOSITE_SPACE, _MODE): 1..4 as
    // GIMP numbers them, "auto" (stored negated) as what it stood for; 0: not
    // given; -1: a value GIMP doesn't have.
    int space = 0, composite = 0;
    int depth = 0;  // how deep in groups (the length of its item path, less one)
    uint64_t pixels = 0, mask = 0;
    std::vector<size_t> children;  // groups: indices into the layer list, topmost first
};

// An area of straight (not premultiplied) linear RGBA, at (x, y) in its parent's.
struct Canvas
{
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<float> px;
};

struct Xcf
{
    Reader r;
    int version = 0, base = 0, compression = 0;
    Precision prec;
    uint32_t colormap[256] = {};
    int colours = 0;
    std::vector<Layer> layers;
    bool other_modes = false, effects = false;
    std::vector<float> to_linear;  // a perceptual integer sample's linear value, for 8 and 16 bits

    // A drawable's tiles (a layer's, or a mask's: one channel), 64 x 64
    // pixels each, row by row; where each one starts in the file.
    struct Tiles
    {
        uint32_t w = 0, h = 0, across = 0;
        int bpp = 0;
        std::vector<uint64_t> starts;  // and a 0 after the last
        size_t count() const { return starts.empty() ? 0 : starts.size() - 1; }
        int64_t x(size_t i) const { return int64_t(i % across) * 64; }
        int64_t y(size_t i) const { return int64_t(i / across) * 64; }
        int tw(size_t i) const { return int(std::min<int64_t>(64, w - x(i))); }
        int th(size_t i) const { return int(std::min<int64_t>(64, h - y(i))); }
    };
    bool open(uint64_t at, uint32_t w, uint32_t h, int bpp, Tiles& t)
    {
        if (r.u32(at) != w || r.u32(at) != h || r.u32(at) != uint32_t(bpp))
            return false;
        uint64_t level = r.ptr(at);
        if (r.u32(level) != w || r.u32(level) != h || r.bad)
            return false;
        t = {w, h, (w + 63) / 64, bpp, {}};
        const uint64_t count = uint64_t(t.across) * ((h + 63) / 64);
        if (!r.has(level, (count + 1) * (r.wide ? 8 : 4)))  // the table is in the file, which bounds its size
            return false;
        t.starts.resize(count + 1);
        for (uint64_t& start : t.starts)
            start = r.ptr(level);
        return true;
    }
    // Tile i's samples as stored, decompressed.
    bool read(const Tiles& t, size_t i, uint8_t* out) const
    {
        const uint64_t start = t.starts[i];
        // Tiles follow each other: the next one's start ends this one.
        const uint64_t end = t.starts[i + 1] > start ? std::min<uint64_t>(t.starts[i + 1], r.n) : r.n;
        return start && start < end && unpack(r.d + start, size_t(end - start), out, t.tw(i) * t.th(i), t.bpp);
    }

    bool unpack(const uint8_t* s, size_t n, uint8_t* out, int pixels, int bpp) const
    {
        const size_t size = size_t(pixels) * bpp;
        if (compression == 0)
            return n >= size && (std::memcpy(out, s, size), true);
        if (compression == 2)
        {
            uLongf len = uLongf(size);
            return uncompress(out, &len, s, uLong(n)) == Z_OK && len == size;
        }
        if (compression != 1)
            return false;
        // RLE: each byte of the pixels a stream of its own.
        size_t at = 0;
        for (int k = 0; k < bpp; ++k)
            for (int i = 0; i < pixels;)
            {
                if (at >= n)
                    return false;
                const int op = s[at++];
                int count;
                bool run = op < 128;
                if (op == 127 || op == 128)
                {
                    if (at + 2 > n)
                        return false;
                    count = s[at] << 8 | s[at + 1];
                    at += 2;
                }
                else
                    count = op < 127 ? op + 1 : 256 - op;
                if (count > pixels - i || at + (run ? 1 : count) > n)
                    return false;
                for (int j = 0; j < count; ++j, ++i)
                    out[size_t(i) * bpp + k] = s[run ? at : at + j];
                at += run ? 1 : count;
            }
        return true;
    }

    // One stored sample, 0..1 for integers.
    float sample(const uint8_t* s) const
    {
        if (!prec.is_float)
        {
            if (prec.bytes == 1)
                return s[0] / 255.0f;
            if (prec.bytes == 2)
                return be16(s) / 65535.0f;
            return float(be32(s) / 4294967295.0);
        }
        if (prec.bytes == 2)
            return half_to_float(be16(s));
        if (prec.bytes == 4)
        {
            const uint32_t v = be32(s);
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }
        const uint64_t v = be64(s);
        double f;
        std::memcpy(&f, &v, 8);
        return float(f);
    }
    float colour(const uint8_t* s) const
    {
        if (prec.linear)
            return sample(s);
        if (!prec.is_float && prec.bytes <= 2)
            return to_linear[prec.bytes == 1 ? s[0] : be16(s)];
        return srgb_decode(sample(s));
    }

    bool props(uint64_t& at, Layer* layer)
    {
        for (int guard = 0; guard < 100000; ++guard)
        {
            const uint32_t type = r.u32(at), len = r.u32(at);
            if (r.bad)
                return false;
            if (type == 0)
                return true;
            uint64_t p = at;
            uint64_t next = at + len;
            if (type == 1 && !layer)  // the colour map; some GIMPs wrote its length wrong
            {
                const uint32_t count = std::min(r.u32(p), 256u);
                colours = int(count);
                next = p + uint64_t(count) * 3;
                if (!r.has(p, uint64_t(count) * 3))
                    return false;
                for (uint32_t i = 0; i < count; ++i)
                    colormap[i] = uint32_t(r.d[p + i * 3]) << 16 | uint32_t(r.d[p + i * 3 + 1]) << 8 | r.d[p + i * 3 + 2];
            }
            else if (!r.has(at, len))
                return false;
            else if (type == 17 && !layer && len >= 1)
                compression = r.d[at];
            else if (type == 21 && !layer)
                parasites(at, at + len);
            else if (layer && len >= 4)
            {
                const uint32_t v = r.u32(p);
                switch (type)
                {
                    case 5: layer->floating = true; break;
                    case 6: layer->opacity = std::min(v, 255u) / 255.0f; break;
                    case 7: layer->mode = v; break;
                    case 8: layer->visible = v != 0; break;
                    case 11: layer->apply_mask = v != 0; break;
                    case 15: layer->x = std::clamp(int32_t(v), -(1 << 30), 1 << 30), layer->y = std::clamp(int32_t(r.u32(p)), -(1 << 30), 1 << 30); break;
                    case 30: layer->depth = int(std::min<uint32_t>(len / 4, 64)) - 1; break;
                    case 33:
                    {
                        float f;
                        std::memcpy(&f, &v, 4);
                        layer->opacity = std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 1;
                        break;
                    }
                    case 35: layer->composite = setting(v); break;
                    case 36: layer->space = setting(v); break;
                }
            }
            else if (layer && type == 29)
                layer->group = true;
            at = next;
        }
        return false;
    }

    // A composite space or mode as stored: 1..4, or negated for "auto".
    static int setting(uint32_t v)
    {
        for (int k = 1; k <= 4; ++k)
            if (v == uint32_t(k) || v == uint32_t(-k))
                return k;
        return -1;
    }

    std::vector<uint8_t> icc;
    void parasites(uint64_t at, uint64_t end)
    {
        while (at < end && !r.bad)
        {
            const uint32_t name_len = r.u32(at);
            if (!r.has(at, name_len))
                return void(r.bad = true);
            const std::string name(reinterpret_cast<const char*>(r.d + at), name_len ? name_len - 1 : 0);
            at += name_len;
            r.u32(at);  // flags
            const uint32_t len = r.u32(at);
            if (!r.has(at, len))
                return void(r.bad = true);
            if (name == "icc-profile")
                icc.assign(r.d + at, r.d + at + len);
            at += len;
        }
    }

    bool layer(uint64_t at, Layer& l)
    {
        l.width = r.u32(at), l.height = r.u32(at), l.type = r.u32(at);
        r.skip_string(at);
        if (!props(at, &l))
            return false;
        // Composited in linear light unless it says otherwise, or (saying
        // nothing) is in a legacy mode (before 23): those were perceptual.
        l.perceptual = l.space > 0 ? l.space == 2 || l.space == 4 : l.mode < 23;
        // Only Normal (legacy or not; pass-through groups as Normal), over
        // the backdrop as a union, in RGB: anything else is done that way
        // too, and said to be (once it's composited).
        l.other_mode = (l.mode != 0 && l.mode != 28 && l.mode != 61) || (l.composite != 0 && l.composite != 1) || l.space == 3 || l.space < 0;
        l.pixels = r.ptr(at);
        l.mask = r.ptr(at);
        l.effects = version >= 20 && r.ptr(at) != 0;
        return !r.bad && l.width && l.height && l.width <= (1u << 20) && l.height <= (1u << 20);
    }

    // A layer's mask, applied: its tiles, which line up with the layer's.
    bool mask(const Layer& l, Tiles& t)
    {
        if (!l.mask || !l.apply_mask)
            return false;
        uint64_t at = l.mask;
        if (r.u32(at) != l.width || r.u32(at) != l.height)
            return false;
        r.skip_string(at);
        Layer ignored;
        return props(at, &ignored) && open(r.ptr(at), l.width, l.height, prec.bytes, t);
    }

    // Normal mode, union: a straight linear RGBA run over the canvas's.
    static void blend(float* d, const float* s, const float* m, int n, float opacity, bool perceptual)
    {
        for (int i = 0; i < n; ++i, d += 4, s += 4)
        {
            const float a = s[3] * opacity * (m ? m[i] : 1.0f);
            if (!(a > 0))
                continue;
            const float b = d[3], out = a + b * (1 - a), k = a / out;
            for (int c = 0; c < 3; ++c)
                d[c] = perceptual ? srgb_decode(srgb_encode(d[c]) + (srgb_encode(s[c]) - srgb_encode(d[c])) * k) : d[c] + (s[c] - d[c]) * k;
            d[3] = out;
        }
    }

    // Composites layer `i` (with its children, a group) onto `dst`.
    bool composite(size_t i, Canvas& dst, int depth)
    {
        const Layer& l = layers[i];
        if (!l.visible || l.floating || l.opacity <= 0)
            return true;
        // The part of dst it covers, in the layer (a group's canvas is only that).
        const int64_t x0 = int64_t(l.x) - dst.x, y0 = int64_t(l.y) - dst.y;
        const int64_t left = std::max<int64_t>(0, -x0), top = std::max<int64_t>(0, -y0);
        const int64_t right = std::min<int64_t>(l.width, dst.w - x0), bottom = std::min<int64_t>(l.height, dst.h - y0);
        if (right <= left || bottom <= top)
            return true;
        other_modes |= l.other_mode;
        effects |= l.effects;
        Tiles m;
        const bool masked = mask(l, m);
        auto visible = [&](const Tiles& t, size_t i)
        { return t.x(i) < right && t.x(i) + t.tw(i) > left && t.y(i) < bottom && t.y(i) + t.th(i) > top; };
        // Tile i's area of the layer onto dst, through the mask's tile i:
        // its pixels from `px`, straight linear RGBA, the layer's (px_x, px_y)
        // first, `stride` floats a row.
        auto put = [&](size_t i, int64_t x, int64_t y, int w, int h, const float* px, int64_t px_x, int64_t px_y, size_t stride)
        {
            float mv[64 * 64];
            if (masked)
            {
                uint8_t stored[64 * 64 * 8];
                if (m.tw(i) != w || m.th(i) != h || !read(m, i, stored))
                    return false;
                for (int k = 0; k < w * h; ++k)
                    mv[k] = std::clamp(sample(stored + k * prec.bytes), 0.0f, 1.0f);
            }
            const int64_t a = std::max(x, left), b = std::min(x + w, right);
            for (int64_t ly = std::max(y, top); ly < std::min(y + h, bottom) && b > a; ++ly)
                blend(&dst.px[(size_t(y0 + ly) * dst.w + size_t(x0 + a)) * 4], px + size_t(ly - px_y) * stride + size_t(a - px_x) * 4,
                      masked ? mv + (ly - y) * w + (a - x) : nullptr, int(b - a), l.opacity, l.perceptual);
            return true;
        };
        std::atomic<bool> ok = true, short_of_memory = false;
        if (l.group)
        {
            if (depth > 64)
                return false;
            Canvas g{int(l.x + left), int(l.y + top), int(right - left), int(bottom - top), {}};
            g.px.assign(size_t(g.w) * g.h * 4, 0.0f);
            for (size_t k = l.children.size(); k-- > 0;)
                if (!composite(l.children[k], g, depth + 1))
                    return false;
            if (!masked)
                return put(0, left, top, g.w, g.h, g.px.data(), left, top, size_t(g.w) * 4);
            parallel(m.count(),
                     [&](size_t i)
                     {
                         if (visible(m, i) && !put(i, m.x(i), m.y(i), m.tw(i), m.th(i), g.px.data(), left, top, size_t(g.w) * 4))
                             ok = false;
                     });
            return ok;
        }
        const bool alpha = l.type & 1, grey = l.type == 2 || l.type == 3, indexed = l.type >= 4;
        if (l.type > 5 || indexed != (base == 2) || grey != (base == 1))
            return false;
        const int channels = (indexed || grey ? 1 : 3) + alpha, bytes = indexed ? 1 : prec.bytes;
        Tiles t;
        if (!open(l.pixels, l.width, l.height, channels * bytes, t) || (masked && m.count() != t.count()))
            return false;
        parallel(t.count(),
                 [&](size_t i)
                 {
                     if (!visible(t, i))
                         return;
                     const int w = t.tw(i), h = t.th(i);
                     // On threads of their own: an exception mustn't leave one.
                     std::vector<uint8_t> stored;
                     std::vector<float> rgba;
                     try
                     {
                         stored.resize(size_t(w) * h * t.bpp);
                         rgba.resize(size_t(w) * h * 4);
                     }
                     catch (const std::bad_alloc&)
                     {
                         short_of_memory = true;
                         return void(ok = false);
                     }
                     if (!read(t, i, stored.data()))
                         return void(ok = false);
                     const uint8_t* s = stored.data();
                     for (float* p = rgba.data(); p < rgba.data() + rgba.size(); p += 4, s += t.bpp)
                     {
                         if (indexed)
                         {
                             const uint32_t c = colormap[s[0]];
                             p[0] = to_linear[c >> 16 & 255], p[1] = to_linear[c >> 8 & 255], p[2] = to_linear[c & 255];
                             p[3] = alpha ? (s[1] >= 128 ? 1.0f : 0.0f) : 1.0f;  // GIMP: indexed alpha is on or off
                             continue;
                         }
                         for (int c = 0; c < 3; ++c)
                         {
                             const float v = colour(s + (grey ? 0 : c * bytes));
                             p[c] = std::isfinite(v) ? v : 0.0f;
                         }
                         p[3] = alpha ? std::clamp(sample(s + (channels - 1) * bytes), 0.0f, 1.0f) : 1.0f;
                     }
                     if (!put(i, t.x(i), t.y(i), w, h, rgba.data(), t.x(i), t.y(i), size_t(w) * 4))
                         ok = false;
                 });
        if (short_of_memory)
            throw std::bad_alloc();  // here, where decode() catches it
        return ok;
    }
};

bool decode_xcf(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* data;
    size_t size;
    if (!file_bytes(path, data, size) || !sniff(data, size))
        return error = "not an XCF file", false;
    Xcf x;
    x.r.d = data, x.r.n = size;
    if (std::memcmp(data + 9, "file", 4) == 0)
        x.version = 0;
    else if (data[9] == 'v' && data[13] == 0 && std::isdigit(data[10]) && std::isdigit(data[11]) && std::isdigit(data[12]))
        x.version = (data[10] - '0') * 100 + (data[11] - '0') * 10 + (data[12] - '0');
    else
        return error = "unknown XCF version", false;
    x.r.wide = x.version >= 11;
    uint64_t at = 14;
    const uint32_t w = x.r.u32(at), h = x.r.u32(at);
    x.base = int(x.r.u32(at));
    const uint32_t p = x.version >= 4 ? x.r.u32(at) : 0;
    if (x.r.bad || !Image::fits(w, h) || x.base > 2)
        return error = "bad XCF header", false;
    if (!precision(x.version, p, x.prec))
        return error = "unsupported XCF precision " + std::to_string(p), false;
    if (x.base == 2)
        x.prec = {};  // indexed: 8-bit perceptual always
    if (!x.props(at, nullptr) || x.compression > 2)
        return error = x.compression > 2 ? "unsupported XCF compression" : "bad XCF image properties", false;
    // Perceptual integers (and the colour map's entries) to linear by table.
    x.to_linear.resize(x.prec.bytes == 2 ? 65536 : 256);
    for (size_t i = 0; i < x.to_linear.size(); ++i)
        x.to_linear[i] = srgb_decode(float(i) / float(x.to_linear.size() - 1));
    std::vector<uint64_t> pointers;
    for (uint64_t ptr; (ptr = x.r.ptr(at)) != 0 && !x.r.bad && pointers.size() < 100000;)
        pointers.push_back(ptr);
    for (uint64_t ptr : pointers)
    {
        Layer l;
        if (!x.layer(ptr, l))
            return error = "bad XCF layer", false;
        x.layers.push_back(std::move(l));
    }
    // The list is flat, topmost first, a group's layers straight after it.
    std::vector<size_t> top;
    {
        std::vector<size_t> open;  // the groups we're in
        for (size_t i = 0; i < x.layers.size(); ++i)
        {
            const int depth = std::min<int>(x.layers[i].depth, int(open.size()));
            open.resize(depth);
            (open.empty() ? top : x.layers[open.back()].children).push_back(i);
            if (x.layers[i].group)
                open.push_back(i);
        }
    }
    Canvas canvas{0, 0, int(w), int(h), std::vector<float>(size_t(w) * h * 4, 0.0f)};
    for (size_t k = top.size(); k-- > 0;)
        if (!x.composite(top[k], canvas, 0))
            return error = "bad XCF layer data", false;

    bool opaque = true;
    for (size_t i = 3; i < canvas.px.size() && opaque; i += 4)
        opaque = canvas.px[i] >= 1;
    const bool grey = x.base == 1;
    const int channels = (grey ? 1 : 3) + !opaque;
    if (x.prec.bytes == 1)
    {
        out.allocate(int(w), int(h));
        std::vector<uint8_t> rgba(canvas.px.size());
        const Encoder encode(255);
        parallel(h,
                 [&](size_t y)
                 {
                     for (size_t i = y * w * 4; i < (y + 1) * w * 4; ++i)
                         rgba[i] = uint8_t(i % 4 == 3 ? std::lround(canvas.px[i] * 255) : encode(canvas.px[i]));
                 });
        out.from_rgba8(rgba.data(), int(w) * 4);
        out.has_alpha = !opaque;
    }
    else
    {
        // Deeper: perceptual integers as 16 bits, the rest as linear floats.
        const bool linear = x.prec.linear || x.prec.is_float;
        auto src = std::make_shared<SourceData>(linear ? SourceData::Sample::F32 : SourceData::Sample::U16, channels, int(w), int(h), linear);
        const Encoder encode(linear ? 1 : 65535);
        parallel(h,
                 [&](size_t y)
                 {
                     for (uint32_t i = 0; i < w; ++i)
                     {
                         const float* s = &canvas.px[(y * w + i) * 4];
                         uint8_t* d = src->pixel(int(i), int(y));
                         for (int c = 0; c < channels; ++c)
                         {
                             const bool a = c == channels - 1 && !opaque;
                             const float v = a ? s[3] : s[c];
                             if (linear)
                                 std::memcpy(d + c * 4, &v, 4);
                             else
                             {
                                 const uint16_t u = uint16_t(a ? std::lround(v * 65535) : encode(v));
                                 std::memcpy(d + c * 2, &u, 2);
                             }
                         }
                     }
                 });
        out.set_source(std::move(src));
    }

    out.format = "XCF";
    out.bits_per_sample = x.prec.bytes * 8;
    using M = PixelFormat::Model;
    if (x.base == 2)
        out.pixel_format = PixelFormat::palette(8, x.colours);
    else
    {
        out.pixel_format = PixelFormat::packed(grey ? M::L : M::RGB, opaque ? (grey ? "L" : "RGB") : (grey ? "LA" : "RGBA"), x.prec.bytes * 8);
        if (x.prec.is_float)
            out.pixel_format.sample = PixelFormat::Sample::Float;
    }
    static const char* const compressions[] = {"none", "RLE", "zlib"};
    out.pixel_format.notes.push_back(compressions[x.compression]);
    size_t groups = 0;
    for (const Layer& l : x.layers)
        groups += l.group;
    out.add(N_("XCF"), N_("Version"), std::to_string(x.version));
    out.add(N_("XCF"), N_("Canvas"), std::to_string(w) + " × " + std::to_string(h));
    out.add(N_("XCF"), N_("Precision"), x.prec.name());
    out.add(N_("XCF"), N_("Layers"), std::to_string(x.layers.size() - groups) + (groups ? " (" + std::to_string(groups) + " groups)" : ""));
    if (x.other_modes)
        out.add(N_("XCF"), N_("Note"), N_("some layer modes or compositing options not applied"));
    if (x.effects)
        out.add(N_("XCF"), N_("Note"), N_("layer effects not applied"));
    if (!x.icc.empty())
    {
        out.color_space = icc_profile_description(x.icc.data(), x.icc.size());
        FileMetadata stored;
        stored.icc = std::move(x.icc);
        out.stored = std::make_shared<const FileMetadata>(std::move(stored));
    }
    return true;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    try
    {
        return decode_xcf(path, out, error);
    }
    catch (const std::bad_alloc&)  // a canvas, or a group's, too large to composite in
    {
        return error = "XCF too large to composite", false;
    }
}

}  // namespace

const Decoder& xcf_decoder()
{
    static const Decoder d{"XCF", {"xcf"}, sniff, decode};
    return d;
}
