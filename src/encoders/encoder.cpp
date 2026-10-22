// Image writers for the formats worth saving to: PNG, JPEG, TIFF, BMP,
// Targa, WebP, JPEG XL and AVIF. Each library-backed one sits behind the
// same HAVE_* define as its decoder; BMP and Targa are written here.
#include "encoder.h"

#include "reduce.h"

#include "../platform.h"


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
    cinfo.optimize_coding = TRUE;  // lossless, and always smaller
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
    config.method = 6;  // the smallest: it only costs time at save
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
    // libjxl's default; 8 and 9 take minutes on a large photo for little gain.
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, 7);
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

// Little-endian fields for the BMP and Targa headers.
void put16(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}
void put32(std::vector<uint8_t>& out, uint32_t v)
{
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

const Encoder& bmp_encoder();

// BMP in the pixel formats games and tools still read: 32-bit with alpha,
// 24-bit, the 16-bit bitfield layouts and 8-bit with a palette.
bool encode_bmp(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    const Encoder& self = bmp_encoder();
    int format = self.value(options, "format");  // Auto, 32, 24, 565, 1555, 4444, indexed
    if (format == 0)
        format = image.has_alpha ? 1 : 2;
    const bool dither = self.value(options, "dither") != 0;
    const bool srgb = self.value(options, "srgb") != 0;
    const bool top_down = self.value(options, "topdown") != 0;
    const int w = image.width, h = image.height;
    std::vector<uint8_t> px = to_rgba8(image);
    int bpp = 24;
    uint32_t mask[4] = {0, 0, 0, 0};  // R, G, B, A
    Palettised pal;
    switch (format)
    {
        case 1: bpp = 32, mask[0] = 0x00ff0000, mask[1] = 0x0000ff00, mask[2] = 0x000000ff, mask[3] = 0xff000000; break;
        case 3: bpp = 16, mask[0] = 0xf800, mask[1] = 0x07e0, mask[2] = 0x001f, reduce_channels(px, w, h, 5, 6, 5, 0, dither); break;
        case 4:
            bpp = 16, mask[0] = 0x7c00, mask[1] = 0x03e0, mask[2] = 0x001f, mask[3] = 0x8000;
            reduce_channels(px, w, h, 5, 5, 5, 1, dither);
            break;
        case 5:
            bpp = 16, mask[0] = 0x0f00, mask[1] = 0x00f0, mask[2] = 0x000f, mask[3] = 0xf000;
            reduce_channels(px, w, h, 4, 4, 4, 4, dither);
            break;
        case 6: bpp = 8, pal = palettise(px, w, h, dither); break;
        default: break;
    }
    const bool bitfields = mask[0] != 0;
    const bool v5 = srgb || bitfields;  // the classic header can't tag a colour space or carry an alpha mask
    const uint32_t header = v5 ? 124 : 40, palette = bpp == 8 ? 256 * 4 : 0;
    const uint32_t stride = uint32_t((w * bpp + 31) / 32 * 4), offset = 14 + header + palette;
    std::vector<uint8_t> out;
    out.reserve(offset + size_t(stride) * h);
    out.push_back('B');
    out.push_back('M');
    put32(out, offset + stride * uint32_t(h));
    put32(out, 0);
    put32(out, offset);
    put32(out, header);
    put32(out, uint32_t(w));
    put32(out, uint32_t(top_down ? -h : h));
    put16(out, 1);
    put16(out, uint16_t(bpp));
    put32(out, bitfields ? 3 /* BI_BITFIELDS */ : 0 /* BI_RGB */);
    put32(out, stride * uint32_t(h));
    put32(out, 2835);  // 72 dpi
    put32(out, 2835);
    put32(out, bpp == 8 ? 256 : 0);
    put32(out, 0);
    if (v5)
    {
        for (uint32_t m : mask)
            put32(out, m);
        put32(out, srgb ? 0x73524742 /* 'sRGB' */ : 0x57696e20 /* 'Win ': the system's */);
        out.insert(out.end(), 36 + 12, 0);  // endpoints, gamma
        put32(out, 4);                      // LCS_GM_IMAGES
        out.insert(out.end(), 12, 0);       // profile data, size, reserved
    }
    for (int i = 0; i < 256 && bpp == 8; ++i)
    {
        const bool used = size_t(i) * 3 < pal.palette.size();
        out.push_back(used ? pal.palette[size_t(i) * 3 + 2] : 0);
        out.push_back(used ? pal.palette[size_t(i) * 3 + 1] : 0);
        out.push_back(used ? pal.palette[size_t(i) * 3] : 0);
        out.push_back(0);
    }
    for (int row = 0; row < h; ++row)
    {
        const int y = top_down ? row : h - 1 - row;
        const size_t start = out.size();
        for (int x = 0; x < w; ++x)
        {
            const uint8_t* p = &px[(size_t(y) * w + x) * 4];
            const uint32_t r = p[0], g = p[1], b = p[2], a = p[3];
            switch (bpp)
            {
                case 32: out.insert(out.end(), {uint8_t(b), uint8_t(g), uint8_t(r), uint8_t(a)}); break;
                case 24: out.insert(out.end(), {uint8_t(b), uint8_t(g), uint8_t(r)}); break;
                case 16:
                {
                    uint32_t v = 0;
                    if (format == 3)
                        v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
                    else if (format == 4)
                        v = (a >= 128 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
                    else
                        v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
                    put16(out, v);
                    break;
                }
                default: out.push_back(pal.indices[size_t(y) * w + x]); break;
            }
        }
        out.resize(start + stride, 0);
    }
    return write_file(path, out.data(), out.size(), error);
}

const Encoder& tga_encoder();

// Targa: 32, 24 and 16-bit (ARGB 1555) true colour, or 8-bit with a colour
// map, optionally run-length encoded.
bool encode_tga(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)
{
    const Encoder& self = tga_encoder();
    int format = self.value(options, "format");  // Auto, 32, 24, 1555, indexed
    if (format == 0)
        format = image.has_alpha ? 1 : 2;
    const bool dither = self.value(options, "dither") != 0;
    const bool rle = self.value(options, "rle") != 0;
    const bool top_left = self.value(options, "topleft") != 0;
    const int w = image.width, h = image.height;
    std::vector<uint8_t> px = to_rgba8(image);
    const int bpp = format == 1 ? 32 : format == 2 ? 24 : format == 3 ? 16 : 8;
    Palettised pal;
    if (format == 3)
        reduce_channels(px, w, h, 5, 5, 5, 1, dither);
    else if (format == 4)
        pal = palettise(px, w, h, dither);
    const bool indexed = bpp == 8;
    std::vector<uint8_t> out;
    out.push_back(0);                  // no image id
    out.push_back(indexed ? 1 : 0);    // colour map
    out.push_back(uint8_t(indexed ? (rle ? 9 : 1) : (rle ? 10 : 2)));
    put16(out, 0);
    put16(out, indexed ? 256 : 0);
    out.push_back(indexed ? 24 : 0);
    put16(out, 0);
    put16(out, 0);
    put16(out, uint16_t(w));
    put16(out, uint16_t(h));
    out.push_back(uint8_t(bpp));
    out.push_back(uint8_t((bpp == 32 ? 8 : bpp == 16 ? 1 : 0) | (top_left ? 0x20 : 0)));
    for (int i = 0; i < 256 && indexed; ++i)
    {
        const bool used = size_t(i) * 3 < pal.palette.size();
        out.push_back(used ? pal.palette[size_t(i) * 3 + 2] : 0);
        out.push_back(used ? pal.palette[size_t(i) * 3 + 1] : 0);
        out.push_back(used ? pal.palette[size_t(i) * 3] : 0);
    }
    const int bytes = bpp / 8;
    std::vector<uint8_t> line(size_t(w) * bytes);
    for (int row = 0; row < h; ++row)
    {
        const int y = top_left ? row : h - 1 - row;
        for (int x = 0; x < w; ++x)
        {
            const uint8_t* p = &px[(size_t(y) * w + x) * 4];
            uint8_t* o = &line[size_t(x) * bytes];
            if (bpp == 8)
                o[0] = pal.indices[size_t(y) * w + x];
            else if (bpp == 16)
            {
                const uint32_t v = (p[3] >= 128 ? 0x8000u : 0) | uint32_t(p[0] >> 3) << 10 | uint32_t(p[1] >> 3) << 5 | uint32_t(p[2] >> 3);
                o[0] = uint8_t(v), o[1] = uint8_t(v >> 8);
            }
            else
            {
                o[0] = p[2], o[1] = p[1], o[2] = p[0];
                if (bpp == 32)
                    o[3] = p[3];
            }
        }
        if (!rle)
        {
            out.insert(out.end(), line.begin(), line.end());
            continue;
        }
        // Run-length packets, each within its row (some readers insist).
        int x = 0;
        while (x < w)
        {
            const auto same = [&](int a, int b) { return std::memcmp(&line[size_t(a) * bytes], &line[size_t(b) * bytes], size_t(bytes)) == 0; };
            int run = 1;
            while (x + run < w && run < 128 && same(x, x + run))
                ++run;
            if (run > 1)
            {
                out.push_back(uint8_t(0x80 | (run - 1)));
                out.insert(out.end(), line.begin() + std::ptrdiff_t(x) * bytes, line.begin() + std::ptrdiff_t(x + 1) * bytes);
                x += run;
                continue;
            }
            int raw = 1;
            while (x + raw < w && raw < 128 && !(x + raw + 1 < w && same(x + raw, x + raw + 1)))
                ++raw;
            out.push_back(uint8_t(raw - 1));
            out.insert(out.end(), line.begin() + std::ptrdiff_t(x) * bytes, line.begin() + std::ptrdiff_t(x + raw) * bytes);
            x += raw;
        }
    }
    return write_file(path, out.data(), out.size(), error);
}


// The formats and the settings each offers when saving.
using Kind = EncodeSetting::Kind;

const EncodeSetting kQuality{"quality", "Quality", Kind::Range, 1, 100, 90, {}, nullptr, "lossless"};
const EncodeSetting kLossless{"lossless", "Lossless", Kind::Toggle, 0, 1, 0, {}, "exact; larger"};

#ifdef HAVE_PNG
const Encoder& png_encoder()
{
    static const Encoder e{"PNG", "png",
                           {{"interlaced", "Interlaced", Kind::Toggle, 0, 1, 0, {}, "loads coarse to fine; a little larger"}},
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
                            {"progressive", "Progressive", Kind::Toggle, 0, 1, 0, {}, "loads coarse to fine"}},
                           encode_jpeg};
    return e;
}
#endif
#ifdef HAVE_TIFF
const Encoder& tiff_encoder()
{
    static const Encoder e{"TIFF", "tif", {{"compression", "Compression", Kind::Choice, 0, 2, 0, {"Deflate (ZIP)", "LZW", "None"}}},
                           encode_tiff};
    return e;
}
#endif
const Encoder& bmp_encoder()
{
    static const Encoder e{"BMP", "bmp",
                           {{"format", "Pixel format", Kind::Choice, 0, 6, 0,
                             {"Automatic (32-bit with alpha, else 24-bit)", "32-bit BGRA", "24-bit BGR", "16-bit RGB 565", "16-bit ARGB 1555",
                              "16-bit ARGB 4444", "8-bit indexed (256 colours)"}},
                            {"dither", "Dither", Kind::Toggle, 0, 1, 1, {}, "16-bit and indexed"},
                            {"srgb", "sRGB colour space tag", Kind::Toggle, 0, 1, 1},
                            {"topdown", "Top-down rows", Kind::Toggle, 0, 1, 0}},
                           encode_bmp,
                           [](const EncodeOptions& o) { return o.get("format", 0) >= 3; }};
    return e;
}
const Encoder& tga_encoder()
{
    static const Encoder e{"Targa", "tga",
                           {{"format", "Pixel format", Kind::Choice, 0, 4, 0,
                             {"Automatic (32-bit with alpha, else 24-bit)", "32-bit BGRA", "24-bit BGR", "16-bit ARGB 1555", "8-bit indexed (256 colours)"}},
                            {"dither", "Dither", Kind::Toggle, 0, 1, 1, {}, "16-bit and indexed"},
                            {"rle", "RLE compression", Kind::Toggle, 0, 1, 0},
                            {"topleft", "Top-left origin", Kind::Toggle, 0, 1, 0}},
                           encode_tga,
                           [](const EncodeOptions& o) { return o.get("format", 0) >= 3; }};
    return e;
}
#ifdef HAVE_WEBP
const Encoder& webp_encoder()
{
    static const Encoder e{"WebP", "webp",
                           {kLossless, kQuality},
                           encode_webp};
    return e;
}
#endif
#ifdef HAVE_JXL
const Encoder& jxl_encoder()
{
    static const Encoder e{"JPEG XL", "jxl",
                           {kLossless, kQuality},
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
        v.push_back(&tga_encoder());
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
