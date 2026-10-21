// PDF through poppler-glib: the first page rendered to pixels at a fixed
// DPI, over white. Page navigation can come later.
#include "decoder.h"

#include <poppler.h>
#include <cairo.h>
#include <glib.h>

#include <cmath>
#include <cstring>

namespace {

constexpr double kDpi = 150.0;

bool sniff(const uint8_t* h, size_t n) {
    return n >= 5 && std::memcmp(h, "%PDF-", 5) == 0;
}

void add_info(Image& out, const char* key, gchar* value) {
    if (value && *value) out.add("Document", key, value);
    g_free(value);
}

bool decode(const std::string& path, Image& out, std::string& error) {
    gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    GError* err = nullptr;
    PopplerDocument* doc = poppler_document_new_from_file(uri, nullptr, &err);
    g_free(uri);
    if (!doc) {
        error = err ? err->message : "cannot open PDF";
        if (err) g_error_free(err);
        return false;
    }
    const int pages = poppler_document_get_n_pages(doc);
    PopplerPage* page = pages > 0 ? poppler_document_get_page(doc, 0) : nullptr;
    if (!page) {
        error = "PDF has no pages";
        g_object_unref(doc);
        return false;
    }
    double pw = 0, ph = 0;
    poppler_page_get_size(page, &pw, &ph);   // points
    const double scale = kDpi / 72.0;
    out.allocate(int(std::ceil(pw * scale)), int(std::ceil(ph * scale)));
    cairo_surface_t* surface = cairo_image_surface_create_for_data(out.pixels.data(), CAIRO_FORMAT_ARGB32, out.width, out.height, out.stride);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);

    out.format = "PDF";
    out.info = std::to_string(pages) + (pages == 1 ? " page" : " pages");
    add_info(out, "Title", poppler_document_get_title(doc));
    add_info(out, "Author", poppler_document_get_author(doc));
    add_info(out, "Subject", poppler_document_get_subject(doc));
    add_info(out, "Keywords", poppler_document_get_keywords(doc));
    add_info(out, "Creator", poppler_document_get_creator(doc));
    add_info(out, "Producer", poppler_document_get_producer(doc));
    add_info(out, "PDF version", poppler_document_get_pdf_version_string(doc));
    out.add("Document", "Pages", std::to_string(pages));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f × %.1f mm", pw * 25.4 / 72, ph * 25.4 / 72);
    out.add("Document", "Page size", buf);
    out.add("Document", "Rendered at", std::to_string(int(kDpi)) + " dpi");

    g_object_unref(page);
    g_object_unref(doc);
    return true;
}

}  // namespace

const Decoder& pdf_decoder() {
    static const Decoder d{"PDF", {"pdf"}, sniff, decode};
    return d;
}
