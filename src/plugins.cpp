// Image format plugins (include/ifritview_plugin.h). Each library found in
// the plugin folders is opened, asked to describe itself, and each of its
// formats becomes a Decoder (and an Encoder where it writes) whose functions
// call into it. The plugin hands pixels over in one of a few layouts, into a
// buffer we allocate: 8-bit ones become the ARGB32 pixels directly, deeper
// ones are kept as the image's SourceData, as a 16-bit PNG's are.
#include "plugins.h"

#include "decoders/decoder.h"
#include "decoders/icc.h"
#include "encoders/encoder.h"
#include "half.h"
#include "platform.h"

#include <ifritview_plugin.h>

#include <glib.h>
#include <glib/gstdio.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <thread>

namespace plugins
{
namespace
{

// A plugin's exports: their types from the header's declarations, and the
// names they're found by.
using info_fn = decltype(&ifrit_plugin_info);
using decode_fn = decltype(&ifrit_decode);
using sniff_fn = decltype(&ifrit_sniff);
using preview_fn = decltype(&ifrit_decode_preview);
using encode_fn = decltype(&ifrit_encode);
const std::string kInfoName = "ifrit_plugin_info";
const std::string kDecodeName = "ifrit_decode";
const std::string kSniffName = "ifrit_sniff";
const std::string kPreviewName = "ifrit_decode_preview";
const std::string kEncodeName = "ifrit_encode";

// -- the libraries ------------------------------------------------------------

#ifdef _WIN32
std::wstring to_wide(const std::string& s)
{
    std::wstring w;
    if (gunichar2* u = g_utf8_to_utf16(s.c_str(), -1, nullptr, nullptr, nullptr))
    {
        w = reinterpret_cast<const wchar_t*>(u);
        g_free(u);
    }
    return w;
}

std::string from_wide(const wchar_t* w)
{
    std::string s;
    if (gchar* u = g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(w), -1, nullptr, nullptr, nullptr))
    {
        s = u;
        g_free(u);
    }
    return s;
}

std::string last_error_text()
{
    const DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string s = text ? from_wide(text) : std::string();
    if (text)
        LocalFree(text);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.'))
        s.pop_back();
    return s + " (error " + std::to_string(code) + ")";
}

void* open_module(const std::string& path, std::string& error)
{
    std::wstring wide = to_wide(path);
    std::replace(wide.begin(), wide.end(), L'/', L'\\');  // LOAD_WITH_ALTERED_SEARCH_PATH wants a Windows path
    // No "missing DLL" message box for a plugin whose dependencies are absent:
    // it's just skipped, with the reason.
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
    // Altered search path: DLLs beside the plugin satisfy its own imports.
    HMODULE m = LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m)
        error = last_error_text();
    SetThreadErrorMode(old_mode, nullptr);
    return m;
}

void close_module(void* m)
{
    FreeLibrary(static_cast<HMODULE>(m));
}

void* symbol(void* m, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m), name));
}

constexpr const char* kSuffix = ".dll";
#else
void* open_module(const std::string& path, std::string& error)
{
    // RTLD_LOCAL: plugins don't see each other's symbols, nor clash.
    void* m = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m)
    {
        const char* e = dlerror();
        error = e ? e : "cannot load";
        if (error.rfind(path + ": ", 0) == 0)  // the path is listed already
            error.erase(0, path.size() + 2);
    }
    return m;
}

void close_module(void* m)
{
    dlclose(m);
}

void* symbol(void* m, const char* name)
{
    return dlsym(m, name);
}

constexpr const char* kSuffix = ".so";
#endif

// -- one plugin format --------------------------------------------------------

struct Plugin;

// A format, its Decoder and Encoder, and the strings they point into. Never
// freed (plugins stay loaded), so the pointers stay good.
struct Format
{
    Plugin* plugin = nullptr;
    uint32_t index = 0;
    FormatSummary summary;
    uint32_t encode_layouts = 0;
    std::deque<std::string> strings;  // a deque: growing it leaves earlier strings where they are
    Decoder decoder{};
    Encoder encoder{};
    const char* keep(const char* s)
    {
        strings.emplace_back(s ? s : "");
        return strings.back().c_str();
    }
};

struct Plugin
{
    void* module = nullptr;
    decode_fn decode = nullptr;
    sniff_fn sniff = nullptr;
    preview_fn preview = nullptr;
    encode_fn encode = nullptr;
    PluginSummary summary;
    std::vector<std::unique_ptr<Format>> formats;
};

// -- decoding -----------------------------------------------------------------

struct LayoutInfo
{
    int channels;
    int bytes;  // per sample
    bool alpha, grey, floating;
};

LayoutInfo layout_info(int layout)
{
    switch (layout)
    {
        case IFRIT_RGBA8: return {4, 1, true, false, false};
        case IFRIT_RGB8: return {3, 1, false, false, false};
        case IFRIT_L8: return {1, 1, false, true, false};
        case IFRIT_LA8: return {2, 1, true, true, false};
        case IFRIT_RGBA16: return {4, 2, true, false, false};
        case IFRIT_RGB16: return {3, 2, false, false, false};
        case IFRIT_L16: return {1, 2, false, true, false};
        case IFRIT_LA16: return {2, 2, true, true, false};
        case IFRIT_RGBA32F: return {4, 4, true, false, true};
        case IFRIT_RGB32F: return {3, 4, false, false, true};
        case IFRIT_L32F: return {1, 4, false, true, true};
        case IFRIT_RGBA16F: return {4, 2, true, false, true};
        default: return {0, 0, false, false, false};
    }
}

// The state of one ifrit_decode call; the host callbacks' context.
struct DecodeCall
{
    Image& out;
    std::string error;
    int layout = 0;
    int width = 0, height = 0;
    std::shared_ptr<SourceData> deep;  // 16-bit and float layouts
    PixelBuffer narrow;                // RGB8: converted from after the call
    FileMetadata stored;
    std::string format_text, info, color_space;
    std::optional<ColorSpace> space;  // named by the plugin, for a profile made for it
    std::string format_name;  // the info pane group when the plugin names none
    explicit DecodeCall(Image& image) : out(image) {}
};

void IFRIT_CALL host_error(void* context, const char* message)
{
    auto* call = static_cast<DecodeCall*>(context);
    if (call->error.empty() && message)
        call->error = message;
}

void* IFRIT_CALL host_pixels(void* context, int32_t width, int32_t height, ifrit_layout layout, size_t* stride)
{
    auto* call = static_cast<DecodeCall*>(context);
    const LayoutInfo li = layout_info(layout);
    if (!li.channels)
    {
        host_error(context, ("plugin asked for an unknown pixel layout " + std::to_string(layout)).c_str());
        return nullptr;
    }
    if (width < 1 || height < 1 || width > 65535 || height > 65535)
    {
        host_error(context, ("plugin asked for an image of " + std::to_string(width) + "×" + std::to_string(height)).c_str());
        return nullptr;
    }
    // What it costs: the plugin's buffer, plus the 8-bit pixels drawn.
    const size_t pixels = size_t(width) * size_t(height);
    const size_t bytes = pixels * li.channels * li.bytes + (li.bytes > 1 ? pixels * 4 : 0);
    const size_t available = platform::available_memory();
    if (available && bytes > available)
    {
        host_error(context, "not enough memory for the image");
        return nullptr;
    }
    call->deep.reset();
    call->narrow = PixelBuffer();
    call->layout = layout;
    call->width = width;
    call->height = height;
    try
    {
        if (li.bytes > 1)
        {
            using S = SourceData::Sample;
            const S sample = layout == IFRIT_RGBA16F ? S::F16 : li.floating ? S::F32 : S::U16;
            call->deep = std::make_shared<SourceData>(sample, li.channels, width, height, li.floating);
            *stride = size_t(width) * call->deep->pixel_bytes();
            return call->deep->data.data();
        }
        if (layout == IFRIT_RGB8)
        {
            call->narrow.resize(pixels * 3);
            *stride = size_t(width) * 3;
            return call->narrow.data();
        }
        // RGBA8 straight into the image (from_rgba8 premultiplies in place);
        // grey goes there too and is spread out from the start of each row.
        call->out.allocate(width, height);
        *stride = size_t(call->out.stride);
        return call->out.pixels.data();
    }
    catch (const std::bad_alloc&)
    {
        host_error(context, "not enough memory for the image");
        return nullptr;
    }
}

void IFRIT_CALL host_metadata(void* context, const char* group, const char* key, const char* value)
{
    auto* call = static_cast<DecodeCall*>(context);
    if (key && value)
        call->out.add(group && *group ? group : call->format_name, key, value);
}

void IFRIT_CALL host_blob(void* context, ifrit_blob_kind kind, const void* data, size_t size)
{
    auto* call = static_cast<DecodeCall*>(context);
    if (!data || !size)
        return;
    const auto* p = static_cast<const uint8_t*>(data);
    std::vector<uint8_t>* into = nullptr;
    switch (kind)
    {
        case IFRIT_BLOB_EXIF:
            if (size >= 6 && std::memcmp(p, "Exif\0\0", 6) == 0)
            {
                p += 6;
                size -= 6;
            }
            into = &call->stored.exif;
            break;
        case IFRIT_BLOB_XMP: into = &call->stored.xmp; break;
        case IFRIT_BLOB_IPTC: into = &call->stored.iptc; break;
        case IFRIT_BLOB_ICC: into = &call->stored.icc; break;
        default: return;
    }
    into->assign(p, p + size);
}

void IFRIT_CALL host_color_space(void* context, ifrit_color_space space, const ifrit_chromaticities* custom)
{
    auto* call = static_cast<DecodeCall*>(context);
    switch (space)
    {
        case IFRIT_SPACE_SRGB: call->space = ColorSpace::srgb(); break;
        case IFRIT_SPACE_DISPLAY_P3: call->space = ColorSpace::display_p3(); break;
        case IFRIT_SPACE_ADOBE_RGB: call->space = ColorSpace::adobe_rgb(); break;
        case IFRIT_SPACE_REC2020: call->space = ColorSpace::rec2020(); break;
        case IFRIT_SPACE_PROPHOTO: call->space = ColorSpace::prophoto(); break;
        case IFRIT_SPACE_CUSTOM:
            if (custom && custom->red_y > 0 && custom->green_y > 0 && custom->blue_y > 0 && custom->white_y > 0)
            {
                const auto t = custom->gamma <= 0 ? ColorSpace::Transfer::SRGB
                               : custom->gamma == 1 ? ColorSpace::Transfer::Linear
                                                    : ColorSpace::Transfer::Gamma;
                call->space = ColorSpace::from_primaries({custom->red_x, custom->red_y}, {custom->green_x, custom->green_y},
                                                         {custom->blue_x, custom->blue_y}, {custom->white_x, custom->white_y}, t,
                                                         custom->gamma, "Custom RGB");
            }
            break;
        default: break;
    }
}

void IFRIT_CALL host_describe(void* context, ifrit_text_kind kind, const char* text)
{
    auto* call = static_cast<DecodeCall*>(context);
    if (!text)
        return;
    if (kind == IFRIT_TEXT_FORMAT)
        call->format_text = text;
    else if (kind == IFRIT_TEXT_INFO)
        call->info = text;
    else if (kind == IFRIT_TEXT_COLOR_SPACE)
        call->color_space = text;
}

// The plugin's pixels into the image: 8-bit layouts become ARGB32, deeper
// ones its source, which renders them.
void finish_pixels(DecodeCall& call)
{
    Image& out = call.out;
    const LayoutInfo li = layout_info(call.layout);
    if (call.deep)
    {
        out.set_source(call.deep);
        // An alpha channel that's opaque throughout isn't transparency.
        const SourceData& s = *call.deep;
        bool opaque = true;
        if (li.alpha)
            for (int y = 0; y < s.height && opaque; ++y)
                for (int x = 0; x < s.width; ++x)
                    if (s.value(s.pixel(x, y), s.channels - 1) < 1.0f)
                    {
                        opaque = false;
                        break;
                    }
        out.has_alpha = li.alpha && !opaque;
    }
    else if (call.layout == IFRIT_RGB8)
    {
        out.allocate(call.width, call.height);
        out.from_rgb8(call.narrow.data(), call.width * 3);
        call.narrow = PixelBuffer();
        out.has_alpha = false;
    }
    else
    {
        if (li.grey)
        {
            // Spread Y (or Y, A) out to R, G, B, A in place, back to front so
            // nothing is overwritten before it's read.
            for (int y = 0; y < out.height; ++y)
            {
                uint8_t* row = out.pixels.data() + size_t(y) * out.stride;
                for (int x = out.width - 1; x >= 0; --x)
                {
                    const uint8_t v = row[size_t(x) * li.channels];
                    const uint8_t a = li.alpha ? row[size_t(x) * 2 + 1] : 255;
                    uint8_t* d = row + size_t(x) * 4;
                    d[0] = d[1] = d[2] = v;
                    d[3] = a;
                }
            }
        }
        bool opaque = true;
        if (li.alpha)
            for (int y = 0; y < out.height && opaque; ++y)
            {
                const uint8_t* row = out.pixels.data() + size_t(y) * out.stride;
                for (int x = 0; x < out.width; ++x)
                    if (row[size_t(x) * 4 + 3] != 255)
                    {
                        opaque = false;
                        break;
                    }
            }
        out.from_rgba8(out.pixels.data(), out.stride);
        out.has_alpha = !opaque;
    }
    out.bits_per_sample = li.bytes * 8;
    static const char* const names[] = {"", "L", "LA", "RGB", "RGBA"};
    out.pixel_format = PixelFormat::packed(li.grey ? PixelFormat::Model::L : PixelFormat::Model::RGB, names[li.channels], li.bytes * 8);
    if (li.floating)
        out.pixel_format.sample = PixelFormat::Sample::Float;
}

// Metadata the plugin handed over: kept for saving, and read for display.
void finish_metadata(DecodeCall& call)
{
    Image& out = call.out;
    FileMetadata& m = call.stored;
    if (call.space)
    {
        // Floats are linear whatever the space; their 8-bit rendering, which
        // is what's saved, has its primaries and sRGB's curve.
        const bool floating = call.layout && layout_info(call.layout).floating;
        out.declared_space = floating ? call.space->with_transfer(ColorSpace::Transfer::SRGB) : *call.space;
        out.color_space = out.declared_space->name;
    }
    if (!call.color_space.empty())
        out.color_space = call.color_space;
    if (!m.icc.empty())
    {
        const std::string name = icc_profile_description(m.icc.data(), m.icc.size());
        if (!name.empty())
            out.color_space = name;
    }
#ifdef HAVE_EXIF
    if (!m.exif.empty())
    {
        const ExifSummary exif = read_exif_blob(m.exif.data(), m.exif.size(), out);
        if (call.info.empty())
            call.info = exif.camera;
        if (out.color_space.empty())
            out.color_space = exif.color_space;
        out.apply_orientation(exif.orientation);
    }
#endif
#ifdef HAVE_IPTC
    if (!m.iptc.empty())
        read_iptc_blob(m.iptc.data(), m.iptc.size(), out);
#endif
    if (!m.empty())
        out.stored = std::make_shared<const FileMetadata>(std::move(m));
    if (!call.format_text.empty())
        out.format = call.format_text;
    out.info = call.info;
}

bool run_decode(const Format& f, decode_fn fn, const std::string& path, Image& out, std::string& error)
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!file_bytes(path, data, size))
    {
        error = "cannot read the file";
        return false;
    }
    DecodeCall call(out);
    call.format_name = f.summary.name;
    ifrit_decode_host host{};
    host.struct_size = sizeof host;
    host.context = &call;
    host.pixels = host_pixels;
    host.error = host_error;
    host.metadata = host_metadata;
    host.blob = host_blob;
    host.describe = host_describe;
    host.color_space = host_color_space;
    const ifrit_result r = fn(f.index, data, size, &host);
    if (r == IFRIT_OK && !call.layout)
        call.error = "the plugin returned no image";
    if (r != IFRIT_OK || !call.error.empty())
    {
        error = !call.error.empty() ? call.error : r == IFRIT_UNSUPPORTED ? "not a file this plugin reads" : "the plugin failed without saying why";
        out = Image{};
        return false;
    }
    finish_pixels(call);
    finish_metadata(call);
    if (out.format.empty())
        out.format = f.summary.name;
    return true;
}

// -- encoding -----------------------------------------------------------------

inline double srgb_decode(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

inline double srgb_encode(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
}

// Which of the encoder's layouts suits the image best: its depth (16-bit for
// 16-bit samples, float for linear ones), its alpha, its colour.
int choose_layout(const Image& image, uint32_t accepted)
{
    if (!accepted)
        accepted = IFRIT_LAYOUT_BIT(IFRIT_RGBA8);
    const SourceData* s = image.source.get();
    const int depth = !s ? 1 : s->linear || s->sample == SourceData::Sample::F16 || s->sample == SourceData::Sample::F32 ? 4 : 2;
    const bool grey = s && s->channels <= 2;
    int best = 0, best_score = -1;
    for (int l = 1; l < IFRIT_LAYOUT_COUNT; ++l)
    {
        if (!(accepted & IFRIT_LAYOUT_BIT(l)))
            continue;
        const LayoutInfo li = layout_info(l);
        const int d = li.floating ? 4 : li.bytes;
        // Depth matters most, then keeping the alpha and the colour, then not
        // carrying channels that aren't needed.
        int score = d == depth ? 300 : d > depth ? 200 : 100 + d;
        score += image.has_alpha ? (li.alpha ? 20 : 0) : (li.alpha ? 0 : 2);
        score += grey ? (li.grey ? 1 : 0) : (li.grey ? 0 : 10);
        if (score > best_score)
        {
            best = l;
            best_score = score;
        }
    }
    return best;
}

// The image as `layout`, straight alpha, rows packed. 8-bit layouts come
// from the pixels as drawn; deeper ones from the source samples when there
// are some, so a 16-bit image is written at 16 bits.
std::vector<uint8_t> convert_for_encoder(const Image& image, int layout, size_t& stride)
{
    const LayoutInfo li = layout_info(layout);
    const int w = image.width, h = image.height;
    stride = size_t(w) * li.channels * li.bytes;
    std::vector<uint8_t> out(stride * h);
    const SourceData* s = image.source && image.source->width == w && image.source->height == h ? image.source.get() : nullptr;
    const std::vector<uint8_t> rgba = li.bytes == 1 || !s ? to_rgba8(image) : std::vector<uint8_t>();
    const auto luma = [](double r, double g, double b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; };
    for (int y = 0; y < h; ++y)
    {
        uint8_t* d = out.data() + size_t(y) * stride;
        for (int x = 0; x < w; ++x)
        {
            // The pixel as R, G, B, A, 0..1: display-encoded for the integer
            // layouts, linear for the float ones.
            double px[4];
            if (!rgba.empty())
            {
                const uint8_t* p = rgba.data() + (size_t(y) * w + x) * 4;
                for (int c = 0; c < 4; ++c)
                    px[c] = p[c] / 255.0;
                if (li.floating)
                    for (int c = 0; c < 3; ++c)
                        px[c] = srgb_decode(px[c]);
            }
            else
            {
                const uint8_t* p = s->pixel(x, y);
                const int n = s->channels;
                const double v0 = s->value(p, 0);
                px[0] = v0;
                px[1] = n >= 3 ? s->value(p, 1) : v0;
                px[2] = n >= 3 ? s->value(p, 2) : v0;
                px[3] = n == 2 ? s->value(p, 1) : n == 4 ? s->value(p, 3) : 1.0;
                if (li.floating != s->linear)
                    for (int c = 0; c < 3; ++c)
                        px[c] = li.floating ? srgb_decode(px[c]) : srgb_encode(px[c]);
            }
            double ch[4];
            int n = 0;
            if (li.grey)
                ch[n++] = luma(px[0], px[1], px[2]);
            else
                for (int c = 0; c < 3; ++c)
                    ch[n++] = px[c];
            if (li.alpha)
                ch[n++] = px[3];
            for (int c = 0; c < n; ++c, d += li.bytes)
            {
                if (layout == IFRIT_RGBA16F)
                {
                    const uint16_t v = float_to_half(float(ch[c]));
                    std::memcpy(d, &v, 2);
                }
                else if (li.floating)
                {
                    const float v = float(ch[c]);
                    std::memcpy(d, &v, 4);
                }
                else if (li.bytes == 2)
                {
                    // Integer samples go through exactly: v / 65535 and back.
                    const uint16_t v = uint16_t(std::lround(std::clamp(ch[c], 0.0, 1.0) * 65535.0));
                    std::memcpy(d, &v, 2);
                }
                else
                    *d = uint8_t(std::lround(std::clamp(ch[c], 0.0, 1.0) * 255.0));
            }
        }
    }
    return out;
}

struct EncodeCall
{
    FILE* file = nullptr;
    bool failed = false;
    std::string error;
};

int32_t IFRIT_CALL host_write(void* context, const void* data, size_t size)
{
    auto* call = static_cast<EncodeCall*>(context);
    if (call->failed || (size && std::fwrite(data, 1, size, call->file) != size))
    {
        call->failed = true;
        return 0;
    }
    return 1;
}

void IFRIT_CALL host_encode_error(void* context, const char* message)
{
    auto* call = static_cast<EncodeCall*>(context);
    if (call->error.empty() && message)
        call->error = message;
}

bool run_encode(const Format& f, const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    const int layout = choose_layout(image, f.encode_layouts);
    size_t stride = 0;
    const std::vector<uint8_t> pixels = convert_for_encoder(image, layout, stride);
    ifrit_encode_image in{};
    in.struct_size = sizeof in;
    in.width = image.width;
    in.height = image.height;
    in.layout = layout;
    in.stride = stride;
    in.pixels = pixels.data();
    in.has_alpha = image.has_alpha;
    if (options.metadata)
    {
        const FileMetadata& m = *options.metadata;
        const auto put = [](const std::vector<uint8_t>& v, const uint8_t*& p, size_t& n)
        {
            p = v.empty() ? nullptr : v.data();
            n = v.size();
        };
        // The colour profile goes to every format, as it's part of what the
        // pixels mean; the rest only to formats that keep metadata.
        put(m.icc, in.icc, in.icc_size);
        if (f.summary.metadata)
        {
            put(m.exif, in.exif, in.exif_size);
            put(m.xmp, in.xmp, in.xmp_size);
            put(m.iptc, in.iptc, in.iptc_size);
        }
    }
    std::vector<int32_t> values;
    for (const EncodeSetting& s : f.encoder.settings)
        values.push_back(std::clamp(f.encoder.value(options, s.key), s.min, s.max));

    // Into a temporary file beside the target, renamed over it on success.
    const std::string temp = path + ".ifrit-partial";
    EncodeCall call;
    call.file = platform::fopen(temp, "wb");
    if (!call.file)
    {
        error = "cannot create the file";
        return false;
    }
    ifrit_encode_host host{};
    host.struct_size = sizeof host;
    host.context = &call;
    host.write = host_write;
    host.error = host_encode_error;
    const ifrit_result r = f.plugin->encode(f.index, &in, values.data(), uint32_t(values.size()), &host);
    const bool closed = std::fclose(call.file) == 0;
    if (r == IFRIT_OK && !call.failed && closed)
    {
        g_remove(path.c_str());  // Windows' rename won't replace a file
        if (g_rename(temp.c_str(), path.c_str()) == 0)
            return true;
        error = "cannot replace the file";
    }
    else if (call.failed || !closed)
        error = "cannot write the file (disk full?)";
    else
        error = !call.error.empty() ? call.error : "the plugin failed without saying why";
    g_remove(temp.c_str());
    return false;
}

// -- loading ------------------------------------------------------------------

std::mutex g_mutex;  // the lists below, and the done callbacks
std::once_flag g_once;
std::atomic<bool> g_ready{false};
bool g_started = false;
std::vector<std::function<void()>> g_done;
// Never freed, not even at exit: a decode thread may still be running a
// plugin's decoder while the statics go.
std::vector<std::unique_ptr<Plugin>>& g_plugins = *new std::vector<std::unique_ptr<Plugin>>;
std::vector<Failure> g_failures;
std::vector<const Encoder*> g_encoders;

// A struct of the plugin's, copied into ours as far as both know it: newer
// fields it has are ignored, ones it lacks stay zero.
template <typename T>
T read_struct(const void* p)
{
    T t{};
    uint32_t size = 0;
    std::memcpy(&size, p, sizeof size);
    std::memcpy(&t, p, std::min<size_t>(size, sizeof t));
    t.struct_size = size;
    return t;
}

std::vector<std::string> split(const char* text, char sep)
{
    std::vector<std::string> parts;
    if (!text)
        return parts;
    std::string cur;
    for (const char* c = text;; ++c)
    {
        if (*c == sep || !*c)
        {
            if (!cur.empty())
                parts.push_back(cur);
            cur.clear();
            if (!*c)
                break;
        }
        else
            cur += *c;
    }
    return parts;
}

std::string clean_extension(std::string e)
{
    e.erase(std::remove_if(e.begin(), e.end(), [](unsigned char c) { return std::isspace(c) || c == '.'; }), e.end());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

// Builds a format's Decoder and Encoder; false (with `why`) if its
// description doesn't hold together.
bool make_format(Plugin& p, uint32_t index, const ifrit_format& d, Format& f, std::string& why)
{
    f.plugin = &p;
    f.index = index;
    if (d.struct_size < offsetof(ifrit_format, flags) + sizeof d.flags)
    {
        why = "format " + std::to_string(index) + ": description too short";
        return false;
    }
    if (!d.name || !*d.name)
    {
        why = "format " + std::to_string(index) + " has no name";
        return false;
    }
    f.summary.name = d.name;
    f.summary.description = d.description ? d.description : "";
    for (const std::string& e : split(d.extensions, ';'))
    {
        const std::string clean = clean_extension(e);
        if (!clean.empty() && std::find(f.summary.extensions.begin(), f.summary.extensions.end(), clean) == f.summary.extensions.end())
            f.summary.extensions.push_back(clean);
    }
    f.summary.decode = (d.flags & IFRIT_FORMAT_DECODE) != 0;
    f.summary.encode = (d.flags & IFRIT_FORMAT_ENCODE) != 0 && p.encode && !f.summary.extensions.empty();
    f.summary.metadata = (d.flags & IFRIT_FORMAT_METADATA) != 0;
    if (!f.summary.decode && !f.summary.encode)
    {
        why = std::string("format ") + d.name + (d.flags & IFRIT_FORMAT_ENCODE ? " encodes, but the plugin doesn't export ifrit_encode" : " neither decodes nor encodes");
        return false;
    }
    Format* fp = &f;
    f.decoder.name = f.keep(d.name);
    if (f.summary.decode)
    {
        f.decoder.extensions = f.summary.extensions;
        f.decoder.decode = [fp](const std::string& path, Image& out, std::string& error) { return run_decode(*fp, fp->plugin->decode, path, out, error); };
        if (p.sniff)
            f.decoder.sniff = [fp](const uint8_t* head, size_t len) { return fp->plugin->sniff(fp->index, head, len) != 0; };
        if (p.preview)
            f.decoder.preview = [fp](const std::string& path, Image& out)
            {
                std::string ignored;
                return run_decode(*fp, fp->plugin->preview, path, out, ignored);
            };
    }
    if (f.summary.encode)
    {
        f.encode_layouts = d.encode_layouts;
        f.encoder.name = f.decoder.name;
        f.encoder.extension = f.keep(f.summary.extensions.front().c_str());
        f.encoder.metadata = f.summary.metadata;
        f.encoder.encode = [fp](const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
        { return run_encode(*fp, image, path, options, error); };
        const size_t step = d.settings ? d.settings[0].struct_size : 0;
        if (d.setting_count && step < offsetof(ifrit_setting, initial) + sizeof(int32_t))
        {
            why = std::string("format ") + d.name + ": settings missing or too short";
            return false;
        }
        for (uint32_t i = 0; i < d.setting_count; ++i)
        {
            // Stepped by the first's size, as the formats are.
            const ifrit_setting s = read_struct<ifrit_setting>(reinterpret_cast<const uint8_t*>(d.settings) + size_t(i) * step);
            if (!s.key || !*s.key)
            {
                why = std::string("format ") + d.name + ": setting " + std::to_string(i) + " has no key";
                return false;
            }
            EncodeSetting e{f.keep(s.key), f.keep(s.label && *s.label ? s.label : s.key), EncodeSetting::Kind::Range};
            e.min = s.min;
            e.max = s.max;
            if (s.kind == IFRIT_SETTING_TOGGLE)
            {
                e.kind = EncodeSetting::Kind::Toggle;
                e.min = 0;
                e.max = 1;
            }
            else if (s.kind == IFRIT_SETTING_CHOICE)
            {
                e.kind = EncodeSetting::Kind::Choice;
                for (const std::string& c : split(s.choices, '|'))
                    e.choices.push_back(f.keep(c.c_str()));
                if (e.choices.empty())
                {
                    why = std::string("format ") + d.name + ": choice setting " + s.key + " has no choices";
                    return false;
                }
                e.min = 0;
                e.max = int(e.choices.size()) - 1;
            }
            else if (s.kind != IFRIT_SETTING_RANGE || s.min > s.max)
            {
                why = std::string("format ") + d.name + ": setting " + s.key + " has an unknown kind or an empty range";
                return false;
            }
            e.initial = std::clamp(s.initial, e.min, e.max);
            e.note = s.note ? f.keep(s.note) : nullptr;
            e.off_when = s.off_when ? f.keep(s.off_when) : nullptr;
            f.encoder.settings.push_back(std::move(e));
        }
    }
    return true;
}

// Opens one library; a Plugin, or null with the reason.
std::unique_ptr<Plugin> load_plugin(const std::string& path, std::string& why)
{
    void* module = open_module(path, why);
    if (!module)
        return nullptr;
    auto p = std::make_unique<Plugin>();
    p->module = module;
    p->summary.path = path;
    const auto fail = [&](std::string reason)
    {
        why = std::move(reason);
        close_module(module);
        return nullptr;
    };
    auto info = reinterpret_cast<info_fn>(symbol(module, kInfoName.c_str()));
    p->decode = reinterpret_cast<decode_fn>(symbol(module, kDecodeName.c_str()));
    if (!info)
        return fail("not an IfritView plugin (no " + kInfoName + ")");
    if (!p->decode)
        return fail("required export " + kDecodeName + " is missing");
    p->sniff = reinterpret_cast<sniff_fn>(symbol(module, kSniffName.c_str()));
    p->preview = reinterpret_cast<preview_fn>(symbol(module, kPreviewName.c_str()));
    p->encode = reinterpret_cast<encode_fn>(symbol(module, kEncodeName.c_str()));
    p->summary.sniff = p->sniff != nullptr;
    p->summary.preview = p->preview != nullptr;

    const ifrit_plugin* raw = info(IFRIT_PLUGIN_API_VERSION);
    if (!raw)
        return fail(kInfoName + " declined to load (it may need a newer IfritView)");
    const ifrit_plugin d = read_struct<ifrit_plugin>(raw);
    if (d.struct_size < offsetof(ifrit_plugin, api_version) + sizeof d.api_version)
        return fail("plugin description too short");
    p->summary.api_version = d.api_version;
    if (d.api_version < 1 || d.api_version > IFRIT_PLUGIN_API_VERSION)
        return fail("built for plugin API version " + std::to_string(d.api_version) + "; this IfritView has version " +
                    std::to_string(IFRIT_PLUGIN_API_VERSION));
    if (d.struct_size < offsetof(ifrit_plugin, format_count) + sizeof d.format_count)
        return fail("plugin description too short");
    if (!d.name || !*d.name)
        return fail("plugin has no name");
    p->summary.name = d.name;
    p->summary.version = d.version ? d.version : "";
    if (!d.formats || d.format_count == 0 || d.format_count > 256)
        return fail("plugin describes " + std::to_string(d.format_count) + " formats");
    const auto* bytes = reinterpret_cast<const uint8_t*>(d.formats);
    const size_t step = d.formats[0].struct_size;
    if (step < offsetof(ifrit_format, flags) + sizeof(uint32_t))
        return fail("format description too short");
    for (uint32_t i = 0; i < d.format_count; ++i)
    {
        auto f = std::make_unique<Format>();
        std::string reason;
        if (!make_format(*p, i, read_struct<ifrit_format>(bytes + size_t(i) * step), *f, reason))
            return fail(reason);
        p->summary.formats.push_back(f->summary);
        p->formats.push_back(std::move(f));
    }
    return p;
}

std::string canonical(const std::string& path)
{
    gchar* c = g_canonicalize_filename(path.c_str(), nullptr);
    std::string s = c;
    g_free(c);
    return s;
}

void load_all()
{
    const gint64 t0 = g_get_monotonic_time();
    std::vector<std::unique_ptr<Plugin>> plugins;
    std::vector<Failure> failures;
    std::set<std::string> seen;  // file names, so the first folder's copy wins
    for (const std::string& dir : search_paths())
    {
        GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
        if (!d)
            continue;
        std::vector<std::string> names;
        while (const gchar* name = g_dir_read_name(d))
        {
            const size_t n = std::strlen(name), k = std::strlen(kSuffix);
            if (n > k && g_ascii_strcasecmp(name + n - k, kSuffix) == 0)  // Windows: "PLUGIN.DLL" too
                names.push_back(name);
        }
        g_dir_close(d);
        std::sort(names.begin(), names.end());
        for (const std::string& name : names)
        {
            gchar* full = g_build_filename(dir.c_str(), name.c_str(), nullptr);
            const std::string path = full;
            g_free(full);
#ifdef _WIN32
            std::string key = name;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#else
            const std::string& key = name;
#endif
            if (!seen.insert(key).second)
            {
                failures.push_back({path, "overridden by a plugin of the same name in an earlier folder"});
                continue;
            }
            std::string why;
            if (auto p = load_plugin(path, why))
                plugins.push_back(std::move(p));
            else
                failures.push_back({path, why});
        }
    }

    std::vector<const Decoder*> decoders;
    std::vector<const Encoder*> encoders;
    for (const auto& p : plugins)
        for (const auto& f : p->formats)
        {
            if (f->summary.decode)
                decoders.push_back(&f->decoder);
            if (f->summary.encode)
                encoders.push_back(&f->encoder);
        }
    register_decoders(decoders);
    if (g_getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "plugins: %zu loaded, %zu skipped, %.1f ms\n", plugins.size(), failures.size(), (g_get_monotonic_time() - t0) / 1000.0);
    std::lock_guard lock(g_mutex);
    g_plugins = std::move(plugins);
    g_failures = std::move(failures);
    g_encoders = std::move(encoders);
}

void finish_loading()
{
    std::call_once(g_once, load_all);
    std::vector<std::function<void()>> done;
    {
        std::lock_guard lock(g_mutex);
        g_ready = true;
        done.swap(g_done);
    }
    for (auto& fn : done)
        fn();
}

}  // namespace

std::vector<std::string> search_paths()
{
    std::vector<std::string> dirs;
    const auto add = [&dirs](const std::string& d)
    {
        if (d.empty())
            return;
        const std::string c = canonical(d);
        if (std::find(dirs.begin(), dirs.end(), c) == dirs.end())
            dirs.push_back(c);
    };
    if (const char* env = g_getenv("IFRITVIEW_PLUGIN_PATH"))
        for (const std::string& d : split(env, G_SEARCHPATH_SEPARATOR))
            add(d);
#ifdef _WIN32
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        add(from_wide(local) + "\\IfritView\\plugins");
    CoTaskMemFree(local);
#else
    // There's no XDG variable for per-user libraries; ~/.local/lib is where
    // file-hierarchy(7) puts them, mirroring the system's lib/ifritview.
    add(std::string(g_get_home_dir()) + "/.local/lib/ifritview/plugins");
#endif
    gchar* exe_dir = g_path_get_dirname(platform::executable_path().c_str());
    const std::string exe = exe_dir;
    g_free(exe_dir);
    add(exe + G_DIR_SEPARATOR_S "plugins");
#ifdef IFRIT_FORMAT_PLUGIN_DIR
    // Installed: lib/<multiarch>/ifritview/plugins under the prefix, found
    // from the executable so an unpacked tarball works too, and where the
    // build was configured to install.
    add(exe + "/../" IFRIT_FORMAT_PLUGIN_DIR);
#endif
#ifdef IFRIT_FORMAT_PLUGIN_DIR_FULL
    add(IFRIT_FORMAT_PLUGIN_DIR_FULL);
#endif
#ifndef _WIN32
    add("/usr/local/lib/ifritview/plugins");
#endif
    return dirs;
}

void load_async(std::function<void()> done)
{
    {
        std::lock_guard lock(g_mutex);
        if (!g_ready)
        {
            if (done)
                g_done.push_back(std::move(done));
            if (g_started)
                return;
            g_started = true;
            std::thread(
                []
                {
                    platform::lower_thread_priority();
                    finish_loading();
                })
                .detach();
            return;
        }
    }
    if (done)
        done();
}

void wait()
{
    if (!g_ready)
        finish_loading();  // waits on the loading thread's call_once, or loads here
}

bool ready()
{
    return g_ready;
}

std::vector<PluginSummary> loaded()
{
    std::lock_guard lock(g_mutex);
    std::vector<PluginSummary> list;
    for (const auto& p : g_plugins)
        list.push_back(p->summary);
    return list;
}

std::vector<Failure> failures()
{
    std::lock_guard lock(g_mutex);
    return g_failures;
}

std::vector<std::string> extensions()
{
    std::vector<std::string> all;
    for (const PluginSummary& p : loaded())
        for (const FormatSummary& f : p.formats)
            if (f.decode)
                for (const std::string& e : f.extensions)
                    if (std::find(all.begin(), all.end(), e) == all.end())
                        all.push_back(e);
    return all;
}

const std::vector<const Encoder*>& encoders()
{
    wait();
    return g_encoders;  // written once, before g_ready
}

int print_report()
{
    wait();
    std::printf("Plugin API version %d. Folders searched, in order:\n", IFRIT_PLUGIN_API_VERSION);
    for (const std::string& d : search_paths())
        std::printf("  %s%s\n", d.c_str(), g_file_test(d.c_str(), G_FILE_TEST_IS_DIR) ? "" : "  (absent)");
    const std::vector<PluginSummary> list = loaded();
    std::printf("\nLoaded (%zu):\n", list.size());
    for (const PluginSummary& p : list)
    {
        std::printf("  %s %s  (API %u)\n    %s\n", p.name.c_str(), p.version.c_str(), p.api_version, p.path.c_str());
        for (const FormatSummary& f : p.formats)
        {
            std::string exts, features;
            for (const std::string& e : f.extensions)
                exts += (exts.empty() ? "." : " .") + e;
            const auto feature = [&features](bool on, const char* what)
            {
                if (on)
                    features += (features.empty() ? "" : ", ") + std::string(what);
            };
            feature(f.decode, "decode");
            feature(f.decode && p.sniff, "sniff");
            feature(f.decode && p.preview, "preview");
            feature(f.encode, "encode");
            feature(f.encode && f.metadata, "metadata");
            std::printf("    %-12s %-20s %s\n", f.name.c_str(), exts.c_str(), features.c_str());
            if (!f.description.empty())
                std::printf("    %-12s %s\n", "", f.description.c_str());
        }
    }
    const std::vector<Failure> failed = failures();
    if (!failed.empty())
    {
        std::printf("\nNot loaded (%zu):\n", failed.size());
        for (const Failure& f : failed)
            std::printf("  %s\n    %s\n", f.path.c_str(), f.reason.c_str());
    }
    return 0;
}

}  // namespace plugins
