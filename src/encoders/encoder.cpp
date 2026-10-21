// Image writers for the formats worth saving to: PNG, JPEG, TIFF, BMP,
// WebP, JPEG XL and AVIF. Each sits behind the same HAVE_* define as its
// decoder (BMP goes through gdk-pixbuf, which is always present).
#include "encoder.h"

#include "../platform.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef HAVE_PNG
#include <png.h>
#endif
#ifdef HAVE_JPEG
#include <jpeglib.h>
#include <csetjmp>
#endif
#ifdef HAVE_TIFF
#include <tiffio.h>
#endif
#ifdef HAVE_WEBP
#include <webp/encode.h>
#endif
#ifdef HAVE_JXL
#include <jxl/encode.h>
#include <memory>
#endif
#ifdef HAVE_HEIF
#include <libheif/heif.h>
#endif

std::vector<uint8_t> to_rgba8(const Image& image)
{
    std::vector<uint8_t> out(size_t(image.width) * image.height * 4);
    uint8_t* d = out.data();
    for (int y = 0; y < image.height; ++y)
    {
        const uint32_t* s = image.row(y);
        for (int x = 0; x < image.width; ++x, d += 4)
        {
            const uint32_t p = s[x];
            const uint32_t a = p >> 24;
            uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (a != 0 && a != 255)
            {  // un-premultiply
                r = std::min(255u, (r * 255 + a / 2) / a);
                g = std::min(255u, (g * 255 + a / 2) / a);
                b = std::min(255u, (b * 255 + a / 2) / a);
            }
            d[0] = uint8_t(r);
            d[1] = uint8_t(g);
            d[2] = uint8_t(b);
            d[3] = uint8_t(a);
        }
    }
    return out;
}

std::vector<uint8_t> to_rgb8(const Image& image)
{
    std::vector<uint8_t> out(size_t(image.width) * image.height * 3);
    uint8_t* d = out.data();
    for (int y = 0; y < image.height; ++y)
    {
        const uint32_t* s = image.row(y);
        for (int x = 0; x < image.width; ++x, d += 3)
        {
            d[0] = uint8_t(s[x] >> 16);
            d[1] = uint8_t(s[x] >> 8);
            d[2] = uint8_t(s[x]);
        }
    }
    return out;
}

namespace
{

#ifdef HAVE_PNG
bool encode_png(const Image& image, const std::string& path, const EncodeOptions&, std::string& error)
{
    FILE* f = platform::fopen(path, "wb");
    if (!f)
    {
        error = "cannot create file";
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    std::vector<uint8_t> pixels = image.has_alpha ? to_rgba8(image) : to_rgb8(image);
    std::vector<png_bytep> rows(image.height);
    const int bpp = image.has_alpha ? 4 : 3;
    for (int y = 0; y < image.height; ++y)
        rows[y] = pixels.data() + size_t(y) * image.width * bpp;
    if (setjmp(png_jmpbuf(png)))
    {
        error = "PNG encode error";
        png_destroy_write_struct(&png, &info);
        std::fclose(f);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, image.width, image.height, 8, image.has_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(f);
    return true;
}
#endif

#ifdef HAVE_JPEG
struct JpegError
{
    jpeg_error_mgr pub;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

bool encode_jpeg(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    FILE* f = platform::fopen(path, "wb");
    if (!f)
    {
        error = "cannot create file";
        return false;
    }
    std::vector<uint8_t> pixels = to_rgb8(image);
    jpeg_compress_struct cinfo{};
    JpegError err{};
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = [](j_common_ptr c)
    {
        auto* e = reinterpret_cast<JpegError*>(c->err);
        (*c->err->format_message)(c, e->message);
        longjmp(e->jump, 1);
    };
    err.pub.output_message = [](j_common_ptr) {};
    if (setjmp(err.jump))
    {
        error = err.message;
        jpeg_destroy_compress(&cinfo);
        std::fclose(f);
        return false;
    }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = image.width;
    cinfo.image_height = image.height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, std::clamp(options.quality, 1, 100), TRUE);
    if (options.quality >= 90)
    {  // keep chroma at full resolution for high quality
        cinfo.comp_info[0].h_samp_factor = 1;
        cinfo.comp_info[0].v_samp_factor = 1;
    }
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height)
    {
        JSAMPROW row = pixels.data() + size_t(cinfo.next_scanline) * image.width * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::fclose(f);
    return true;
}
#endif

#ifdef HAVE_TIFF
bool encode_tiff(const Image& image, const std::string& path, const EncodeOptions&, std::string& error)
{
    TIFF* tif = TIFFOpen(path.c_str(), "w");
    if (!tif)
    {
        error = "cannot create file";
        return false;
    }
    const int spp = image.has_alpha ? 4 : 3;
    std::vector<uint8_t> pixels = image.has_alpha ? to_rgba8(image) : to_rgb8(image);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t(image.width));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t(image.height));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t(spp));
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t(8));
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
    if (image.has_alpha)
    {
        const uint16_t extra[1] = {EXTRASAMPLE_UNASSALPHA};
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, extra);
    }
    bool ok = true;
    for (int y = 0; y < image.height && ok; ++y)
        ok = TIFFWriteScanline(tif, pixels.data() + size_t(y) * image.width * spp, y, 0) >= 0;
    TIFFClose(tif);
    if (!ok)
        error = "TIFF write failed";
    return ok;
}
#endif

bool write_file(const std::string& path, const void* data, size_t size, std::string& error)
{
    FILE* f = platform::fopen(path, "wb");
    if (!f)
    {
        error = "cannot create file";
        return false;
    }
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    if (!ok)
        error = "write failed";
    return ok;
}

#ifdef HAVE_WEBP
bool encode_webp(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    std::vector<uint8_t> pixels = to_rgba8(image);
    uint8_t* out = nullptr;
    size_t size = options.quality >= 100 ? WebPEncodeLosslessRGBA(pixels.data(), image.width, image.height, image.width * 4, &out)
                                         : WebPEncodeRGBA(pixels.data(), image.width, image.height, image.width * 4, float(options.quality), &out);
    if (!size)
    {
        error = "WebP encode failed";
        return false;
    }
    const bool ok = write_file(path, out, size, error);
    WebPFree(out);
    return ok;
}
#endif

#ifdef HAVE_JXL
bool encode_jxl(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    std::vector<uint8_t> pixels = image.has_alpha ? to_rgba8(image) : to_rgb8(image);
    std::unique_ptr<JxlEncoder, decltype(&JxlEncoderDestroy)> enc(JxlEncoderCreate(nullptr), JxlEncoderDestroy);
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = image.width;
    info.ysize = image.height;
    info.num_color_channels = 3;
    info.num_extra_channels = image.has_alpha ? 1 : 0;
    info.alpha_bits = image.has_alpha ? 8 : 0;
    info.bits_per_sample = 8;
    info.uses_original_profile = options.quality >= 100 ? JXL_TRUE : JXL_FALSE;
    JxlEncoderSetBasicInfo(enc.get(), &info);
    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    JxlEncoderSetColorEncoding(enc.get(), &color);
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (options.quality >= 100)
        JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    else
        JxlEncoderSetFrameDistance(fs, JxlEncoderDistanceFromQuality(float(options.quality)));
    const JxlPixelFormat fmt{uint32_t(image.has_alpha ? 4 : 3), JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    if (JxlEncoderAddImageFrame(fs, &fmt, pixels.data(), pixels.size()) != JXL_ENC_SUCCESS)
    {
        error = "JXL encode failed";
        return false;
    }
    JxlEncoderCloseInput(enc.get());
    std::vector<uint8_t> out(1 << 20);
    uint8_t* next = out.data();
    size_t avail = out.size();
    JxlEncoderStatus st;
    while ((st = JxlEncoderProcessOutput(enc.get(), &next, &avail)) == JXL_ENC_NEED_MORE_OUTPUT)
    {
        const size_t used = next - out.data();
        out.resize(out.size() * 2);
        next = out.data() + used;
        avail = out.size() - used;
    }
    if (st != JXL_ENC_SUCCESS)
    {
        error = "JXL encode failed";
        return false;
    }
    return write_file(path, out.data(), next - out.data(), error);
}
#endif

#ifdef HAVE_HEIF
bool encode_avif(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    heif_context* ctx = heif_context_alloc();
    heif_encoder* enc = nullptr;
    heif_error e = heif_context_get_encoder_for_format(ctx, heif_compression_AV1, &enc);
    if (e.code)
    {
        error = std::string("no AV1 encoder available: ") + e.message;
        heif_context_free(ctx);
        return false;
    }
    if (options.quality >= 100)
        heif_encoder_set_lossless(enc, 1);
    else
        heif_encoder_set_lossy_quality(enc, options.quality);
    heif_image* img = nullptr;
    const heif_chroma chroma = image.has_alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB;
    heif_image_create(image.width, image.height, heif_colorspace_RGB, chroma, &img);
    heif_image_add_plane(img, heif_channel_interleaved, image.width, image.height, 8);
    int stride = 0;
    uint8_t* plane = heif_image_get_plane(img, heif_channel_interleaved, &stride);
    std::vector<uint8_t> pixels = image.has_alpha ? to_rgba8(image) : to_rgb8(image);
    const int bpp = image.has_alpha ? 4 : 3;
    for (int y = 0; y < image.height; ++y)
        std::memcpy(plane + size_t(y) * stride, pixels.data() + size_t(y) * image.width * bpp, size_t(image.width) * bpp);
    heif_image_handle* handle = nullptr;
    e = heif_context_encode_image(ctx, img, enc, nullptr, &handle);
    bool ok = e.code == heif_error_Ok;
    if (!ok)
        error = e.message;
    else
    {
        e = heif_context_write_to_file(ctx, path.c_str());
        ok = e.code == heif_error_Ok;
        if (!ok)
            error = e.message;
    }
    if (handle)
        heif_image_handle_release(handle);
    heif_image_release(img);
    heif_encoder_release(enc);
    heif_context_free(ctx);
    return ok;
}
#endif

bool encode_bmp(const Image& image, const std::string& path, const EncodeOptions&, std::string& error)
{
    std::vector<uint8_t> pixels = to_rgb8(image);
    GdkPixbuf* pb = gdk_pixbuf_new_from_data(pixels.data(), GDK_COLORSPACE_RGB, FALSE, 8, image.width, image.height, image.width * 3, nullptr, nullptr);
    GError* err = nullptr;
    const bool ok = gdk_pixbuf_save(pb, path.c_str(), "bmp", &err, nullptr);
    if (!ok)
    {
        error = err ? err->message : "BMP write failed";
        if (err)
            g_error_free(err);
    }
    g_object_unref(pb);
    return ok;
}

}  // namespace

const std::vector<const Encoder*>& encoders()
{
    static const std::vector<Encoder> table = []
    {
        std::vector<Encoder> v;
#ifdef HAVE_PNG
        v.push_back({"PNG", "png", false, encode_png});
#endif
#ifdef HAVE_JPEG
        v.push_back({"JPEG", "jpg", true, encode_jpeg});
#endif
#ifdef HAVE_TIFF
        v.push_back({"TIFF", "tif", false, encode_tiff});
#endif
        v.push_back({"BMP", "bmp", false, encode_bmp});
#ifdef HAVE_WEBP
        v.push_back({"WebP", "webp", true, encode_webp});
#endif
#ifdef HAVE_JXL
        v.push_back({"JPEG XL", "jxl", true, encode_jxl});
#endif
#ifdef HAVE_HEIF
        v.push_back({"AVIF", "avif", true, encode_avif});
#endif
        return v;
    }();
    static const std::vector<const Encoder*> list = []
    {
        std::vector<const Encoder*> p;
        for (const Encoder& e : table)
            p.push_back(&e);
        return p;
    }();
    return list;
}
