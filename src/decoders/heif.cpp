#include "decoder.h"

#include <libheif/heif.h>

#include <cstring>

namespace {

bool sniff(const uint8_t* h, size_t n) {
    if (n < 12 || std::memcmp(h + 4, "ftyp", 4) != 0) return false;
    static const char* brands[] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1", "avif", "avis", "heif"};
    for (const char* b : brands)
        if (std::memcmp(h + 8, b, 4) == 0) return true;
    return false;
}

bool decode(const std::string& path, Image& out, std::string& error) {
    heif_context* ctx = heif_context_alloc();
    heif_error err = heif_context_read_from_file(ctx, path.c_str(), nullptr);
    if (err.code != heif_error_Ok) {
        error = err.message;
        heif_context_free(ctx);
        return false;
    }
    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok) {
        error = err.message;
        heif_context_free(ctx);
        return false;
    }
    const bool alpha = heif_image_handle_has_alpha_channel(handle);
    heif_image* img = nullptr;
    err = heif_decode_image(handle, &img, heif_colorspace_RGB,
                            alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, nullptr);
    if (err.code != heif_error_Ok) {
        error = err.message;
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return false;
    }
    int stride = 0;
    const uint8_t* plane = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
    out.allocate(heif_image_get_width(img, heif_channel_interleaved), heif_image_get_height(img, heif_channel_interleaved));
    if (alpha) out.from_rgba8(plane, stride, heif_image_handle_is_premultiplied_alpha(handle));
    else out.from_rgb8(plane, stride);
    out.has_alpha = alpha;
    out.bits_per_sample = heif_image_handle_get_luma_bits_per_pixel(handle);

    uint8_t head[12] = {};
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        if (std::fread(head, 1, 12, f) == 12 && std::memcmp(head + 8, "avi", 3) == 0) out.format = "AVIF";
        std::fclose(f);
    }
    if (out.format.empty()) out.format = "HEIF";
    if (heif_image_handle_get_number_of_depth_images(handle) > 0) out.info = "has depth map";

    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return true;
}

}  // namespace

const Decoder& heif_decoder() {
    static const Decoder d{"HEIF", {"heic", "heif", "hif", "avif"}, sniff, decode};
    return d;
}
