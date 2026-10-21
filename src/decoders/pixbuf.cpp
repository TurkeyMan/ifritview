// Catch-all through gdk-pixbuf, which ships with GTK: BMP, GIF, ICO, XPM,
// XBM, ANI, SVG and whatever loaders the distro has installed.
#include "decoder.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace
{

std::vector<std::string> extensions()
{
    std::vector<std::string> list;
    GSList* formats = gdk_pixbuf_get_formats();
    for (GSList* l = formats; l; l = l->next)
    {
        auto* fmt = static_cast<GdkPixbufFormat*>(l->data);
        if (gdk_pixbuf_format_is_disabled(fmt))
            continue;
        gchar** exts = gdk_pixbuf_format_get_extensions(fmt);
        for (gchar** e = exts; e && *e; ++e)
        {
            std::string ext = *e;
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext == "gz")
                continue;  // the SVGZ loader claims it; a browse filter shouldn't
            if (std::find(list.begin(), list.end(), ext) == list.end())
                list.push_back(ext);
        }
        g_strfreev(exts);
    }
    g_slist_free(formats);
    return list;
}

// Packed formats the loaders normalise away: read the headers ourselves.
int mask_bits(uint32_t m)
{
    int n = 0;
    while (m)
    {
        n += m & 1;
        m >>= 1;
    }
    return n;
}

PixelFormat header_pixel_format(const std::string& path)
{
    using M = PixelFormat::Model;
    PixelFormat none;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return none;
    uint8_t h[70] = {};
    const size_t n = std::fread(h, 1, sizeof h, f);
    std::fclose(f);
    auto le16 = [&](int o) { return uint32_t(h[o] | (h[o + 1] << 8)); };
    auto le32 = [&](int o) { return uint32_t(h[o] | (h[o + 1] << 8) | (h[o + 2] << 16) | (uint32_t(h[o + 3]) << 24)); };
    if (n >= 54 && h[0] == 'B' && h[1] == 'M')
    {
        const uint32_t header_size = le32(14), bpp = le16(28), compression = le32(30);
        const uint32_t colours = le32(46);
        if (bpp <= 8)
            return PixelFormat::palette(int(bpp), int(colours ? colours : 1u << bpp));
        if (compression == 3 && n >= 66)
        {  // BI_BITFIELDS: masks right after the info header (inside it for V4/V5)
            const int r = mask_bits(le32(54)), g = mask_bits(le32(58)), b = mask_bits(le32(62));
            const int a = header_size >= 56 && n >= 70 ? mask_bits(le32(66)) : 0;
            if (a)
                return PixelFormat::widths(M::RGB, "ARGB", {a, r, g, b}, int(bpp));
            const int pad = int(bpp) - r - g - b;
            if (pad > 0)
                return PixelFormat::widths(M::RGB, "XRGB", {pad, r, g, b}, int(bpp));
            return PixelFormat::widths(M::RGB, "RGB", {r, g, b}, int(bpp));
        }
        if (bpp == 16)
            return PixelFormat::widths(M::RGB, "XRGB", {1, 5, 5, 5}, 16);
        if (bpp == 24)
            return PixelFormat::packed(M::RGB, "BGR", 8);
        if (bpp == 32)
            return PixelFormat::packed(M::RGB, "BGRX", 8);
        if (bpp == 64)
            return PixelFormat::packed(M::RGB, "BGRA", 16);
    }
    return none;
}

// Count GIF image descriptors and note interlacing, which the loader hides.
void gif_header_info(const std::string& path, Image& out, int& frames)
{
    frames = 0;
    std::vector<uint8_t> d;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        d.resize(n > 0 ? size_t(n) : 0);
        if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size())
            d.clear();
        std::fclose(f);
    }
    if (d.size() < 13 || std::memcmp(d.data(), "GIF", 3) != 0)
        return;
    const uint8_t packed = d[10];
    const int table_bits = (packed & 7) + 1;
    out.pixel_format = PixelFormat::palette(8, packed & 0x80 ? 1 << table_bits : 0);
    out.pixel_format.notes.push_back("LZW");
    size_t pos = 13 + (packed & 0x80 ? size_t(3) << table_bits : 0);
    bool interlaced = false, transparent = false;
    auto skip_blocks = [&] {
        while (pos < d.size() && d[pos])
            pos += d[pos] + 1;
        ++pos;
    };
    while (pos < d.size())
    {
        const uint8_t b = d[pos++];
        if (b == 0x2C)
        {
            if (pos + 9 > d.size())
                break;
            const uint8_t flags = d[pos + 8];
            interlaced |= (flags & 0x40) != 0;
            pos += 9 + (flags & 0x80 ? size_t(3) << ((flags & 7) + 1) : 0) + 1;  // + LZW minimum code size
            skip_blocks();
            ++frames;
        }
        else if (b == 0x21)
        {
            if (pos < d.size() && d[pos] == 0xF9 && pos + 2 < d.size())
                transparent |= (d[pos + 2] & 1) != 0;
            ++pos;
            skip_blocks();
        }
        else
            break;
    }
    if (interlaced)
        out.pixel_format.notes.push_back("interlaced");
    if (transparent)
        out.pixel_format.notes.push_back("transparency key");
    out.add("GIF", "Version", std::string(reinterpret_cast<const char*>(d.data() + 3), 3));
    out.add("GIF", "Frames", std::to_string(frames));
}

// Copy a pixbuf into ARGB32; GIF frames are composed canvases from the loader.
void pixbuf_to_argb(GdkPixbuf* pb, Image& out)
{
    out.allocate(gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb));
    if (gdk_pixbuf_get_has_alpha(pb))
        out.from_rgba8(gdk_pixbuf_read_pixels(pb), gdk_pixbuf_get_rowstride(pb));
    else
        out.from_rgb8(gdk_pixbuf_read_pixels(pb), gdk_pixbuf_get_rowstride(pb));
}

// gdk-pixbuf 2.44 deprecates its animation API in favour of glycin; it still
// works and glycin is not a dependency we want yet.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

bool decode_gif(const std::string& path, Image& out, std::string& error)
{
    int frames = 0;
    gif_header_info(path, out, frames);
    GError* err = nullptr;
    GdkPixbufAnimation* anim = gdk_pixbuf_animation_new_from_file(path.c_str(), &err);
    if (!anim)
    {
        error = err ? err->message : "gdk-pixbuf failed";
        if (err)
            g_error_free(err);
        return false;
    }
    PixelFormat pf = out.pixel_format;
    std::vector<MetadataGroup> meta = out.metadata;
    if (gdk_pixbuf_animation_is_static_image(anim) || frames <= 1)
    {
        pixbuf_to_argb(gdk_pixbuf_animation_get_static_image(anim), out);
    }
    else
    {
        // Walk the animation at its own pace, one full composed canvas per frame.
        GTimeVal t{0, 0};
        GdkPixbufAnimationIter* it = gdk_pixbuf_animation_get_iter(anim, &t);
        auto animation = std::make_shared<Animation>();
        const size_t budget = size_t(512) << 20;
        size_t used = 0;
        for (int i = 0; i < frames; ++i)
        {
            int delay = gdk_pixbuf_animation_iter_get_delay_time(it);
            if (delay < 0)
                delay = 100;
            Image tmp;
            pixbuf_to_argb(gdk_pixbuf_animation_iter_get_pixbuf(it), tmp);
            if (i == 0)
            {
                out.allocate(tmp.width, tmp.height);
                out.pixels = tmp.pixels;
            }
            used += tmp.pixels.size();
            animation->frames.push_back({std::move(tmp.pixels), delay});
            if (used > budget)
                break;
            g_time_val_add(&t, gint64(delay) * 1000);
            gdk_pixbuf_animation_iter_advance(it, &t);
        }
        g_object_unref(it);
        if (animation->frames.size() > 1)
            out.animation = animation;
    }
    g_object_unref(anim);
    out.pixel_format = pf;
    out.metadata = meta;
    out.has_alpha = true;
    out.format = out.animation ? "GIF, animated (" + std::to_string(frames) + " frames)" : "GIF";
    return true;
}

G_GNUC_END_IGNORE_DEPRECATIONS

bool decode(const std::string& path, Image& out, std::string& error)
{
    {
        uint8_t head[6] = {};
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            const size_t n = std::fread(head, 1, 6, f);
            std::fclose(f);
            if (n == 6 && std::memcmp(head, "GIF8", 4) == 0)
                return decode_gif(path, out, error);
        }
    }
    GError* err = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &err);
    if (!pixbuf)
    {
        error = err ? err->message : "gdk-pixbuf failed";
        if (err)
            g_error_free(err);
        return false;
    }
    if (GdkPixbuf* oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf))
    {
        g_object_unref(pixbuf);
        pixbuf = oriented;
    }
    out.allocate(gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf));
    const guchar* data = gdk_pixbuf_read_pixels(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    out.has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    out.pixel_format = header_pixel_format(path);
    if (out.pixel_format.empty())
    {
        out.pixel_format = PixelFormat::packed(PixelFormat::Model::RGB, out.has_alpha ? "RGBA" : "RGB", gdk_pixbuf_get_bits_per_sample(pixbuf));
        out.pixel_format.notes.push_back("as decoded");
    }
    if (out.has_alpha)
        out.from_rgba8(data, stride);
    else
        out.from_rgb8(data, stride);
    g_object_unref(pixbuf);

    if (GdkPixbufFormat* fmt = gdk_pixbuf_get_file_info(path.c_str(), nullptr, nullptr))
    {
        gchar* name = gdk_pixbuf_format_get_name(fmt);
        std::string n = name ? name : "";
        g_free(name);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::toupper(c); });
        out.format = n;
    }
    return true;
}

}  // namespace

const Decoder& pixbuf_decoder()
{
    static const Decoder d{"gdk-pixbuf", extensions(), nullptr, decode};
    return d;
}
