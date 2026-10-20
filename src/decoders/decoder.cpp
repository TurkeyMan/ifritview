#include "decoder.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <set>

namespace {

std::mutex g_options_mutex;
DecodeOptions g_options;

std::string lowercase_extension(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

bool claims_extension(const Decoder& d, const std::string& ext) {
    return std::find(d.extensions.begin(), d.extensions.end(), ext) != d.extensions.end();
}

bool try_decode(const Decoder& d, const std::string& path, Image& out, std::string& error) {
    out = Image{};
    std::string err;
    if (d.decode(path, out, err)) {
        if (out.format.empty()) out.format = d.name;
        return true;
    }
    if (error.empty()) error = std::string(d.name) + ": " + err;
    return false;
}

}  // namespace

void set_decode_options(const DecodeOptions& options) {
    std::lock_guard lock(g_options_mutex);
    g_options = options;
}

DecodeOptions decode_options() {
    std::lock_guard lock(g_options_mutex);
    return g_options;
}

const std::vector<const Decoder*>& decoders() {
    static const std::vector<const Decoder*> list = [] {
        std::vector<const Decoder*> v;
#ifdef HAVE_RAW
        v.push_back(&raw_decoder());   // before TIFF: most raw files are TIFF containers
#endif
#ifdef HAVE_JPEG
        v.push_back(&jpeg_decoder());
#endif
#ifdef HAVE_PNG
        v.push_back(&png_decoder());
#endif
#ifdef HAVE_WEBP
        v.push_back(&webp_decoder());
#endif
#ifdef HAVE_HEIF
        v.push_back(&heif_decoder());
#endif
#ifdef HAVE_JXL
        v.push_back(&jxl_decoder());
#endif
#ifdef HAVE_TIFF
        v.push_back(&tiff_decoder());
#endif
        v.push_back(&stb_decoder());
        v.push_back(&pixbuf_decoder());  // catch-all, must stay last
        return v;
    }();
    return list;
}

bool decode_image(const std::string& path, Image& out, std::string& error) {
    error.clear();
    const std::string ext = lowercase_extension(path);

    uint8_t head[64];
    size_t head_len = 0;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        head_len = std::fread(head, 1, sizeof head, f);
        std::fclose(f);
    } else {
        error = "cannot open file";
        return false;
    }

    std::set<const Decoder*> tried;
    for (const Decoder* d : decoders()) {
        if (!claims_extension(*d, ext)) continue;
        tried.insert(d);
        if (try_decode(*d, path, out, error)) return true;
    }
    for (const Decoder* d : decoders()) {
        if (tried.count(d) || !d->sniff || !d->sniff(head, head_len)) continue;
        tried.insert(d);
        if (try_decode(*d, path, out, error)) return true;
    }
    const Decoder* fallback = decoders().back();
    if (!tried.count(fallback) && try_decode(*fallback, path, out, error)) return true;

    if (error.empty()) error = "unsupported image format";
    return false;
}

std::vector<std::string> default_extensions() {
    std::vector<std::string> all;
    for (const Decoder* d : decoders())
        for (const std::string& e : d->extensions)
            if (std::find(all.begin(), all.end(), e) == all.end()) all.push_back(e);
    return all;
}
