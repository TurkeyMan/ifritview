// Catch-all through gdk-pixbuf, which ships with GTK: BMP, GIF, ICO, XPM,
// XBM, ANI, SVG and whatever loaders the distro has installed.
#include "decoder.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cctype>

namespace {

std::vector<std::string> extensions() {
    std::vector<std::string> list;
    GSList* formats = gdk_pixbuf_get_formats();
    for (GSList* l = formats; l; l = l->next) {
        auto* fmt = static_cast<GdkPixbufFormat*>(l->data);
        if (gdk_pixbuf_format_is_disabled(fmt)) continue;
        gchar** exts = gdk_pixbuf_format_get_extensions(fmt);
        for (gchar** e = exts; e && *e; ++e) {
            std::string ext = *e;
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext == "gz") continue;  // the SVGZ loader claims it; a browse filter shouldn't
            if (std::find(list.begin(), list.end(), ext) == list.end()) list.push_back(ext);
        }
        g_strfreev(exts);
    }
    g_slist_free(formats);
    return list;
}

bool decode(const std::string& path, Image& out, std::string& error) {
    GError* err = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &err);
    if (!pixbuf) {
        error = err ? err->message : "gdk-pixbuf failed";
        if (err) g_error_free(err);
        return false;
    }
    if (GdkPixbuf* oriented = gdk_pixbuf_apply_embedded_orientation(pixbuf)) {
        g_object_unref(pixbuf);
        pixbuf = oriented;
    }
    out.allocate(gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf));
    const guchar* data = gdk_pixbuf_read_pixels(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    out.has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    if (out.has_alpha) out.from_rgba8(data, stride);
    else out.from_rgb8(data, stride);
    g_object_unref(pixbuf);

    if (GdkPixbufFormat* fmt = gdk_pixbuf_get_file_info(path.c_str(), nullptr, nullptr)) {
        gchar* name = gdk_pixbuf_format_get_name(fmt);
        std::string n = name ? name : "";
        g_free(name);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::toupper(c); });
        out.format = n;
    }
    return true;
}

}  // namespace

const Decoder& pixbuf_decoder() {
    static const Decoder d{"gdk-pixbuf", extensions(), nullptr, decode};
    return d;
}
