#include "decoder.h"
#include "icc.h"

#include <jpeglib.h>

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

struct ErrorManager
{
    jpeg_error_mgr pub;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

void on_error(j_common_ptr cinfo)
{
    auto* err = reinterpret_cast<ErrorManager*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jump, 1);
}

void on_message(j_common_ptr) {}  // keep libjpeg's warnings off stderr

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF;
}

// Shared body: the source is already attached to cinfo.
bool decode_from(jpeg_decompress_struct& cinfo, ErrorManager& err, Image& out, std::string& error)
{
    std::vector<uint8_t> cmyk_row;  // declared before setjmp so the error path can't skip its destructor
    bool progressive = false;
    if (setjmp(err.jump))
    {
        error = err.message;
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);  // keep the ICC profile
    jpeg_read_header(&cinfo, TRUE);
    progressive = cinfo.progressive_mode;
    {
        JOCTET* icc = nullptr;
        unsigned int icc_len = 0;
        if (jpeg_read_icc_profile(&cinfo, &icc, &icc_len))
        {
            out.color_space = icc_profile_description(icc, icc_len);
            free(icc);
        }
    }
    const bool cmyk = cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK;
    cinfo.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_BGRX;
    jpeg_start_decompress(&cinfo);

    out.allocate(int(cinfo.output_width), int(cinfo.output_height));
    if (cmyk)
        cmyk_row.resize(size_t(out.width) * 4);
    while (cinfo.output_scanline < cinfo.output_height)
    {
        uint32_t* row = out.row(int(cinfo.output_scanline));
        if (cmyk)
        {
            JSAMPROW rows[1] = {cmyk_row.data()};
            jpeg_read_scanlines(&cinfo, rows, 1);
            // Photoshop stores CMYK inverted, which makes the conversion a plain multiply.
            for (int x = 0; x < out.width; ++x)
            {
                const uint8_t* s = cmyk_row.data() + x * 4;
                const uint32_t k = s[3];
                row[x] = 0xFF000000u | ((s[0] * k / 255) << 16) | ((s[1] * k / 255) << 8) | (s[2] * k / 255);
            }
        }
        else
        {
            JSAMPROW rows[1] = {reinterpret_cast<JSAMPROW>(row)};
            jpeg_read_scanlines(&cinfo, rows, 1);
        }
    }
    if (!cmyk)
        out.from_bgrx8(out.pixels.data(), out.stride);
    const char* subsampling = "4:4:4";
    if (!cmyk && cinfo.num_components == 3)
    {
        const int h = cinfo.comp_info[0].h_samp_factor, v = cinfo.comp_info[0].v_samp_factor;
        subsampling = (h == 2 && v == 2) ? "4:2:0" : (h == 2 && v == 1) ? "4:2:2" : (h == 1 && v == 2) ? "4:4:0" : "4:4:4";
    }
    {
        using M = PixelFormat::Model;
        if (cmyk)
            out.pixel_format = PixelFormat::packed(M::CMYK, "CMYK", cinfo.data_precision);
        else if (cinfo.num_components == 1)
            out.pixel_format = PixelFormat::packed(M::Gray, "Y", cinfo.data_precision);
        else if (cinfo.jpeg_color_space == JCS_RGB)
            out.pixel_format = PixelFormat::packed(M::RGB, "RGB", cinfo.data_precision);
        else
            out.pixel_format = PixelFormat::ycbcr(cinfo.data_precision, cinfo.comp_info[0].h_samp_factor, cinfo.comp_info[0].v_samp_factor);
        out.pixel_format.notes.push_back(progressive ? "progressive" : "baseline");
    }
    out.add("JPEG", "Coding", progressive ? "progressive" : "baseline");
    out.add("JPEG", "Components", std::to_string(cinfo.num_components));
    if (cinfo.num_components == 3)
        out.add("JPEG", "Chroma subsampling", subsampling);
    if (cinfo.saw_JFIF_marker)
        out.add("JPEG", "JFIF", std::to_string(cinfo.JFIF_major_version) + "." + std::to_string(cinfo.JFIF_minor_version));
    if (cinfo.saw_Adobe_marker)
        out.add("JPEG", "Adobe transform", std::to_string(cinfo.Adobe_transform));
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    out.format = progressive ? "JPEG (progressive)" : "JPEG";
    if (cmyk)
        out.format += ", CMYK";
    return true;
}

void setup(jpeg_decompress_struct& cinfo, ErrorManager& err)
{
    std::memset(&cinfo, 0, sizeof cinfo);
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    err.pub.output_message = on_message;
    jpeg_create_decompress(&cinfo);
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    FILE* f = open_file(path);
    if (!f)
    {
        error = "cannot open file";
        return false;
    }
    jpeg_decompress_struct cinfo;
    ErrorManager err{};
    setup(cinfo, err);
    jpeg_stdio_src(&cinfo, f);
    const bool ok = decode_from(cinfo, err, out, error);
    std::fclose(f);
    if (!ok)
        return false;
#ifdef HAVE_EXIF
    const ExifSummary exif = read_exif_file(path, out);
    out.apply_orientation(exif.orientation);
    out.info = exif.camera;
    if (out.color_space.empty())
        out.color_space = exif.color_space;  // an embedded profile is more specific than the EXIF flag
#endif
    return true;
}

}  // namespace

bool decode_jpeg_memory(const uint8_t* data, size_t size, Image& out, std::string& error)
{
    jpeg_decompress_struct cinfo;
    ErrorManager err{};
    setup(cinfo, err);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    if (!decode_from(cinfo, err, out, error))
        return false;
#ifdef HAVE_EXIF
    out.apply_orientation(read_exif_blob(data, size, out).orientation);
#endif
    return true;
}

const Decoder& jpeg_decoder()
{
    static const Decoder d{"JPEG", {"jpg", "jpeg", "jpe", "jfif"}, sniff, decode};
    return d;
}
