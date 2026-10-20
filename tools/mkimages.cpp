// Generates a folder of test images in every format we can encode with the
// libraries already linked, so the viewer can be exercised without hunting
// for sample files. Build via the `mkimages` CMake target.
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <webp/encode.h>
#include <jxl/encode.h>
#include <libheif/heif.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> gradient(int w, int h, bool alpha, int seed) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            const bool check = ((x / 32) + (y / 32)) % 2 == 0;
            p[0] = uint8_t(255 * x / std::max(1, w - 1));
            p[1] = uint8_t(255 * y / std::max(1, h - 1));
            p[2] = uint8_t((seed * 60 + (check ? 200 : 60)) & 0xFF);
            p[3] = alpha ? uint8_t(std::clamp(int(255.0 * std::hypot(x - w / 2.0, y - h / 2.0) / (w / 2.0)), 0, 255)) : 255;
            // a 1px border so edges are visible against any background
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) { p[0] = p[1] = p[2] = 255; p[3] = 255; }
        }
    return px;
}

static GdkPixbuf* to_pixbuf(const std::vector<uint8_t>& px, int w, int h, bool alpha) {
    GdkPixbuf* pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, alpha, 8, w, h);
    guchar* dst = gdk_pixbuf_get_pixels(pb);
    const int stride = gdk_pixbuf_get_rowstride(pb);
    const int n = alpha ? 4 : 3;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) std::memcpy(dst + y * stride + x * n, &px[(size_t(y) * w + x) * 4], n);
    return pb;
}

static void save_pixbuf(const std::vector<uint8_t>& px, int w, int h, bool alpha, const std::string& path, const char* type) {
    GdkPixbuf* pb = to_pixbuf(px, w, h, alpha);
    GError* err = nullptr;
    if (!gdk_pixbuf_save(pb, path.c_str(), type, &err, nullptr)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err ? err->message : "?");
        if (err) g_error_free(err);
    } else {
        std::printf("wrote %s\n", path.c_str());
    }
    g_object_unref(pb);
}

static void write_file(const std::string& path, const void* data, size_t size) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(data, 1, size, f);
    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
}

static void save_webp(const std::vector<uint8_t>& px, int w, int h, const std::string& path) {
    uint8_t* out = nullptr;
    size_t size = WebPEncodeRGBA(px.data(), w, h, w * 4, 80, &out);
    if (size) write_file(path, out, size);
    WebPFree(out);
}

static void save_jxl(const std::vector<uint8_t>& px, int w, int h, const std::string& path) {
    JxlEncoder* enc = JxlEncoderCreate(nullptr);
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = w;
    info.ysize = h;
    info.num_color_channels = 3;
    info.num_extra_channels = 1;
    info.alpha_bits = 8;
    info.bits_per_sample = 8;
    info.uses_original_profile = JXL_FALSE;
    JxlEncoderSetBasicInfo(enc, &info);
    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    JxlEncoderSetColorEncoding(enc, &color);
    JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc, nullptr);
    JxlPixelFormat fmt{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlEncoderAddImageFrame(fs, &fmt, px.data(), px.size());
    JxlEncoderCloseInput(enc);
    std::vector<uint8_t> out(1 << 20);
    uint8_t* next = out.data();
    size_t avail = out.size();
    JxlEncoderStatus st;
    while ((st = JxlEncoderProcessOutput(enc, &next, &avail)) == JXL_ENC_NEED_MORE_OUTPUT) {
        size_t used = next - out.data();
        out.resize(out.size() * 2);
        next = out.data() + used;
        avail = out.size() - used;
    }
    if (st == JXL_ENC_SUCCESS) write_file(path, out.data(), next - out.data());
    else std::fprintf(stderr, "jxl encode failed\n");
    JxlEncoderDestroy(enc);
}

static void save_heif(const std::vector<uint8_t>& px, int w, int h, const std::string& path, heif_compression_format comp) {
    heif_context* ctx = heif_context_alloc();
    heif_encoder* enc = nullptr;
    heif_error e = heif_context_get_encoder_for_format(ctx, comp, &enc);
    if (e.code) {
        std::fprintf(stderr, "%s: no encoder (%s)\n", path.c_str(), e.message);
        heif_context_free(ctx);
        return;
    }
    heif_encoder_set_lossy_quality(enc, 70);
    heif_image* img = nullptr;
    heif_image_create(w, h, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, &img);
    heif_image_add_plane(img, heif_channel_interleaved, w, h, 8);
    int stride = 0;
    uint8_t* plane = heif_image_get_plane(img, heif_channel_interleaved, &stride);
    for (int y = 0; y < h; ++y) std::memcpy(plane + y * stride, &px[size_t(y) * w * 4], w * 4);
    heif_image_handle* handle = nullptr;
    e = heif_context_encode_image(ctx, img, enc, nullptr, &handle);
    if (e.code) std::fprintf(stderr, "%s: encode failed (%s)\n", path.c_str(), e.message);
    else {
        e = heif_context_write_to_file(ctx, path.c_str());
        if (e.code) std::fprintf(stderr, "%s: write failed (%s)\n", path.c_str(), e.message);
        else std::printf("wrote %s\n", path.c_str());
    }
    if (handle) heif_image_handle_release(handle);
    heif_image_release(img);
    heif_encoder_release(enc);
    heif_context_free(ctx);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mkimages <output-dir>\n");
        return 1;
    }
    const std::string dir = std::string(argv[1]) + "/";
    g_mkdir_with_parents(argv[1], 0755);

    struct Size { const char* tag; int w, h; };
    const Size sizes[] = {{"tiny", 16, 16}, {"small", 320, 200}, {"medium", 1280, 800}, {"tall", 600, 1900}, {"huge", 6000, 4000}};
    int seed = 0;
    for (const Size& s : sizes) {
        const auto rgb = gradient(s.w, s.h, false, seed++);
        const auto rgba = gradient(s.w, s.h, true, seed++);
        const std::string base = dir + s.tag + "_" + std::to_string(s.w) + "x" + std::to_string(s.h);
        save_pixbuf(rgb, s.w, s.h, false, base + ".jpg", "jpeg");
        save_pixbuf(rgba, s.w, s.h, true, base + "_alpha.png", "png");
        if (s.w <= 1280) {
            save_pixbuf(rgb, s.w, s.h, false, base + ".bmp", "bmp");
            save_pixbuf(rgb, s.w, s.h, false, base + ".tiff", "tiff");
            save_webp(rgba, s.w, s.h, base + "_alpha.webp");
            save_jxl(rgba, s.w, s.h, base + "_alpha.jxl");
            save_heif(rgb, s.w, s.h, base + ".heic", heif_compression_HEVC);
            save_heif(rgb, s.w, s.h, base + ".avif", heif_compression_AV1);
        }
    }
    return 0;
}
