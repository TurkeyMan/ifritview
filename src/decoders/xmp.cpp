// XMP via exempi: the packet embedded in the file, plus a Lightroom/darktable
// style sidecar beside it (IMG.xmp or IMG.ext.xmp). Everything is listed
// under its schema prefix; crs:* are Camera Raw's edit parameters.
#include "decoder.h"
#include "../i18n.h"

#include <exempi/xmp.h>

#include <glib.h>

#include <mutex>
#include <string_view>

namespace
{

constexpr int kMaxItems = 600;

void ensure_init()
{
    static std::once_flag once;
    std::call_once(once, [] { xmp_init(); });
}

// Walk every leaf property and add it as "prefix:path".
void collect(XmpPtr xmp, const std::string& group, Image& out)
{
    XmpIteratorPtr it = xmp_iterator_new(xmp, nullptr, nullptr, XmpIterOptions(XMP_ITER_JUSTLEAFNODES | XMP_ITER_OMITQUALIFIERS));
    if (!it)
        return;
    XmpStringPtr schema = xmp_string_new(), path = xmp_string_new(), value = xmp_string_new();
    int count = 0;
    uint32_t options = 0;
    while (count < kMaxItems && xmp_iterator_next(it, schema, path, value, &options))
    {
        const char* p = xmp_string_cstr(path);
        const char* v = xmp_string_cstr(value);
        if (!p || !*p || !v || !*v)
            continue;
        if (options & XMP_PROP_VALUE_IS_STRUCT)
            continue;
        std::string key = p;
        // Trim noise: "dc:subject[1]" reads better as "dc:subject".
        if (key.size() > 3 && key.back() == ']' && key.find("[1]") == key.size() - 3)
            key.erase(key.size() - 3);
        std::string val = v;
        if (val.size() > 500)
            val = val.substr(0, 500) + "…";
        out.add(group, key, val);
        ++count;
    }
    xmp_string_free(schema);
    xmp_string_free(path);
    xmp_string_free(value);
    xmp_iterator_free(it);
}

const char* kCrs = "http://ns.adobe.com/camera-raw-settings/1.0/";
const char* kTiff = "http://ns.adobe.com/tiff/1.0/";

bool get_double(XmpPtr xmp, const char* ns, const char* name, double& value)
{
    XmpStringPtr str = xmp_string_new();
    bool ok = xmp_get_property(xmp, ns, name, str, nullptr) && xmp_string_cstr(str) && *xmp_string_cstr(str);
    if (ok)
        value = g_ascii_strtod(xmp_string_cstr(str), nullptr);
    xmp_string_free(str);
    return ok;
}

std::string get_string(XmpPtr xmp, const char* ns, const char* name)
{
    XmpStringPtr str = xmp_string_new();
    std::string value;
    if (xmp_get_property(xmp, ns, name, str, nullptr) && xmp_string_cstr(str))
        value = xmp_string_cstr(str);
    xmp_string_free(str);
    return value;
}

// Later sources (a sidecar) override earlier ones (the embedded packet).
void collect_develop(XmpPtr xmp, Image& out)
{
    XmpDevelop& d = out.xmp;
    double v = 0;
    if (get_double(xmp, kCrs, "Exposure2012", v) || get_double(xmp, kCrs, "Exposure", v))
    {
        d.exposure = v;
        d.present = true;
    }
    const std::string wb = get_string(xmp, kCrs, "WhiteBalance");
    if (wb != "As Shot" && get_double(xmp, kCrs, "Temperature", v) && v > 0)
    {
        d.temperature = v;
        d.tint = get_double(xmp, kCrs, "Tint", v) ? v : 0;
        d.present = true;
    }
    if (get_double(xmp, kTiff, "Orientation", v) && v >= 1 && v <= 8)
    {
        d.orientation = int(v);
        d.present = true;
    }
    const std::string has_crop = get_string(xmp, kCrs, "HasCrop");
    if (has_crop == "True" || has_crop == "true")
    {
        double l = 0, t = 0, r = 1, b = 1;
        get_double(xmp, kCrs, "CropLeft", l);
        get_double(xmp, kCrs, "CropTop", t);
        get_double(xmp, kCrs, "CropRight", r);
        get_double(xmp, kCrs, "CropBottom", b);
        if (r > l && b > t)
        {
            d.has_crop = true;
            d.crop_left = l;
            d.crop_top = t;
            d.crop_right = r;
            d.crop_bottom = b;
            d.present = true;
        }
    }
}

bool read_sidecar(const std::string& sidecar, Image& out)
{
    gchar* text = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(sidecar.c_str(), &text, &len, nullptr))
        return false;
    XmpPtr xmp = xmp_new_empty();
    const bool ok = xmp_parse(xmp, text, len);
    if (ok)
    {
        gchar* base = g_path_get_basename(sidecar.c_str());
        out.add(N_("XMP sidecar"), N_("File"), base);
        g_free(base);
        collect(xmp, "XMP sidecar", out);
        collect_develop(xmp, out);
    }
    xmp_free(xmp);
    g_free(text);
    return ok;
}

}  // namespace

// The embedded XMP packet sits verbatim in the file (JPEG APP1, PNG iTXt,
// TIFF/DNG tag, WebP chunk...); find it in the bytes and parse just that,
// instead of exempi's file handlers reading the whole file again.
void read_xmp(const std::string& path, const uint8_t* data, size_t size, Image& out)
{
    ensure_init();
    if (data && size)
    {
        const char* begin = reinterpret_cast<const char*>(data);
        const std::string_view all(begin, size);
        const size_t start = all.find("<x:xmpmeta");
        const size_t end = start == std::string_view::npos ? start : all.find("</x:xmpmeta>", start);
        if (end != std::string_view::npos)
        {
            const size_t len = end + 12 - start;
            XmpPtr xmp = xmp_new_empty();
            if (xmp_parse(xmp, begin + start, len))
            {
                collect(xmp, "XMP", out);
                collect_develop(xmp, out);
            }
            xmp_free(xmp);
        }
    }
    // Sidecars: IMG_0001.CR2.xmp (darktable) first, then IMG_0001.xmp (Lightroom).
    if (!read_sidecar(path + ".xmp", out))
    {
        const auto dot = path.find_last_of('.');
        const auto slash = path.find_last_of("/\\");  // either slash: a dot in a folder's name isn't one
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            read_sidecar(path.substr(0, dot) + ".xmp", out);
    }
}
