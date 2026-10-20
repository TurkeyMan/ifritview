#include "decoder.h"

#include <jpeglib.h>
#include <libexif/exif-data.h>

#include <csetjmp>
#include <cstdio>
#include <cstring>

namespace {

struct ErrorManager {
    jpeg_error_mgr pub;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

void on_error(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<ErrorManager*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jump, 1);
}

void on_message(j_common_ptr) {}  // keep libjpeg's warnings off stderr

struct ExifInfo {
    int orientation = 1;
    std::string camera;
};

ExifInfo read_exif(ExifData* ed) {
    ExifInfo info;
    if (!ed) return info;
    if (ExifEntry* e = exif_data_get_entry(ed, EXIF_TAG_ORIENTATION)) {
        int o = exif_get_short(e->data, exif_data_get_byte_order(ed));
        if (o >= 1 && o <= 8) info.orientation = o;
    }
    if (ExifEntry* e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MODEL)) {
        char buf[128];
        if (exif_entry_get_value(e, buf, sizeof buf)) info.camera = buf;
    }
    exif_data_unref(ed);
    return info;
}

bool sniff(const uint8_t* h, size_t n) {
    return n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF;
}

// Shared body: the source is already attached to cinfo.
bool decode_from(jpeg_decompress_struct& cinfo, ErrorManager& err, Image& out, std::string& error) {
    std::vector<uint8_t> cmyk_row;  // declared before setjmp so the error path can't skip its destructor
    bool progressive = false;
    if (setjmp(err.jump)) {
        error = err.message;
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_read_header(&cinfo, TRUE);
    progressive = cinfo.progressive_mode;
    const bool cmyk = cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK;
    cinfo.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_BGRX;
    jpeg_start_decompress(&cinfo);

    out.allocate(int(cinfo.output_width), int(cinfo.output_height));
    if (cmyk) cmyk_row.resize(size_t(out.width) * 4);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint32_t* row = out.row(int(cinfo.output_scanline));
        if (cmyk) {
            JSAMPROW rows[1] = {cmyk_row.data()};
            jpeg_read_scanlines(&cinfo, rows, 1);
            // Photoshop stores CMYK inverted, which makes the conversion a plain multiply.
            for (int x = 0; x < out.width; ++x) {
                const uint8_t* s = cmyk_row.data() + x * 4;
                const uint32_t k = s[3];
                row[x] = 0xFF000000u | ((s[0] * k / 255) << 16) | ((s[1] * k / 255) << 8) | (s[2] * k / 255);
            }
        } else {
            JSAMPROW rows[1] = {reinterpret_cast<JSAMPROW>(row)};
            jpeg_read_scanlines(&cinfo, rows, 1);
        }
    }
    if (!cmyk) out.from_bgrx8(out.pixels.data(), out.stride);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    out.format = progressive ? "JPEG (progressive)" : "JPEG";
    if (cmyk) out.format += ", CMYK";
    return true;
}

void setup(jpeg_decompress_struct& cinfo, ErrorManager& err) {
    std::memset(&cinfo, 0, sizeof cinfo);
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    err.pub.output_message = on_message;
    jpeg_create_decompress(&cinfo);
}

bool decode(const std::string& path, Image& out, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open file";
        return false;
    }
    jpeg_decompress_struct cinfo;
    ErrorManager err{};
    setup(cinfo, err);
    jpeg_stdio_src(&cinfo, f);
    const bool ok = decode_from(cinfo, err, out, error);
    std::fclose(f);
    if (!ok) return false;

    ExifInfo exif = read_exif(exif_data_new_from_file(path.c_str()));
    out.apply_orientation(exif.orientation);
    out.info = exif.camera;
    return true;
}

}  // namespace

bool decode_jpeg_memory(const uint8_t* data, size_t size, Image& out, std::string& error) {
    jpeg_decompress_struct cinfo;
    ErrorManager err{};
    setup(cinfo, err);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    if (!decode_from(cinfo, err, out, error)) return false;
    ExifInfo exif = read_exif(exif_data_new_from_data(data, static_cast<unsigned>(size)));
    out.apply_orientation(exif.orientation);
    return true;
}

const Decoder& jpeg_decoder() {
    static const Decoder d{"JPEG", {"jpg", "jpeg", "jpe", "jfif"}, sniff, decode};
    return d;
}
