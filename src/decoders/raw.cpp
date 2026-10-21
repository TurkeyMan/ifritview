#include "decoder.h"

#include <libraw/libraw.h>

#include <cstring>

namespace {

bool sniff(const uint8_t* h, size_t n) {
    if (n < 16) return false;
    if (std::memcmp(h + 4, "ftypcrx ", 8) == 0) return true;      // Canon CR3
    if (std::memcmp(h, "FUJIFILMCCD-RAW", 15) == 0) return true;   // Fuji RAF
    if (std::memcmp(h, "II\x1a\0\0\0HEAPCCDR", 14) == 0) return true;  // Canon CRW
    if (std::memcmp(h, "IIRO", 4) == 0 || std::memcmp(h, "IIRS", 4) == 0) return true;  // Olympus ORF
    if (std::memcmp(h, "IIU\0", 4) == 0) return true;             // Panasonic RW2
    return false;
}

// LibRaw's flip values are dcraw's: 0 none, 3 = 180, 5 = 90 CCW, 6 = 90 CW.
int flip_to_orientation(int flip) {
    switch (flip) {
        case 3: return 3;
        case 5: return 8;
        case 6: return 6;
        default: return 1;
    }
}

std::string camera_name(const LibRaw& raw) {
    std::string make = raw.imgdata.idata.normalized_make[0] ? raw.imgdata.idata.normalized_make : raw.imgdata.idata.make;
    std::string model = raw.imgdata.idata.normalized_model[0] ? raw.imgdata.idata.normalized_model : raw.imgdata.idata.model;
    return make.empty() ? model : make + " " + model;
}

bool decode_preview(LibRaw& raw, Image& out, std::string& error) {
#ifdef HAVE_JPEG
    int r = raw.unpack_thumb();
    if (r != LIBRAW_SUCCESS) {
        error = libraw_strerror(r);
        return false;
    }
    libraw_processed_image_t* thumb = raw.dcraw_make_mem_thumb(&r);
    if (!thumb) {
        error = libraw_strerror(r);
        return false;
    }
    bool ok = false;
    if (thumb->type == LIBRAW_IMAGE_JPEG) {
        ok = decode_jpeg_memory(thumb->data, thumb->data_size, out, error);
        // Embedded previews are stored unrotated; apply the camera orientation.
        if (ok && out.width >= out.height) out.apply_orientation(flip_to_orientation(raw.imgdata.sizes.flip));
    } else if (thumb->type == LIBRAW_IMAGE_BITMAP && thumb->colors == 3 && thumb->bits == 8) {
        out.allocate(thumb->width, thumb->height);
        out.from_rgb8(thumb->data, thumb->width * 3);
        out.apply_orientation(flip_to_orientation(raw.imgdata.sizes.flip));
        ok = true;
    } else {
        error = "unsupported embedded preview type";
    }
    LibRaw::dcraw_clear_mem(thumb);
    return ok;
#else
    (void)raw; (void)out;
    error = "embedded previews need JPEG support";
    return false;
#endif
}

// Same rule as LibRaw's auto-brightness: the gain that pushes 1% of all
// samples past white.
double auto_gain(const std::vector<uint16_t>& rgb) {
    std::vector<uint32_t> histogram(4096, 0);
    for (uint16_t v : rgb) ++histogram[v >> 4];
    const size_t allow = rgb.size() / 100;
    size_t seen = 0;
    int bin = 4095;
    for (; bin > 0; --bin) {
        seen += histogram[bin];
        if (seen > allow) break;
    }
    const double white = (bin + 1) * 16.0;
    return white > 0 ? 65535.0 / white : 1.0;
}

bool decode_full(LibRaw& raw, bool half, Image& out, std::string& error) {
    raw.imgdata.params.half_size = half ? 1 : 0;
    // Linear 16-bit, no auto-brightening: tone mapping is done by the viewer
    // so an exposure change needs no second demosaic.
    raw.imgdata.params.output_bps = 16;
    raw.imgdata.params.gamm[0] = 1.0;
    raw.imgdata.params.gamm[1] = 1.0;
    raw.imgdata.params.no_auto_bright = 1;
    int r = raw.unpack();
    if (r != LIBRAW_SUCCESS) {
        error = libraw_strerror(r);
        return false;
    }
    r = raw.dcraw_process();
    if (r != LIBRAW_SUCCESS) {
        error = libraw_strerror(r);
        return false;
    }
    libraw_processed_image_t* img = raw.dcraw_make_mem_image(&r);
    if (!img) {
        error = libraw_strerror(r);
        return false;
    }
    bool ok = false;
    if (img->type == LIBRAW_IMAGE_BITMAP && img->colors == 3 && img->bits == 16) {
        auto linear = std::make_shared<LinearData>();
        const auto* px = reinterpret_cast<const uint16_t*>(img->data);
        linear->rgb.assign(px, px + size_t(img->width) * img->height * 3);
        linear->auto_gain = auto_gain(linear->rgb);
        out.allocate(img->width, img->height);
        out.linear = std::move(linear);
        out.retone(0.0);
        ok = true;
    } else {
        error = "unexpected LibRaw output";
    }
    LibRaw::dcraw_clear_mem(img);
    return ok;
}

bool decode_raw(const std::string& path, Image& out, std::string& error) {
    const DecodeOptions options = decode_options();
    LibRaw raw;
    raw.imgdata.params.use_camera_wb = 1;
    raw.imgdata.params.output_color = 1;  // sRGB
    int r = raw.open_file(path.c_str());
    if (r != LIBRAW_SUCCESS) {
        error = libraw_strerror(r);
        return false;
    }

    bool ok = false;
    std::string mode;
    using RawMode = DecodeOptions::RawMode;
    if (options.raw_mode == RawMode::Preview) {
        ok = decode_preview(raw, out, error);
        mode = "embedded preview";
    }
    if (!ok) {
        const bool half = options.raw_mode != RawMode::Full;
        ok = decode_full(raw, half, out, error);
        mode = half ? "half size" : "full size";
    }
    if (!ok) return false;

    out.bits_per_sample = raw.imgdata.color.raw_bps ? raw.imgdata.color.raw_bps : 14;
    std::string make = raw.imgdata.idata.normalized_make[0] ? raw.imgdata.idata.normalized_make : raw.imgdata.idata.make;
    out.format = make.empty() ? "RAW" : make + " RAW";
    out.format += ", " + mode;
    out.info = camera_name(raw);
    return true;
}

}  // namespace

const Decoder& raw_decoder() {
    static const Decoder d{
        "RAW",
        {"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "pef", "dng", "raw",
         "rwl", "3fr", "erf", "kdc", "mef", "mos", "mrw", "x3f", "iiq", "srw", "fff", "dcr"},
        sniff, decode_raw};
    return d;
}
