#include "decoder.h"
#include "../i18n.h"
#include "icc.h"

#include <jpeglib.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

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

// libjpeg's scaling of its standard luminance table by quality, undone: a
// rough figure for files whose tables were made otherwise (cameras').
int estimate_quality(const JQUANT_TBL& table)
{
    static const unsigned standard[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                          14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                          18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                          49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
    double sum = 0, std_sum = 0;
    bool ones = true;
    for (int i = 0; i < 64; ++i)
    {
        sum += table.quantval[i];
        std_sum += standard[i];
        ones = ones && table.quantval[i] == 1;
    }
    if (ones)
        return 100;
    const double scale = sum * 100 / std_sum;  // libjpeg's percentage of the standard table
    const double q = scale <= 100 ? (200 - scale) / 2 : 5000 / scale;
    return std::clamp(int(std::lround(q)), 1, 100);
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
    // As the save dialog's JPEG settings.
    out.encoded_as = "jpg";
    out.encoding["progressive"] = progressive;
    int quality = 0;
    if (const JQUANT_TBL* table = cinfo.quant_tbl_ptrs[cinfo.comp_info[0].quant_tbl_no])
        out.encoding["quality"] = quality = estimate_quality(*table);
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
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    cinfo.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_BGRA;  // cairo's ARGB32 byte order, alpha 0xFF: no second pass
#else
    cinfo.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_ARGB;
#endif
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
    const char* subsampling = "4:4:4";
    if (!cmyk && cinfo.num_components == 3)
    {
        const int h = cinfo.comp_info[0].h_samp_factor, v = cinfo.comp_info[0].v_samp_factor;
        subsampling = (h == 2 && v == 2) ? "4:2:0" : (h == 2 && v == 1) ? "4:2:2" : (h == 1 && v == 2) ? "4:4:0" : "4:4:4";
        // The settings' choices: full, half across, quarter (4:4:0 has none).
        const int choice = (h == 1 && v == 1) ? 1 : (h == 2 && v == 1) ? 2 : (h == 2 && v == 2) ? 3 : 0;
        if (choice)
            out.encoding["subsampling"] = choice;
    }
    {
        using M = PixelFormat::Model;
        if (cmyk)
            out.pixel_format = PixelFormat::packed(M::CMYK, "CMYK", cinfo.data_precision);
        else if (cinfo.num_components == 1)
            out.pixel_format = PixelFormat::packed(M::L, "L", cinfo.data_precision);
        else if (cinfo.jpeg_color_space == JCS_RGB)
            out.pixel_format = PixelFormat::packed(M::RGB, "RGB", cinfo.data_precision);
        else
            out.pixel_format = PixelFormat::ycbcr(cinfo.data_precision, cinfo.comp_info[0].h_samp_factor, cinfo.comp_info[0].v_samp_factor);
        out.pixel_format.notes.push_back(progressive ? "progressive" : "baseline");
    }
    out.add(N_("JPEG"), N_("Coding"), progressive ? "progressive" : "baseline");
    out.add(N_("JPEG"), N_("Components"), std::to_string(cinfo.num_components));
    if (quality)
        out.add(N_("JPEG"), N_("Quality (estimated)"), std::to_string(quality));
    if (cinfo.num_components == 3)
        out.add(N_("JPEG"), N_("Chroma subsampling"), subsampling);
    if (cinfo.saw_JFIF_marker)
        out.add(N_("JPEG"), N_("JFIF"), std::to_string(cinfo.JFIF_major_version) + "." + std::to_string(cinfo.JFIF_minor_version));
    if (cinfo.saw_Adobe_marker)
        out.add(N_("JPEG"), N_("Adobe transform"), std::to_string(cinfo.Adobe_transform));
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
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (!file_bytes(path, bytes, size))
    {
        error = "cannot open file";
        return false;
    }
    jpeg_decompress_struct cinfo;
    ErrorManager err{};
    setup(cinfo, err);
    jpeg_mem_src(&cinfo, bytes, static_cast<unsigned long>(size));
    const bool ok = decode_from(cinfo, err, out, error);
    if (!ok)
        return false;
#ifdef HAVE_EXIF
    const ExifSummary exif = read_exif_blob(bytes, size, out);  // the file's bytes, already in memory
    out.apply_camera_orientation(exif.orientation);
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
    out.apply_camera_orientation(read_exif_blob(data, size, out).orientation);
#endif
    return true;
}

bool decode_jpeg_samples(const uint8_t* data, size_t size, std::vector<uint8_t>& samples, int& width, int& height,
                         int& components, std::string& error)
{
    jpeg_decompress_struct cinfo;
    ErrorManager err{};
    setup(cinfo, err);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));
    if (setjmp(err.jump))
    {
        error = err.message;
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_read_header(&cinfo, TRUE);
    if (cinfo.num_components == 4)
        cinfo.jpeg_color_space = cinfo.out_color_space = JCS_CMYK;  // the samples untouched, whatever the markers say
    else if (cinfo.num_components != 1)
        cinfo.out_color_space = JCS_RGB;
    if (uint64_t(cinfo.image_width) * cinfo.image_height > (uint64_t(1) << 30))
    {
        error = "JPEG is too large";
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_start_decompress(&cinfo);
    width = int(cinfo.output_width);
    height = int(cinfo.output_height);
    components = cinfo.output_components;
    const size_t row = size_t(width) * components;
    samples.resize(row * height);
    while (cinfo.output_scanline < cinfo.output_height)
    {
        JSAMPROW rows[1] = {samples.data() + row * cinfo.output_scanline};
        jpeg_read_scanlines(&cinfo, rows, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

const Decoder& jpeg_decoder()
{
    static const Decoder d{"JPEG", {"jpg", "jpeg", "jpe", "jfif"}, sniff, decode};
    return d;
}
