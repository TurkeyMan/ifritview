// XMP via exempi: the packet embedded in the file, plus a Lightroom/darktable
// style sidecar beside it (IMG.xmp or IMG.ext.xmp). Everything is listed
// under its schema prefix; crs:* are Camera Raw's edit parameters.
#include "decoder.h"
#include "../i18n.h"
#include "../platform.h"

#include <exempi/xmp.h>

#include <glib.h>

#include <algorithm>
#include <cstdio>
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
    // The process its settings are for, then them.
    Adjustments& a = d.adjust;
    const std::string process = get_string(xmp, kCrs, "ProcessVersion");
    if (!process.empty())
    {
        d.process_version = process;
        a.legacy = g_ascii_strtod(process.c_str(), nullptr) < 6;
    }
    else if (!xmp_has_property(xmp, kCrs, "Contrast2012") &&
             (xmp_has_property(xmp, kCrs, "Brightness") || xmp_has_property(xmp, kCrs, "FillLight")))
        a.legacy = true;
    for (const CrsField& f : crs_fields(a))
        if (get_double(xmp, kCrs, f.name.c_str(), v))
        {
            *f.value = v;
            d.present = true;
        }
    for (const CrsCurve& c : crs_curves(a))
    {
        Adjustments::Curve points;
        XmpStringPtr item = xmp_string_new();
        for (int i = 1; xmp_get_array_item(xmp, kCrs, c.name, i, item, nullptr); ++i)
        {
            int x = 0, y = 0;
            if (std::sscanf(xmp_string_cstr(item), "%d , %d", &x, &y) == 2)
                points.emplace_back(x, y);
        }
        xmp_string_free(item);
        if (!points.empty())
        {
            *c.curve = std::move(points);
            d.present = true;
        }
    }
    if (std::string name = get_string(xmp, kCrs, crs_curve_name(a)); !name.empty())
        a.curve_name = std::move(name);
    // The rest of Camera Raw's simple settings, not applied (yet), kept for
    // writing them back.
    if (XmpIteratorPtr it = xmp_iterator_new(xmp, kCrs, nullptr, XmpIterOptions(XMP_ITER_JUSTLEAFNODES | XMP_ITER_OMITQUALIFIERS)))
    {
        std::vector<std::string> owned = {"ProcessVersion", "Exposure", "Exposure2012", "WhiteBalance", "Temperature", "Tint",
                                          "ToneCurveName", "ToneCurveName2012", "HasSettings"};
        for (const CrsField& f : crs_fields(a))
            owned.push_back(f.name);
        XmpStringPtr schema = xmp_string_new(), path = xmp_string_new(), value = xmp_string_new();
        uint32_t options = 0;
        while (xmp_iterator_next(it, schema, path, value, &options))
        {
            const std::string_view p = xmp_string_cstr(path);
            if (p.size() <= 4 || p.substr(0, 4) != "crs:" || p.find_first_of("[/") != std::string_view::npos ||
                (options & (XMP_PROP_VALUE_IS_STRUCT | XMP_PROP_VALUE_IS_ARRAY)))
                continue;
            const std::string name(p.substr(4));
            if (std::find(owned.begin(), owned.end(), name) == owned.end())
                a.kept[name] = xmp_string_cstr(value);
        }
        xmp_string_free(schema);
        xmp_string_free(path);
        xmp_string_free(value);
        xmp_iterator_free(it);
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
    // Sidecars: IMG_0001.CR2.xmp (darktable), then IMG_0001.xmp (Lightroom's,
    // and what IfritView writes): the last one read has the last word.
    read_sidecar(path + ".xmp", out);
    if (const std::string lightroom = xmp_sidecar_path(path); lightroom != path + ".xmp")
        read_sidecar(lightroom, out);
}

std::string xmp_sidecar_path(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    const auto slash = path.find_last_of("/\\");  // either slash: a dot in a folder's name isn't one
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return path.substr(0, dot) + ".xmp";
    return path + ".xmp";
}

namespace
{

// A setting as Lightroom writes it, whatever the locale: "+15", "-0.35",
// zero unsigned.
std::string crs_number(double v, const char* format = "%+.0f")
{
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(text, sizeof text, format, v);
    if (text[0] == '+' || text[0] == '-')
        if (g_ascii_strtod(text + 1, nullptr) == 0)
            return text + 1;
    return text;
}

}  // namespace

bool write_xmp_sidecar(const std::string& path, const XmpDevelop& edits, std::string& error)
{
    ensure_init();
    const std::string sidecar = xmp_sidecar_path(path);
    // What's there already, all of it kept but what's set below; one that
    // can't be read is left as it is.
    XmpPtr xmp = nullptr;
    gchar* text = nullptr;
    gsize len = 0;
    if (g_file_get_contents(sidecar.c_str(), &text, &len, nullptr))
    {
        xmp = xmp_new_empty();
        const bool ok = xmp_parse(xmp, text, len);
        g_free(text);
        if (!ok)
        {
            xmp_free(xmp);
            error = rap::i18n::format(_("{0} isn't XMP that can be read: left as it is"), sidecar);
            return false;
        }
    }
    else
        xmp = xmp_new_empty();
    XmpStringPtr prefix = xmp_string_new();
    xmp_register_namespace(kCrs, "crs", prefix);
    xmp_string_free(prefix);

    Adjustments a = edits.adjust;
    const auto set = [&](const char* name, const std::string& value) { xmp_set_property(xmp, kCrs, name, value.c_str(), 0); };
    // Settings it read and doesn't apply yet go back where they're missing.
    for (const auto& [name, value] : a.kept)
        if (!xmp_has_property(xmp, kCrs, name.c_str()))
            set(name.c_str(), value);
    std::string process = edits.process_version;
    if (process.empty() || (g_ascii_strtod(process.c_str(), nullptr) < 6) != a.legacy)
        process = a.legacy ? "5.7" : "11.0";
    set("ProcessVersion", process);
    set(a.legacy ? "Exposure" : "Exposure2012", crs_number(edits.exposure, "%+.2f"));
    if (edits.temperature > 0)
    {
        set("WhiteBalance", "Custom");
        set("Temperature", crs_number(edits.temperature, "%.0f"));
        set("Tint", crs_number(edits.tint));
    }
    else
    {
        // As shot: the camera's, which Lightroom reads from the raw.
        set("WhiteBalance", "As Shot");
        xmp_delete_property(xmp, kCrs, "Temperature");
        xmp_delete_property(xmp, kCrs, "Tint");
    }
    for (const CrsField& f : crs_fields(a))
        if (!f.process || f.process == (a.legacy ? 2010 : 2012))
            set(f.name.c_str(), crs_number(*f.value, f.sign ? "%+.0f" : "%.0f"));
    for (const CrsCurve& c : crs_curves(a))
    {
        xmp_delete_property(xmp, kCrs, c.name);
        Adjustments::Curve points = *c.curve;
        if (points.size() < 2)
            points = {{0, 0}, {255, 255}};
        for (const auto& [x, y] : points)
            xmp_append_array_item(xmp, kCrs, c.name, XMP_PROP_ARRAY_IS_ORDERED, (std::to_string(x) + ", " + std::to_string(y)).c_str(), 0);
    }
    const bool linear = a.curve.size() < 2 || a.curve == Adjustments::Curve{{0, 0}, {255, 255}};
    set(crs_curve_name(a), !a.curve_name.empty() ? a.curve_name : linear ? "Linear" : "Custom");
    set("HasSettings", "True");

    XmpStringPtr packet = xmp_string_new();
    bool ok = xmp_serialize_and_format(xmp, packet, XMP_SERIAL_OMITPACKETWRAPPER | XMP_SERIAL_USECOMPACTFORMAT, 0, "\n", " ", 0);
    if (ok)
    {
        const std::string bytes = xmp_string_cstr(packet);
        ok = platform::write_replacing(
            sidecar,
            [&](const std::string& temp, std::string& why)
            {
                GError* e = nullptr;
                if (g_file_set_contents(temp.c_str(), bytes.data(), gssize(bytes.size()), &e))
                    return true;
                why = e ? e->message : "";
                g_clear_error(&e);
                return false;
            },
            error);
    }
    else
        error = _("The XMP couldn't be put together");
    xmp_string_free(packet);
    xmp_free(xmp);
    return ok;
}
