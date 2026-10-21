// Catch-all through gdk-pixbuf, which ships with GTK: BMP, GIF, ICO, XPM,
// XBM, ANI, SVG and whatever loaders the distro has installed.
#include "decoder.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

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

bool decode(const std::string& path, Image& out, std::string& error)
{
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
