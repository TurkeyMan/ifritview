#include "decoder.h"

#include "../platform.h"
#include "../plugins.h"
#include "../video.h"

#include <glib.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace
{

struct FileBuffer
{
    std::string path;
    std::shared_ptr<const FileBlob> blob;
};
thread_local FileBuffer t_file;
thread_local int t_fit_w = 0, t_fit_h = 0;  // set_preview_fit

std::mutex g_options_mutex;
DecodeOptions g_options;

}  // namespace

std::string file_extension(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

std::string normalise_extension(std::string ext)
{
    ext.erase(std::remove_if(ext.begin(), ext.end(), [](unsigned char c) { return std::isspace(c) || c == '.'; }), ext.end());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

namespace
{

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
    FileMetadata stored = extract_metadata(data, size);
    if (!stored.empty())
        extra.stored = std::make_shared<const FileMetadata>(std::move(stored));
#ifdef HAVE_IPTC
    read_iptc_jpeg(data, size, extra);
#endif
    read_xmp(path, data, size, extra);
}

// A space the file named but didn't embed a profile for: one made for it
// joins what's stored, unless the file has its own (or it's sRGB, which an
// untagged image means anyway).
void carry_declared_space(Image& out)
{
    if (!out.declared_space || out.declared_space->is_srgb() || (out.stored && !out.stored->icc.empty()))
        return;
    FileMetadata m = out.stored ? *out.stored : FileMetadata{};
    m.icc = icc_profile(*out.declared_space);
    out.stored = std::make_shared<const FileMetadata>(std::move(m));
    if (out.color_space.empty())
        out.color_space = out.declared_space->name;
}

void merge_metadata(Image& out, Image&& extra)
{
    for (MetadataGroup& g : extra.metadata)
        out.metadata.push_back(std::move(g));
    if (extra.xmp.present)
        out.xmp = extra.xmp;
    // The container's and the decoder's (a library's) together.
    if (extra.stored && out.stored)
    {
        FileMetadata both = *out.stored;
        both.merge(*extra.stored);
        out.stored = std::make_shared<const FileMetadata>(std::move(both));
    }
    else if (extra.stored)
        out.stored = std::move(extra.stored);
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
        carry_declared_space(out);
    }
    ~MetadataPass()
    {
        if (thread.joinable())
            thread.join();
    }
};

bool try_decode(const Decoder& d, const std::string& path, Image& out, std::string& error, ImagePart part = {})
{
    out = Image{};
    std::string err;
    const gint64 t0 = g_get_monotonic_time();
    bool ok = false;
    try
    {
        if (d.decode_part)
            ok = d.decode_part(path, part, out, err);
        else if (part == ImagePart{})
            ok = d.decode(path, out, err);
        else
            err = "it has one image";
        out.part = part;
    }
    catch (const std::length_error& e)  // Image::allocate: too large to hold
    {
        err = e.what();
    }
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

std::shared_ptr<const FileBlob> read_file(const std::string& path)
{
    auto blob = std::make_shared<FileBlob>();
    const int64_t n = platform::file_size(path);
    if (FILE* f = n >= 0 ? platform::fopen(path, "rb") : nullptr)
    {
        blob->data.resize(size_t(n));
        blob->ok = n == 0 || std::fread(blob->data.data(), 1, blob->data.size(), f) == blob->data.size();
        std::fclose(f);
    }
    return blob;
}

void provide_file_bytes(const std::string& path, std::shared_ptr<const FileBlob> blob)
{
    t_file.path = path;
    t_file.blob = std::move(blob);
}

bool file_bytes(const std::string& path, const uint8_t*& data, size_t& size)
{
    if (t_file.path != path || !t_file.blob)
    {
        t_file.path = path;
        t_file.blob = read_file(path);
    }
    data = t_file.blob->data.data();
    size = t_file.blob->data.size();
    return t_file.blob->ok;
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

void set_preview_fit(int width, int height)
{
    t_fit_w = width;
    t_fit_h = height;
}

void preview_fit(int& width, int& height)
{
    width = t_fit_w;
    height = t_fit_h;
}

void release_file_bytes()
{
    t_file = FileBuffer{};
}

std::shared_ptr<const void> file_bytes_owner()
{
    return t_file.blob;
}

namespace
{
thread_local bool t_cpu_develop = false;
}

void develop_raws_on_cpu(bool on)
{
    t_cpu_develop = on;
}

void set_decode_options(const DecodeOptions& options)
{
    std::lock_guard lock(g_options_mutex);
    g_options = options;
}

DecodeOptions decode_options()
{
    std::lock_guard lock(g_options_mutex);
    DecodeOptions o = g_options;
    if (t_cpu_develop)
        o.gpu_develop = o.gpu_decode = false;
    return o;
}

static const std::vector<const Decoder*>& builtin_decoders()
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
        v.push_back(&wal_decoder());
        v.push_back(&lmp_decoder());
        v.push_back(&pkm_decoder());
        v.push_back(&astc_decoder());
        v.push_back(&ktx_decoder());
        v.push_back(&ktx2_decoder());
        v.push_back(&tpl_decoder());
        v.push_back(&bti_decoder());
        v.push_back(&pvr_decoder());
        v.push_back(&exr_decoder());
        v.push_back(&stb_decoder());
        v.push_back(&pixbuf_decoder());  // catch-all, must stay last
        return v;
    }();
    return list;
}

// The decoder list and its extensions as one snapshot. Registering plugins
// swaps in a new one while decode threads may be walking the old, so a
// snapshot, once published, is never freed (there's one swap per run).
namespace
{

struct Registry
{
    std::vector<const Decoder*> list;
    std::set<std::string> extensions;
};

Registry make_registry(const std::vector<const Decoder*>& extra)
{
    Registry r;
    r.list = builtin_decoders();
    r.list.insert(r.list.end() - 1, extra.begin(), extra.end());  // before the catch-all
    for (const Decoder* d : r.list)
        r.extensions.insert(d->extensions.begin(), d->extensions.end());
    return r;
}

std::atomic<const Registry*> g_registry{nullptr};

const Registry& registry()
{
    if (const Registry* r = g_registry.load(std::memory_order_acquire))
        return *r;
    static const Registry builtin = make_registry({});
    return builtin;
}

}  // namespace

const std::vector<const Decoder*>& decoders()
{
    return registry().list;
}

void register_decoders(const std::vector<const Decoder*>& extra)
{
    static std::mutex m;
    std::lock_guard lock(m);
    g_registry.store(new Registry(make_registry(extra)), std::memory_order_release);
}

bool decode_image(const std::string& path, Image& out, std::string& error)
{
    return decode_image_part(path, {}, out, error);
}

bool decode_image_part(const std::string& path, ImagePart part, Image& out, std::string& error)
{
    error.clear();
    const std::string ext = file_extension(path);
    // Videos: GStreamer streams the file; don't read gigabytes for a poster frame.
    for (const Decoder* d : decoders())
        if (d->streaming && claims_extension(*d, ext))
            return try_decode(*d, path, out, error, part);
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
    // By extension, then by content.
    const auto attempt = [&](const std::vector<const Decoder*>& list)
    {
        for (const Decoder* d : list)
        {
            if (ok || tried.count(d) || !claims_extension(*d, ext))
                continue;
            tried.insert(d);
            ok = try_decode(*d, path, out, error, part);
        }
        for (const Decoder* d : list)
        {
            if (ok || tried.count(d) || !d->sniff || !d->sniff(head, head_len))
                continue;
            tried.insert(d);
            ok = try_decode(*d, path, out, error, part);
        }
    };
#ifdef HAVE_RAW
    // A raw in a TIFF can be named .tif (a Phase One's): the raw decoder's,
    // which knows it by its content, before any by its name.
    if (raw_decoder().sniff(head, head_len))
    {
        tried.insert(&raw_decoder());
        ok = try_decode(raw_decoder(), path, out, error, part);
    }
#endif
    attempt(decoders());
    // Nothing built in took it, but a plugin may: wait for them to load
    // (only such files ever wait) and try theirs.
    if (!ok && !plugins::ready())
    {
        plugins::wait();
        attempt(decoders());
    }
    const Decoder* fallback = decoders().back();
    if (!ok && !tried.count(fallback))
        ok = try_decode(*fallback, path, out, error, part);
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

bool decode_is_quick(const std::string& path)
{
    const std::string ext = file_extension(path);
    for (const Decoder* d : decoders())
        if (d->quick && claims_extension(*d, ext))
            return d->quick(path);
    return false;
}

bool decode_preview(const std::string& path, Image& out)
{
    const std::string ext = file_extension(path);
    for (const Decoder* d : decoders())
        if (d->preview && claims_extension(*d, ext))
        {
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (!file_bytes(path, data, size))
                return false;
            MetadataPass meta(path, data, size);
            out = Image{};
            bool ok = false;
            try
            {
                ok = d->preview(path, out);
            }
            catch (const std::length_error&)  // too large: the full decode will say
            {
            }
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
    return registry().extensions.count(ext) != 0;
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
