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
const Encoder& png_encoder();

bool encode_png(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
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
    png_set_compression_level(png, 9);  // always the smallest: it's lossless, and saving is rare
    const bool interlaced = png_encoder().value(options, "interlaced") != 0;
    png_set_IHDR(png, info, image.width, image.height, 8, image.has_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
                 interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    if (interlaced)
        png_set_interlace_handling(png);
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

const Encoder& jpeg_encoder();

bool encode_jpeg(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    const Encoder& self = jpeg_encoder();
    const int quality = std::clamp(self.value(options, "quality"), 1, 100);
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
    jpeg_set_quality(&cinfo, quality, TRUE);
    // Chroma resolution, as luma samples per chroma sample (h, v): 4:4:4 keeps
    // full colour, 4:2:0 (the library's default) halves it both ways.
    int subsampling = self.value(options, "subsampling");  // Auto, 4:4:4, 4:2:2, 4:2:0
    if (subsampling == 0)
        subsampling = quality >= 90 ? 1 : 3;
    cinfo.comp_info[0].h_samp_factor = subsampling == 1 ? 1 : 2;
    cinfo.comp_info[0].v_samp_factor = subsampling == 3 ? 2 : 1;
    cinfo.optimize_coding = self.value(options, "optimise") ? TRUE : FALSE;
    if (self.value(options, "progressive"))
        jpeg_simple_progression(&cinfo);
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
const Encoder& tiff_encoder();

bool encode_tiff(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    static const uint16_t kCompression[] = {COMPRESSION_ADOBE_DEFLATE, COMPRESSION_LZW, COMPRESSION_NONE};
    const int compression = std::clamp(tiff_encoder().value(options, "compression"), 0, 2);
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
    TIFFSetField(tif, TIFFTAG_COMPRESSION, kCompression[compression]);
    if (compression != 2)
        TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);  // smaller for photos
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
const Encoder& webp_encoder();

bool encode_webp(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    const Encoder& self = webp_encoder();
    WebPConfig config;
    if (!WebPConfigInit(&config))
    {
        error = "WebP library mismatch";
        return false;
    }
    config.lossless = self.value(options, "lossless") ? 1 : 0;
    config.quality = config.lossless ? 75.f : float(std::clamp(self.value(options, "quality"), 1, 100));
    config.method = std::clamp(self.value(options, "effort"), 0, 6);
    WebPPicture pic;
    WebPPictureInit(&pic);
    pic.use_argb = 1;
    pic.width = image.width;
    pic.height = image.height;
    std::vector<uint8_t> pixels = to_rgba8(image);
    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &writer;
    bool ok = WebPPictureImportRGBA(&pic, pixels.data(), image.width * 4) && WebPEncode(&config, &pic);
    if (!ok)
        error = "WebP encode failed";
    else
        ok = write_file(path, writer.mem, writer.size, error);
    WebPPictureFree(&pic);
    WebPMemoryWriterClear(&writer);
    return ok;
}
#endif

#ifdef HAVE_JXL
const Encoder& jxl_encoder();

bool encode_jxl(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    const Encoder& self = jxl_encoder();
    const bool lossless = self.value(options, "lossless") != 0;
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
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    JxlEncoderSetBasicInfo(enc.get(), &info);
    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    JxlEncoderSetColorEncoding(enc.get(), &color);
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (lossless)
        JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    else
        JxlEncoderSetFrameDistance(fs, JxlEncoderDistanceFromQuality(float(std::clamp(self.value(options, "quality"), 1, 100))));
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, std::clamp(self.value(options, "effort"), 1, 9));
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
const Encoder& avif_encoder();

bool encode_avif(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    const Encoder& self = avif_encoder();
    heif_context* ctx = heif_context_alloc();
    heif_encoder* enc = nullptr;
    heif_error e = heif_context_get_encoder_for_format(ctx, heif_compression_AV1, &enc);
    if (e.code)
    {
        error = std::string("no AV1 encoder available: ") + e.message;
        heif_context_free(ctx);
        return false;
    }
    if (self.value(options, "lossless"))
        heif_encoder_set_lossless(enc, 1);
    else
        heif_encoder_set_lossy_quality(enc, std::clamp(self.value(options, "quality"), 1, 100));
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


// The formats and the settings each offers when saving.
using Kind = EncodeSetting::Kind;

const EncodeSetting kQuality{"quality", "Quality", Kind::Range, 1, 100, 90, {}, nullptr, "lossless"};
const EncodeSetting kLossless{"lossless", "Lossless", Kind::Toggle, 0, 1, 0, {}, "Every pixel kept exactly; larger files."};

#ifdef HAVE_PNG
const Encoder& png_encoder()
{
    static const Encoder e{"PNG", "png",
                           {{"interlaced", "Interlaced (Adam7)", Kind::Toggle, 0, 1, 0, {}, "Shows coarse-to-fine while loading; a little larger."}},
                           encode_png};
    return e;
}
#endif
#ifdef HAVE_JPEG
const Encoder& jpeg_encoder()
{
    static const Encoder e{"JPEG", "jpg",
                           {{"quality", "Quality", Kind::Range, 1, 100, 90},
                            {"subsampling", "Colour detail", Kind::Choice, 0, 3, 0,
                             {"Automatic (full at quality 90 and above)", "Full (4:4:4)", "Half across (4:2:2)", "Quarter (4:2:0, smallest)"}},
                            {"progressive", "Progressive", Kind::Toggle, 0, 1, 0, {}, "Shows coarse-to-fine while loading; often a little smaller."},
                            {"optimise", "Optimise coding", Kind::Toggle, 0, 1, 1, {}, "A few percent smaller, at no cost in quality."}},
                           encode_jpeg};
    return e;
}
#endif
#ifdef HAVE_TIFF
const Encoder& tiff_encoder()
{
    static const Encoder e{"TIFF", "tif", {{"compression", "Compression", Kind::Choice, 0, 2, 0, {"Deflate (ZIP)", "LZW", "None"}, "All lossless."}},
                           encode_tiff};
    return e;
}
#endif
const Encoder& bmp_encoder()
{
    static const Encoder e{"BMP", "bmp", {}, encode_bmp};
    return e;
}
#ifdef HAVE_WEBP
const Encoder& webp_encoder()
{
    static const Encoder e{"WebP", "webp",
                           {kLossless, kQuality, {"effort", "Effort", Kind::Range, 0, 6, 4, {}, "Higher is smaller and slower to save."}},
                           encode_webp};
    return e;
}
#endif
#ifdef HAVE_JXL
const Encoder& jxl_encoder()
{
    static const Encoder e{"JPEG XL", "jxl",
                           {kLossless, kQuality, {"effort", "Effort", Kind::Range, 1, 9, 7, {}, "Higher is smaller and slower to save."}},
                           encode_jxl};
    return e;
}
#endif
#ifdef HAVE_HEIF
const Encoder& avif_encoder()
{
    static const Encoder e{"AVIF", "avif", {kLossless, kQuality}, encode_avif};
    return e;
}
#endif

}  // namespace

const std::vector<const Encoder*>& encoders()
{
    static const std::vector<const Encoder*> list = []
    {
        std::vector<const Encoder*> v;
#ifdef HAVE_PNG
        v.push_back(&png_encoder());
#endif
#ifdef HAVE_JPEG
        v.push_back(&jpeg_encoder());
#endif
#ifdef HAVE_TIFF
        v.push_back(&tiff_encoder());
#endif
        v.push_back(&bmp_encoder());
#ifdef HAVE_WEBP
        v.push_back(&webp_encoder());
#endif
#ifdef HAVE_JXL
        v.push_back(&jxl_encoder());
#endif
#ifdef HAVE_HEIF
        v.push_back(&avif_encoder());
#endif
        return v;
    }();
    return list;
}
