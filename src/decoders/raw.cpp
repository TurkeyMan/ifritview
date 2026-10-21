#include "decoder.h"

#include <libraw/libraw.h>

#include <glib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

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

bool decode_full(LibRaw& raw, bool half, Image& out, std::string& error) {
    raw.imgdata.params.half_size = half ? 1 : 0;
    // Linear 16-bit, no auto-brightening: tone mapping is done by the viewer
    // so an exposure change needs no second demosaic.
    raw.imgdata.params.output_bps = 16;
    raw.imgdata.params.gamm[0] = 1.0;
    raw.imgdata.params.gamm[1] = 1.0;
    raw.imgdata.params.no_auto_bright = 1;
    const bool timing = g_getenv("IFRIT_RAW_TIMING") != nullptr;
    gint64 t0 = g_get_monotonic_time();
    auto lap = [&](const char* what) {
        if (!timing) return;
        const gint64 t = g_get_monotonic_time();
        std::fprintf(stderr, "raw: %-14s %5.0f ms\n", what, (t - t0) / 1000.0);
        t0 = t;
    };
    int r = raw.unpack();
    lap("unpack");
    if (r != LIBRAW_SUCCESS) {
        error = libraw_strerror(r);
        return false;
    }
    r = raw.dcraw_process();
    lap("process");
    if (r != LIBRAW_SUCCESS) {
        error = libraw_strerror(r);
        return false;
    }
    // After dcraw_process, imgdata.image holds output-space linear 16-bit
    // as ushort[4] per pixel; read it directly rather than via
    // dcraw_make_mem_image, and build the clip histogram in the same pass.
    const auto& sz = raw.imgdata.sizes;
    const int w = sz.iwidth, h = sz.iheight;
    const ushort (*px)[4] = raw.imgdata.image;
    if (!px || w <= 0 || h <= 0 || raw.imgdata.idata.colors < 3) {
        error = "unexpected LibRaw output";
        return false;
    }
    auto linear = std::make_shared<LinearData>();
    linear->rgb.resize(size_t(w) * h * 3);
    std::vector<uint32_t> histogram(4096, 0);
    uint16_t* d = linear->rgb.data();
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i, d += 3) {
        d[0] = px[i][0];
        d[1] = px[i][1];
        d[2] = px[i][2];
        if ((i & 3) == 0) {   // every fourth pixel is plenty for the 1% clip point
            ++histogram[d[0] >> 4];
            ++histogram[d[1] >> 4];
            ++histogram[d[2] >> 4];
        }
    }
    lap("copy+histogram");
    const size_t allow = size_t(w) * h * 3 / 4 / 100;
    size_t seen = 0;
    int bin = 4095;
    for (; bin > 0; --bin) {
        seen += histogram[bin];
        if (seen > allow) break;
    }
    linear->auto_gain = 65535.0 / ((bin + 1) * 16.0);
    out.allocate(w, h);
    out.linear = std::move(linear);
    out.retone(0.0);
    lap("retone");
    return true;
}

// The embedded JPEG, used as the on-screen stand-in while the real decode
// runs; not offered when the configured mode is the preview anyway.
bool preview(const std::string& path, Image& out) {
    if (decode_options().raw_mode == DecodeOptions::RawMode::Preview) return false;
    LibRaw raw;
    if (raw.open_file(path.c_str()) != LIBRAW_SUCCESS) return false;
    std::string error;
    if (!decode_preview(raw, out, error)) return false;
    out.format = "RAW preview";
    out.info = camera_name(raw);
    return true;
}

// LibRaw keeps no field for ExposureBiasValue (EXIF 0x9204); catch it as
// the EXIF block is parsed.
void on_exif_tag(void* context, int tag, int type, int len, unsigned int order, void* ifp, INT64) {
    if (tag != 0x9204 || type != 10 || len < 1) return;   // SRATIONAL
    auto* stream = static_cast<LibRaw_abstract_datastream*>(ifp);
    uint8_t buf[8];
    if (stream->read(buf, 8, 1) != 1) return;
    auto get32 = [&](const uint8_t* p) -> int32_t {
        return order == 0x4949 ? int32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24))
                               : int32_t((uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
    };
    const int32_t num = get32(buf), den = get32(buf + 4);
    if (den != 0) *static_cast<double*>(context) = double(num) / den;
}

bool decode_raw(const std::string& path, Image& out, std::string& error) {
    const DecodeOptions options = decode_options();
    LibRaw raw;
    double bias = 0;
    raw.set_exifparser_handler(on_exif_tag, &bias);
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

    const auto& id = raw.imgdata.idata;
    const auto& other = raw.imgdata.other;
    const auto& lens = raw.imgdata.lens;
    auto fmt = [](double v, int decimals) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
        return std::string(buf);
    };
    out.add("Camera", "Make", id.make);
    out.add("Camera", "Model", id.model);
    if (id.software[0]) out.add("Camera", "Firmware", id.software);
    if (lens.Lens[0]) out.add("Camera", "Lens", lens.Lens);
    else if (lens.LensMake[0]) out.add("Camera", "Lens make", lens.LensMake);
    if (other.iso_speed > 0) out.add("Camera", "ISO", std::to_string(int(other.iso_speed)));
    if (other.shutter > 0) out.add("Camera", "Shutter", other.shutter >= 1 ? fmt(other.shutter, 1) + " s" : "1/" + std::to_string(int(1 / other.shutter + 0.5)) + " s");
    if (other.aperture > 0) out.add("Camera", "Aperture", "f/" + fmt(other.aperture, 1));
    if (other.focal_len > 0) out.add("Camera", "Focal length", fmt(other.focal_len, 0) + " mm");
    out.exposure_bias = bias;
    if (bias != 0) out.add("Camera", "Exposure compensation", (bias > 0 ? "+" : "") + fmt(bias, 2) + " EV");
    if (other.timestamp) {
        char buf[64];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&other.timestamp));
        out.add("Camera", "Taken", buf);
    }
    if (other.shot_order) out.add("Camera", "Shot number", std::to_string(other.shot_order));
    if (other.artist[0]) out.add("Camera", "Artist", other.artist);
    if (other.desc[0]) out.add("Camera", "Description", other.desc);
    out.add("Raw", "Sensor size", std::to_string(raw.imgdata.sizes.raw_width) + " × " + std::to_string(raw.imgdata.sizes.raw_height));
    out.add("Raw", "Output size", std::to_string(out.width) + " × " + std::to_string(out.height) + " (" + mode + ")");
    if (raw.imgdata.color.raw_bps) out.add("Raw", "Bits per sample", std::to_string(raw.imgdata.color.raw_bps));
    out.add("Raw", "Colour channels", std::to_string(id.colors) + " (" + id.cdesc + ")");
    if (id.dng_version) out.add("Raw", "DNG version", std::to_string(id.dng_version >> 24) + "." + std::to_string((id.dng_version >> 16) & 0xFF));
    if (id.filters == 9) out.add("Raw", "Mosaic", "X-Trans");
    else if (id.filters) out.add("Raw", "Mosaic", "Bayer");
    else out.add("Raw", "Mosaic", "none (linear)");
    const float* wb = raw.imgdata.color.cam_mul;
    if (wb[0] > 0 && wb[1] > 0) out.add("Raw", "Camera white balance", fmt(wb[0] / wb[1], 3) + " / 1 / " + fmt(wb[2] / wb[1], 3));
    if (out.linear) out.add("Raw", "Auto exposure gain", fmt(std::log2(out.linear->auto_gain), 2) + " EV");
    return true;
}

}  // namespace

const Decoder& raw_decoder() {
    static const Decoder d{
        "RAW",
        {"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "pef", "dng", "raw",
         "rwl", "3fr", "erf", "kdc", "mef", "mos", "mrw", "x3f", "iiq", "srw", "fff", "dcr"},
        sniff, decode_raw, preview};
    return d;
}
