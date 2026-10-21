#include "decoder.h"
#include "icc.h"

#include <png.h>

#include <cstdio>
#include <cstring>

namespace {

bool sniff(const uint8_t* h, size_t n) {
    static const uint8_t magic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return n >= 8 && std::memcmp(h, magic, 8) == 0;
}

void on_error(png_structp png, png_const_charp msg) {
    auto* error = static_cast<std::string*>(png_get_error_ptr(png));
    if (error && error->empty()) *error = msg;
    longjmp(png_jmpbuf(png), 1);
}

void on_warning(png_structp, png_const_charp) {}

bool decode(const std::string& path, Image& out, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open file";
        return false;
    }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &error, on_error, on_warning);
    png_infop info = png_create_info_struct(png);
    std::vector<png_bytep> rows;  // before setjmp so the error path can't skip its destructor
    if (setjmp(png_jmpbuf(png))) {
        if (error.empty()) error = "PNG decode error";
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(f);
        return false;
    }

    png_init_io(png, f);
    png_read_info(png, info);
    png_uint_32 w = 0, h = 0;
    int depth = 0, color = 0, interlace = 0;
    png_get_IHDR(png, info, &w, &h, &depth, &color, &interlace, nullptr, nullptr);

    // Normalise everything to 8-bit BGRA.
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    const bool trns = png_get_valid(png, info, PNG_INFO_tRNS);
    if (trns) png_set_tRNS_to_alpha(png);
    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_set_bgr(png);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    out.allocate(int(w), int(h));
    rows.resize(h);
    for (png_uint_32 y = 0; y < h; ++y) rows[y] = reinterpret_cast<png_bytep>(out.row(int(y)));
    png_read_image(png, rows.data());
    png_read_end(png, info);

    {
        using M = PixelFormat::Model;
        if (color == PNG_COLOR_TYPE_PALETTE) {
            int entries = 0;
            png_colorp pal = nullptr;
            if (png_get_PLTE(png, info, &pal, &entries)) {}
            out.pixel_format = PixelFormat::palette(depth, entries);
        } else {
            const char* names = color == PNG_COLOR_TYPE_GRAY ? "Y" : color == PNG_COLOR_TYPE_GRAY_ALPHA ? "YA" : color == PNG_COLOR_TYPE_RGB_ALPHA ? "RGBA" : "RGB";
            out.pixel_format = PixelFormat::packed(color & PNG_COLOR_MASK_COLOR ? M::RGB : M::Gray, names, depth);
        }
        if (trns && !(color & PNG_COLOR_MASK_ALPHA)) out.pixel_format.notes.push_back("transparency key");
        if (interlace != PNG_INTERLACE_NONE) out.pixel_format.notes.push_back("interlaced");
    }
    out.add("PNG", "Bit depth", std::to_string(depth));
    out.add("PNG", "Color type", color == PNG_COLOR_TYPE_PALETTE ? "palette" : color == PNG_COLOR_TYPE_GRAY ? "grayscale"
                                 : color == PNG_COLOR_TYPE_GRAY_ALPHA ? "grayscale + alpha" : color == PNG_COLOR_TYPE_RGB_ALPHA ? "RGB + alpha" : "RGB");
    png_uint_32 xres = 0, yres = 0;
    int unit = 0;
    if (png_get_pHYs(png, info, &xres, &yres, &unit) && unit == PNG_RESOLUTION_METER)
        out.add("PNG", "Resolution", std::to_string(int(xres * 0.0254 + 0.5)) + " dpi");
    double gamma = 0;
    if (png_get_gAMA(png, info, &gamma)) out.add("PNG", "Gamma", std::to_string(gamma).substr(0, 6));
    int intent = 0;
    if (png_get_sRGB(png, info, &intent)) out.add("PNG", "sRGB", "yes");
    png_charp icc_name = nullptr;
    png_bytep icc = nullptr;
    png_uint_32 icc_len = 0;
    int comp = 0;
    if (png_get_iCCP(png, info, &icc_name, &comp, &icc, &icc_len) && icc_name) {
        out.add("PNG", "ICC profile", icc_name);
        out.color_space = icc_profile_description(icc, icc_len);
        if (out.color_space.empty()) out.color_space = icc_name;
    } else if (png_get_valid(png, info, PNG_INFO_sRGB)) {
        out.color_space = "sRGB";
    }
    png_textp text = nullptr;
    int n_text = 0;
    if (png_get_text(png, info, &text, &n_text) > 0)
        for (int i = 0; i < n_text; ++i)
            if (text[i].key && text[i].text) out.add("PNG text", text[i].key, std::string(text[i].text).substr(0, 2000));
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(f);

    out.has_alpha = trns || (color & PNG_COLOR_MASK_ALPHA);
    out.bits_per_sample = depth;
    if (out.has_alpha) out.from_bgra8(out.pixels.data(), out.stride);
    out.format = "PNG";
    if (color == PNG_COLOR_TYPE_PALETTE) out.format += ", palette";
    else if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) out.format += ", grayscale";
    if (interlace != PNG_INTERLACE_NONE) out.format += ", interlaced";
    return true;
}

}  // namespace

const Decoder& png_decoder() {
    static const Decoder d{"PNG", {"png"}, sniff, decode};
    return d;
}
