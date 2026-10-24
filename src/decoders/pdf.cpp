// PDF through poppler-glib: the first page rendered to pixels at 150 dpi,
// over white, and kept open: the view renders it again at the size it's
// shown, and this rendering stands in until that's ready (and is what
// saving and thumbnails get). Page navigation can come later.
//
// Poppler is optional at runtime: built against its headers, but dlopened
// rather than linked (it drags in a few dozen packages). Without it, PDFs
// aren't part of the series.
#include "decoder.h"
#include "../i18n.h"

#include "../platform.h"

#include <cairo.h>
#include <glib.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <poppler.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#define IFRIT_POPPLER_FUNCTIONS(X)             \
    X(poppler_document_new_from_bytes)         \
    X(poppler_document_get_n_pages)            \
    X(poppler_document_get_page)               \
    X(poppler_document_get_title)              \
    X(poppler_document_get_author)             \
    X(poppler_document_get_subject)            \
    X(poppler_document_get_keywords)           \
    X(poppler_document_get_creator)            \
    X(poppler_document_get_producer)           \
    X(poppler_document_get_pdf_version_string) \
    X(poppler_page_get_size)                   \
    X(poppler_page_render)

namespace
{

const char* const kPopplerLibrary = IFRIT_LIBRARY_NAME("libpoppler-glib.so.8", "libpoppler-glib-8.dll");

struct PopplerApi
{
#define IFRIT_POPPLER_POINTER(name) decltype(&::name) name = nullptr;
    IFRIT_POPPLER_FUNCTIONS(IFRIT_POPPLER_POINTER)
#undef IFRIT_POPPLER_POINTER
};
PopplerApi g_poppler;

bool load_poppler()
{
    if (g_getenv("IFRIT_NO_PDF"))
        return false;
    std::string error;
    void* lib = platform::open_library(kPopplerLibrary, &error);
    if (!lib)
    {
        if (g_getenv("IFRIT_TRACE"))
            std::fprintf(stderr, "pdf: %s\n", error.c_str());
        return false;
    }
    PopplerApi api;
    bool ok = true;
#define IFRIT_POPPLER_RESOLVE(name)                                                               \
    if (!(api.name = reinterpret_cast<decltype(api.name)>(platform::library_symbol(lib, #name)))) \
        ok = false;
    IFRIT_POPPLER_FUNCTIONS(IFRIT_POPPLER_RESOLVE)
#undef IFRIT_POPPLER_RESOLVE
    if (!ok)
        return false;  // an incompatible poppler; leave it loaded, it's harmless
    g_poppler = api;
    return true;
}

// From here on the poppler_* names call through the table.
#define poppler_document_new_from_bytes g_poppler.poppler_document_new_from_bytes
#define poppler_document_get_n_pages g_poppler.poppler_document_get_n_pages
#define poppler_document_get_page g_poppler.poppler_document_get_page
#define poppler_document_get_title g_poppler.poppler_document_get_title
#define poppler_document_get_author g_poppler.poppler_document_get_author
#define poppler_document_get_subject g_poppler.poppler_document_get_subject
#define poppler_document_get_keywords g_poppler.poppler_document_get_keywords
#define poppler_document_get_creator g_poppler.poppler_document_get_creator
#define poppler_document_get_producer g_poppler.poppler_document_get_producer
#define poppler_document_get_pdf_version_string g_poppler.poppler_document_get_pdf_version_string
#define poppler_page_get_size g_poppler.poppler_page_get_size
#define poppler_page_render g_poppler.poppler_page_render

// Quick to make, for the moment before the view's own; a page larger than
// A4 renders at less, its long side at most kMaxSide pixels.
constexpr double kDpi = 150.0;
constexpr double kMaxSide = 4096;

// White, then the page: `scale` pixels to the point, from (x, y) in them.
void render_page(PopplerPage* page, double scale, double x, double y, int w, int h, uint8_t* pixels, int stride)
{
    cairo_surface_t* surface = cairo_image_surface_create_for_data(pixels, CAIRO_FORMAT_ARGB32, w, h, stride);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_translate(cr, -x, -y);
    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
}

// The open page, for renderings at other sizes. Poppler's objects aren't
// for two threads at once.
class PdfPage : public VectorSource
{
public:
    PdfPage(PopplerDocument* doc, PopplerPage* page, double page_dpi) : doc_(doc), page_(page) { dpi = page_dpi; }
    ~PdfPage() override
    {
        g_object_unref(page_);
        g_object_unref(doc_);
    }
    bool render(double scale, double x, double y, int out_w, int out_h, PixelBuffer& out) const override
    {
        if (out_w <= 0 || out_h <= 0)
            return false;
        out = PixelBuffer(size_t(out_w) * out_h * 4);
        std::lock_guard<std::mutex> lock(mutex_);
        render_page(page_, scale * dpi / 72.0, x * scale, y * scale, out_w, out_h, out.data(), out_w * 4);
        return true;
    }

private:
    PopplerDocument* doc_;
    PopplerPage* page_;
    mutable std::mutex mutex_;
};

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 5 && std::memcmp(h, "%PDF-", 5) == 0;
}

void add_info(Image& out, const char* key, gchar* value)
{
    if (value && *value)
        out.add("Document", key, value);
    g_free(value);
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    if (!pdf_supported())  // first PDF: load poppler now
    {
        error = "poppler could not be loaded";
        return false;
    }
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (!file_bytes(path, bytes, size))
    {
        error = "cannot read file";
        return false;
    }
    GBytes* gbytes = g_bytes_new(bytes, size);  // a copy: the document outlives the file's bytes
    GError* err = nullptr;
    PopplerDocument* doc = poppler_document_new_from_bytes(gbytes, nullptr, &err);
    g_bytes_unref(gbytes);
    if (!doc)
    {
        error = err ? err->message : "cannot open PDF";
        if (err)
            g_error_free(err);
        return false;
    }
    const int pages = poppler_document_get_n_pages(doc);
    PopplerPage* page = pages > 0 ? poppler_document_get_page(doc, 0) : nullptr;
    if (!page)
    {
        error = "PDF has no pages";
        g_object_unref(doc);
        return false;
    }
    double pw = 0, ph = 0;
    poppler_page_get_size(page, &pw, &ph);  // points
    const double dpi = std::min(kDpi, kMaxSide * 72.0 / std::max({pw, ph, 1.0}));
    const double scale = dpi / 72.0;
    out.allocate(std::max(1, int(std::ceil(pw * scale))), std::max(1, int(std::ceil(ph * scale))));
    render_page(page, scale, 0, 0, out.width, out.height, out.pixels.data(), out.stride);

    out.format = "PDF";
    out.pixel_format.model = PixelFormat::Model::Vector;
    out.pixel_format.notes.push_back("rendered at " + std::to_string(int(std::lround(dpi))) + " dpi");
    out.info = std::to_string(pages) + (pages == 1 ? " page" : " pages");
    add_info(out, "Title", poppler_document_get_title(doc));
    add_info(out, "Author", poppler_document_get_author(doc));
    add_info(out, "Subject", poppler_document_get_subject(doc));
    add_info(out, "Keywords", poppler_document_get_keywords(doc));
    add_info(out, "Creator", poppler_document_get_creator(doc));
    add_info(out, "Producer", poppler_document_get_producer(doc));
    add_info(out, "PDF version", poppler_document_get_pdf_version_string(doc));
    out.add(N_("Document"), N_("Pages"), std::to_string(pages));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f × %.1f mm", pw * 25.4 / 72, ph * 25.4 / 72);
    out.add(N_("Document"), N_("Page size"), buf);
    out.add(N_("Document"), N_("Rendered at"), std::to_string(int(std::lround(dpi))) + " dpi");
    out.vector = std::make_shared<PdfPage>(doc, page, dpi);  // keeps them
    return true;
}

}  // namespace

bool pdf_available()
{
    return !g_getenv("IFRIT_NO_PDF") && platform::library_installed(kPopplerLibrary);
}

bool pdf_supported()
{
    static const bool loaded = load_poppler();
    return loaded;
}

const Decoder& pdf_decoder()
{
    static const Decoder d{"PDF", {"pdf"}, sniff, decode};
    return d;
}
