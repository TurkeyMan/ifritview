// XMP via exempi: the packet embedded in the file, plus a Lightroom/darktable
// style sidecar beside it (IMG.xmp or IMG.ext.xmp). Everything is listed
// under its schema prefix; crs:* are Camera Raw's edit parameters.
#include "decoder.h"

#include <exempi/xmp.h>

#include <glib.h>

#include <mutex>

namespace {

constexpr int kMaxItems = 600;

void ensure_init() {
    static std::once_flag once;
    std::call_once(once, [] { xmp_init(); });
}

// Walk every leaf property and add it as "prefix:path".
void collect(XmpPtr xmp, const std::string& group, Image& out) {
    XmpIteratorPtr it = xmp_iterator_new(xmp, nullptr, nullptr, XmpIterOptions(XMP_ITER_JUSTLEAFNODES | XMP_ITER_OMITQUALIFIERS));
    if (!it) return;
    XmpStringPtr schema = xmp_string_new(), path = xmp_string_new(), value = xmp_string_new();
    int count = 0;
    uint32_t options = 0;
    while (count < kMaxItems && xmp_iterator_next(it, schema, path, value, &options)) {
        const char* p = xmp_string_cstr(path);
        const char* v = xmp_string_cstr(value);
        if (!p || !*p || !v || !*v) continue;
        if (options & XMP_PROP_VALUE_IS_STRUCT) continue;
        std::string key = p;
        // Trim noise: "dc:subject[1]" reads better as "dc:subject".
        if (key.size() > 3 && key.back() == ']' && key.find("[1]") == key.size() - 3) key.erase(key.size() - 3);
        std::string val = v;
        if (val.size() > 500) val = val.substr(0, 500) + "…";
        out.add(group, key, val);
        ++count;
    }
    xmp_string_free(schema);
    xmp_string_free(path);
    xmp_string_free(value);
    xmp_iterator_free(it);
}

bool read_sidecar(const std::string& sidecar, Image& out) {
    gchar* text = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(sidecar.c_str(), &text, &len, nullptr)) return false;
    XmpPtr xmp = xmp_new_empty();
    const bool ok = xmp_parse(xmp, text, len);
    if (ok) {
        gchar* base = g_path_get_basename(sidecar.c_str());
        out.add("XMP sidecar", "File", base);
        g_free(base);
        collect(xmp, "XMP sidecar", out);
    }
    xmp_free(xmp);
    g_free(text);
    return ok;
}

}  // namespace

void read_xmp(const std::string& path, Image& out) {
    ensure_init();
    if (XmpFilePtr file = xmp_files_open_new(path.c_str(), XmpOpenFileOptions(XMP_OPEN_READ | XMP_OPEN_ONLYXMP))) {
        if (XmpPtr xmp = xmp_files_get_new_xmp(file)) {
            collect(xmp, "XMP", out);
            xmp_free(xmp);
        }
        xmp_files_close(file, XMP_CLOSE_NOOPTION);
        xmp_files_free(file);
    }
    // Sidecars: IMG_0001.CR2.xmp (darktable) first, then IMG_0001.xmp (Lightroom).
    if (!read_sidecar(path + ".xmp", out)) {
        const auto dot = path.find_last_of('.');
        const auto slash = path.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) read_sidecar(path.substr(0, dot) + ".xmp", out);
    }
}
