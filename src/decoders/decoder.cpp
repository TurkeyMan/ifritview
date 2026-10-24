#include "decoder.h"

#include "../platform.h"
#include "../video.h"

#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>

namespace
{

struct FileBuffer
{
    std::string path;
    PixelBuffer data;  // huge-page backed when large: a 25 MB raw faults in a dozen times, not thousands
    bool ok = false;
};
thread_local FileBuffer t_file;

std::mutex g_options_mutex;
DecodeOptions g_options;

std::string lowercase_extension(const std::string& path)
{
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

bool claims_extension(const Decoder& d, const std::string& ext)
{
    return std::find(d.extensions.begin(), d.extensions.end(), ext) != d.extensions.end();
}

// Metadata that lives in containers rather than codecs: IPTC in JPEG
// APP13, XMP packets and sidecars. Added once a decoder has succeeded.
// Metadata that lives in containers rather than codecs: IPTC in JPEG APP13,
// XMP packets and sidecars. Runs on its own thread, from the same bytes, while
// the pixels decode; the results land in `extra` and are merged afterwards.
void enrich_metadata(const std::string& path, const uint8_t* data, size_t size, Image& extra)
{
#ifdef HAVE_IPTC
    read_iptc_jpeg(data, size, extra);
#endif
#ifdef HAVE_XMP
    read_xmp(path, data, size, extra);
#else
    (void)path;
    (void)data;
    (void)size;
    (void)extra;
#endif
}

void merge_metadata(Image& out, Image&& extra)
{
    for (MetadataGroup& g : extra.metadata)
        out.metadata.push_back(std::move(g));
    if (extra.xmp.present)
        out.xmp = extra.xmp;
}

// Starts the metadata pass alongside a decode; join() waits for it. The
// display-affecting fields (XMP orientation, crop, exposure) are in by then.
struct MetadataPass
{
    Image extra;
    std::thread thread;
    MetadataPass(const std::string& path, const uint8_t* data, size_t size) : thread([this, path, data, size] { enrich_metadata(path, data, size, extra); }) {}
    void finish(Image& out)
    {
        thread.join();
        merge_metadata(out, std::move(extra));
    }
    ~MetadataPass()
    {
        if (thread.joinable())
            thread.join();
    }
};

bool try_decode(const Decoder& d, const std::string& path, Image& out, std::string& error)
{
    out = Image{};
    std::string err;
    const gint64 t0 = g_get_monotonic_time();
    const bool ok = d.decode(path, out, err);
    if (g_getenv("IFRIT_DECODE_TIMING"))
        std::fprintf(stderr, "  %s decode %.1f ms\n", d.name, (g_get_monotonic_time() - t0) / 1000.0);
    if (ok)
    {
        if (out.format.empty())
            out.format = d.name;
        return true;
    }
    if (error.empty())
        error = std::string(d.name) + ": " + err;
    return false;
}

}  // namespace

bool file_bytes(const std::string& path, const uint8_t*& data, size_t& size)
{
    if (t_file.path != path)
    {
        t_file.path = path;
        t_file.ok = false;
        t_file.data.clear();
        const int64_t n = platform::file_size(path);
        if (FILE* f = n >= 0 ? platform::fopen(path, "rb") : nullptr)
        {
            if (n >= 0)
            {
                t_file.data.resize(size_t(n));
                t_file.ok = n == 0 || std::fread(t_file.data.data(), 1, t_file.data.size(), f) == t_file.data.size();
            }
            std::fclose(f);
        }
    }
    data = t_file.data.data();
    size = t_file.data.size();
    return t_file.ok;
}

FILE* open_file(const std::string& path)
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (file_bytes(path, data, size) && size > 0)
        if (FILE* f = platform::fmemopen(data, size))
            return f;
    return platform::fopen(path, "rb");  // Windows: no fmemopen; the OS cache has the bytes anyway
}

void release_file_bytes()
{
    t_file = FileBuffer{};
}

void set_decode_options(const DecodeOptions& options)
{
    std::lock_guard lock(g_options_mutex);
    g_options = options;
}

DecodeOptions decode_options()
{
    std::lock_guard lock(g_options_mutex);
    return g_options;
}

const std::vector<const Decoder*>& decoders()
{
    static const std::vector<const Decoder*> list = []
    {
        std::vector<const Decoder*> v;
#ifdef HAVE_RAW
        v.push_back(&raw_decoder());  // before TIFF: most raw files are TIFF containers
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
#ifdef HAVE_PDF
        if (pdf_available())  // poppler is loaded when the first PDF is opened
            v.push_back(&pdf_decoder());
#endif
#ifdef HAVE_VIDEO
        if (video_available())  // GStreamer is loaded when the first video is opened
            v.push_back(&video_decoder());
#endif
        v.push_back(&tim_decoder());
        v.push_back(&ilbm_decoder());
        v.push_back(&pcx_decoder());
        v.push_back(&netpbm_decoder());
        v.push_back(&dds_decoder());
        v.push_back(&vtf_decoder());
        v.push_back(&blp_decoder());
        v.push_back(&tim2_decoder());
        v.push_back(&gim_decoder());
        v.push_back(&stb_decoder());
        v.push_back(&pixbuf_decoder());  // catch-all, must stay last
        return v;
    }();
    return list;
}

bool decode_image(const std::string& path, Image& out, std::string& error)
{
    error.clear();
    const std::string ext = lowercase_extension(path);
    // Videos: GStreamer streams the file; don't read gigabytes for a poster frame.
    for (const Decoder* d : decoders())
        if (d->streaming && claims_extension(*d, ext))
            return try_decode(*d, path, out, error);
    const uint8_t* data = nullptr;  // read once; every decode releases the bytes when done, so this is fresh
    size_t size = 0;
    if (!file_bytes(path, data, size))
    {
        error = "cannot open file";
        return false;
    }
    const uint8_t* head = data;
    const size_t head_len = std::min<size_t>(size, 64);
    MetadataPass meta(path, data, size);

    bool ok = false;
    std::set<const Decoder*> tried;
    for (const Decoder* d : decoders())
    {
        if (ok || !claims_extension(*d, ext))
            continue;
        tried.insert(d);
        ok = try_decode(*d, path, out, error);
    }
    for (const Decoder* d : decoders())
    {
        if (ok || tried.count(d) || !d->sniff || !d->sniff(head, head_len))
            continue;
        tried.insert(d);
        ok = try_decode(*d, path, out, error);
    }
    const Decoder* fallback = decoders().back();
    if (!ok && !tried.count(fallback))
        ok = try_decode(*fallback, path, out, error);
    if (ok)
    {
        meta.finish(out);
        error.clear();
    }
    else if (error.empty())
        error = "unsupported image format";
    if (meta.thread.joinable())
        meta.thread.join();
    release_file_bytes();
    return ok;
}

bool decode_preview(const std::string& path, Image& out)
{
    const std::string ext = lowercase_extension(path);
    for (const Decoder* d : decoders())
        if (d->preview && claims_extension(*d, ext))
        {
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (!file_bytes(path, data, size))
                return false;
            MetadataPass meta(path, data, size);
            out = Image{};
            const bool ok = d->preview(path, out);
            if (ok)
            {
                out.stage = Image::Stage::Preview;
                meta.finish(out);
            }
            else if (meta.thread.joinable())
                meta.thread.join();
            if (ok)
                release_file_bytes();  // on failure the full decode that follows reuses them
            return ok;
        }
    return false;
}

std::vector<std::string> default_extensions()
{
    std::vector<std::string> all;
    for (const Decoder* d : decoders())
        for (const std::string& e : d->extensions)
            if (e.find('.') == std::string::npos && std::find(all.begin(), all.end(), e) == all.end())  // "svg.gz": svgz covers it
                all.push_back(e);
    return all;
}

bool extension_supported(const std::string& ext)
{
    static const std::set<std::string> supported = []
    {
        const std::vector<std::string> all = default_extensions();
        return std::set<std::string>(all.begin(), all.end());
    }();
    return supported.count(ext) != 0;
}

void preload_optional_libraries()
{
    static bool started = false;
    if (started)
        return;
    started = true;
    std::thread(
        []
        {
            platform::lower_thread_priority();
#ifdef HAVE_PDF
            if (pdf_available())
                pdf_supported();
#endif
#ifdef HAVE_VIDEO
            if (video_available())
                video_supported();
#endif
        })
        .detach();
}
