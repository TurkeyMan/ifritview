#include "icons.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// One file per (theme, scale, colour): raw premultiplied ARGB32 pixels for
// every icon, so loading is a read and a memcpy with no image decoder.
namespace
{

struct Pending
{
    std::string name;
    int size;
};
std::vector<Pending> g_missing;
GtkWidget* g_reference = nullptr;

struct Cached
{
    int width, height;
    std::vector<uint8_t> argb;  // premultiplied, stride = width * 4
};
std::map<std::string, Cached> g_loaded;  // name-size -> pixels
bool g_loaded_once = false;

GtkWidget* reference_widget()
{
    if (!g_reference)
    {
        g_reference = gtk_label_new("");
        g_object_ref_sink(g_reference);
    }
    return g_reference;
}

std::string cache_file(GtkWidget* reference, int scale)
{
    gchar* theme = nullptr;
    g_object_get(gtk_widget_get_settings(reference), "gtk-icon-theme-name", &theme, nullptr);
    GdkRGBA fg{0, 0, 0, 1};
    gtk_style_context_get_color(gtk_widget_get_style_context(reference), GTK_STATE_FLAG_NORMAL, &fg);
    char key[160];
    std::snprintf(key, sizeof key, "icons-%s-%dx-%02x%02x%02x.bin", theme ? theme : "default", scale, int(fg.red * 255 + 0.5), int(fg.green * 255 + 0.5),
                  int(fg.blue * 255 + 0.5));
    g_free(theme);
    gchar* path = g_build_filename(g_get_user_cache_dir(), "ifritview", key, nullptr);
    std::string s = path;
    g_free(path);
    return s;
}

// Format: "IFRI" magic, uint32 count, then per icon: uint16 name length,
// name bytes, uint32 width, uint32 height, width*height*4 bytes.
void load_cache(GtkWidget* reference, int scale)
{
    g_loaded_once = true;
    gchar* data = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(cache_file(reference, scale).c_str(), &data, &len, nullptr))
        return;
    const auto* p = reinterpret_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    auto u32 = [&](const uint8_t*& q)
    {
        uint32_t v;
        std::memcpy(&v, q, 4);
        q += 4;
        return v;
    };
    if (len < 8 || std::memcmp(p, "IFRI", 4) != 0)
    {
        g_free(data);
        return;
    }
    p += 4;
    const uint32_t count = u32(p);
    for (uint32_t i = 0; i < count && p + 2 <= end; ++i)
    {
        uint16_t n;
        std::memcpy(&n, p, 2);
        p += 2;
        if (p + n + 8 > end)
            break;
        std::string name(reinterpret_cast<const char*>(p), n);
        p += n;
        const uint32_t w = u32(p), h = u32(p);
        const size_t bytes = size_t(w) * h * 4;
        if (p + bytes > end)
            break;
        g_loaded[name] = Cached{int(w), int(h), std::vector<uint8_t>(p, p + bytes)};
        p += bytes;
    }
    g_free(data);
}

void save_cache(GtkWidget* reference, int scale)
{
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
    out.insert(out.end(), {'I', 'F', 'R', 'I'});
    put32(uint32_t(g_loaded.size()));
    for (const auto& [name, c] : g_loaded)
    {
        const uint16_t n = uint16_t(name.size());
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 2);
        out.insert(out.end(), name.begin(), name.end());
        put32(uint32_t(c.width));
        put32(uint32_t(c.height));
        out.insert(out.end(), c.argb.begin(), c.argb.end());
    }
    const std::string path = cache_file(reference, scale);
    gchar* dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_file_set_contents(path.c_str(), reinterpret_cast<const gchar*>(out.data()), gssize(out.size()), nullptr);
}

GtkWidget* image_from_cached(const Cached& c, int scale)
{
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, c.width, c.height);
    std::memcpy(cairo_image_surface_get_data(surface), c.argb.data(), c.argb.size());
    cairo_surface_mark_dirty(surface);
    cairo_surface_set_device_scale(surface, scale, scale);
    GtkWidget* image = gtk_image_new_from_surface(surface);
    cairo_surface_destroy(surface);
    return image;
}

}  // namespace

void set_icon_reference(GtkWidget* widget)
{
    g_reference = widget;
}

GtkWidget* cached_icon_image(const char* name, int size)
{
    GtkWidget* ref = reference_widget();
    const int scale = gtk_widget_get_scale_factor(ref);
    if (!g_loaded_once)
        load_cache(ref, scale);
    const std::string key = std::string(name) + "-" + std::to_string(size);
    auto it = g_loaded.find(key);
    if (it != g_loaded.end())
        return image_from_cached(it->second, scale);
    g_missing.push_back({name, size});
    return gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_LARGE_TOOLBAR);
}

void flush_icon_cache(GtkWidget*)
{
    if (g_missing.empty())
        return;
    GtkWidget* reference = reference_widget();
    const int scale = gtk_widget_get_scale_factor(reference);
    GtkIconTheme* theme = gtk_icon_theme_get_default();
    GtkStyleContext* style = gtk_widget_get_style_context(reference);
    for (const Pending& p : g_missing)
    {
        GtkIconInfo* info = gtk_icon_theme_lookup_icon_for_scale(theme, p.name.c_str(), p.size, scale, GTK_ICON_LOOKUP_FORCE_SYMBOLIC);
        if (!info)
            continue;
        gboolean symbolic = FALSE;
        if (GdkPixbuf* pb = gtk_icon_info_load_symbolic_for_context(info, style, &symbolic, nullptr))
        {
            // Store premultiplied ARGB32 exactly as cairo wants it.
            cairo_surface_t* surface = gdk_cairo_surface_create_from_pixbuf(pb, 1, nullptr);
            cairo_surface_flush(surface);
            const int w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
            const int stride = cairo_image_surface_get_stride(surface);
            Cached c{w, h, {}};
            c.argb.resize(size_t(w) * h * 4);
            for (int y = 0; y < h; ++y)
                std::memcpy(c.argb.data() + size_t(y) * w * 4, cairo_image_surface_get_data(surface) + size_t(y) * stride, size_t(w) * 4);
            g_loaded[p.name + "-" + std::to_string(p.size)] = std::move(c);
            cairo_surface_destroy(surface);
            g_object_unref(pb);
        }
        g_object_unref(info);
    }
    g_missing.clear();
    save_cache(reference, scale);
}
