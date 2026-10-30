#include "../bytes.h"
#include "../i18n.h"
#include "../lens_database.h"
#include "../gpu.h"
#include "../parallel.h"
#include "../platform.h"
#include "crx.h"
#include "decoder.h"
#include "dng_opcodes.h"
#include "dngtiles.h"
#include "fuji.h"
#include "ljpeg.h"
#include "olympus.h"
#include "pana8.h"
#include "phase_one.h"
#include "rawblocks.h"
#include "rawpacked.h"

#include <libraw/libraw.h>

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <ctime>
#include <thread>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    if (n < 16)
        return false;
    if (std::memcmp(h + 4, "ftypcrx ", 8) == 0)
        return true;  // Canon CR3
    if (std::memcmp(h, "FUJIFILMCCD-RAW", 15) == 0)
        return true;  // Fuji RAF
    if (std::memcmp(h, "II\x1a\0\0\0HEAPCCDR", 14) == 0)
        return true;  // Canon CRW
    if (std::memcmp(h, "IIRO", 4) == 0 || std::memcmp(h, "IIRS", 4) == 0)
        return true;  // Olympus ORF
    if (std::memcmp(h, "IIU\0", 4) == 0)
        return true;  // Panasonic RW2
    if (std::memcmp(h + 8, "IIIICwaR", 8) == 0)
        return true;  // Phase One's, IIQ or named .tif
    return false;
}

// LibRaw's flip values are dcraw's: 0 none, 3 = 180, 5 = 90 CCW, 6 = 90 CW.
int flip_to_orientation(int flip)
{
    switch (flip)
    {
        case 3:
            return 3;
        case 5:
            return 8;
        case 6:
            return 6;
        default:
            return 1;
    }
}

std::string camera_name(const LibRaw& raw)
{
    std::string make = raw.imgdata.idata.normalized_make[0] ? raw.imgdata.idata.normalized_make : raw.imgdata.idata.make;
    std::string model = raw.imgdata.idata.normalized_model[0] ? raw.imgdata.idata.normalized_model : raw.imgdata.idata.model;
    return make.empty() ? model : make + " " + model;
}

// How it was taken, as lensfun's database is matched by (lens_database.h).
lenses::Shot shot_of(const LibRaw& raw)
{
    const auto& id = raw.imgdata.idata;
    const auto& lens = raw.imgdata.lens;
    lenses::Shot s;
    s.make = id.normalized_make[0] ? id.normalized_make : id.make;
    s.model = id.normalized_model[0] ? id.normalized_model : id.model;
    s.lens = lens.Lens[0] ? lens.Lens : lens.makernotes.Lens;
    s.focal = raw.imgdata.other.focal_len;
    s.aperture = raw.imgdata.other.aperture;
    s.lens_min_focal = lens.makernotes.MinFocal > 0 ? lens.makernotes.MinFocal : lens.MinFocal;
    s.lens_max_focal = lens.makernotes.MaxFocal > 0 ? lens.makernotes.MaxFocal : lens.MaxFocal;
    return s;
}

bool decode_preview(LibRaw& raw, Image& out, std::string& error, int fit_w = 0, int fit_h = 0)
{
#ifdef HAVE_JPEG
    int r = raw.unpack_thumb();
    if (r != LIBRAW_SUCCESS)
    {
        error = libraw_strerror(r);
        return false;
    }
    libraw_processed_image_t* thumb = raw.dcraw_make_mem_thumb(&r);
    if (!thumb)
    {
        error = libraw_strerror(r);
        return false;
    }
    bool ok = false;
    if (thumb->type == LIBRAW_IMAGE_JPEG)
    {
        ok = decode_jpeg_memory(thumb->data, thumb->data_size, out, error, fit_w, fit_h);
        // Embedded previews are stored unrotated; apply the camera orientation.
        if (ok && out.width >= out.height)
            out.apply_camera_orientation(flip_to_orientation(raw.imgdata.sizes.flip));
    }
    else if (thumb->type == LIBRAW_IMAGE_BITMAP && thumb->colors == 3 && thumb->bits == 8)
    {
        out.allocate(thumb->width, thumb->height);
        out.from_rgb8(thumb->data, thumb->width * 3);
        out.apply_camera_orientation(flip_to_orientation(raw.imgdata.sizes.flip));
        ok = true;
    }
    else
    {
        error = "unsupported embedded preview type";
    }
    LibRaw::dcraw_clear_mem(thumb);
    return ok;
#else
    (void)raw;
    (void)out;
    error = "embedded previews need JPEG support";
    return false;
#endif
}

// The camera's own framing: the sensor readout carries a border the camera
// never shows (its JPEG is cut from the "inset" rectangle). Apply that
// rectangle as LibRaw's crop box so the develop matches the JPEG's framing.
bool apply_camera_crop(LibRaw& raw)
{
    if (!decode_options().raw_camera_crop)
        return false;
    const auto& sz = raw.imgdata.sizes;
    const auto& c = sz.raw_inset_crops[0];
    if (c.cwidth == 0 || c.cheight == 0 || c.cleft == 0xFFFF || c.ctop == 0xFFFF)
        return false;
    // The inset is in raw-frame coordinates; the crop box is relative to the active area.
    const int left = int(c.cleft) - int(sz.left_margin), top = int(c.ctop) - int(sz.top_margin);
    if (left < 0 || top < 0 || left + int(c.cwidth) > int(sz.width) || top + int(c.cheight) > int(sz.height))
        return false;
    if (c.cwidth == sz.width && c.cheight == sz.height)
        return false;  // nothing to crop
    raw.imgdata.params.cropbox[0] = unsigned(left);
    raw.imgdata.params.cropbox[1] = unsigned(top);
    raw.imgdata.params.cropbox[2] = c.cwidth;
    raw.imgdata.params.cropbox[3] = c.cheight;
    return true;
}

// Derive the camera's tone curve from its own JPEG: match the luminance
// distribution of our linear render to the preview's. Brightness and
// contrast then agree with what the camera showed; colour rendering is
// left for the real tonemapper.
std::vector<float> match_tone_to_preview(const LinearData& linear, const Image& preview)
{
    std::vector<uint32_t> hist_preview(256, 0), hist_linear(4096, 0);
    for (int y = 0; y < preview.height; ++y)
    {
        const uint32_t* p = preview.row(y);
        for (int x = 0; x < preview.width; ++x)
        {
            const uint32_t v = p[x];
            const int luma = int(0.2126 * ((v >> 16) & 0xFF) + 0.7152 * ((v >> 8) & 0xFF) + 0.0722 * (v & 0xFF) + 0.5);
            ++hist_preview[std::clamp(luma, 0, 255)];
        }
    }
    const uint16_t* s = linear.rgb.data();
    const size_t n = linear.rgb.size() / 3;
    for (size_t i = 0; i < n; i += 2, s += 6)
    {
        const double lum = (0.2126 * s[0] + 0.7152 * s[1] + 0.0722 * s[2]) / 65535.0;
        ++hist_linear[std::clamp(int(lum * 4095.0 + 0.5), 0, 4095)];
    }
    double total_p = 0, total_l = 0;
    for (uint32_t c : hist_preview)
        total_p += c;
    for (uint32_t c : hist_linear)
        total_l += c;
    if (total_p == 0 || total_l == 0)
        return {};

    auto srgb_decode = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    std::vector<float> tone(4096);
    double cdf_l = 0, cdf_p = hist_preview[0] / total_p;
    int p = 0;
    for (int b = 0; b < 4096; ++b)
    {
        cdf_l += hist_linear[b] / total_l;
        while (p < 255 && cdf_p < cdf_l)
            cdf_p += hist_preview[++p] / total_p;
        tone[b] = float(srgb_decode(p / 255.0));
    }
    // Beyond the brightest input seen, keep heading to white.
    for (int b = 1; b < 4096; ++b)
        tone[b] = std::max(tone[b], tone[b - 1]);
    return tone;
}

bool decode_full(LibRaw& raw, bool half, Image& out, std::string& error)
{
    raw.imgdata.params.half_size = half ? 1 : 0;
    // Linear 16-bit, no auto-brightening: tone mapping is done by the viewer
    // so an exposure change needs no second demosaic.
    raw.imgdata.params.output_bps = 16;
    raw.imgdata.params.gamm[0] = 1.0;
    raw.imgdata.params.gamm[1] = 1.0;
    raw.imgdata.params.no_auto_bright = 1;
    const bool framed = apply_camera_crop(raw);
    int r = raw.unpack();
    if (r != LIBRAW_SUCCESS)
    {
        error = libraw_strerror(r);
        return false;
    }
    r = raw.dcraw_process();
    if (r != LIBRAW_SUCCESS)
    {
        error = libraw_strerror(r);
        return false;
    }
    // After dcraw_process, imgdata.image holds output-space linear 16-bit
    // as ushort[4] per pixel; read it directly rather than via
    // dcraw_make_mem_image, and build the clip histogram in the same pass.
    // It's width x height, as copy_mem_image reads it: a SuperCCD's turned
    // upright, or a raw stretched to square pixels, leaves iwidth and
    // iheight as they were.
    const auto& sz = raw.imgdata.sizes;
    const int w = sz.width, h = sz.height;
    const ushort(*px)[4] = raw.imgdata.image;
    if (!px || w <= 0 || h <= 0 || raw.imgdata.idata.colors < 3)
    {
        error = "unexpected LibRaw output";
        return false;
    }
    auto linear = std::make_shared<LinearData>();
    linear->rgb.resize(size_t(w) * h * 3);
    std::vector<uint32_t> histogram(4096, 0);
    uint16_t* d = linear->rgb.data();
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i, d += 3)
    {
        d[0] = px[i][0];
        d[1] = px[i][1];
        d[2] = px[i][2];
        if ((i & 3) == 0)
        {  // every fourth pixel is plenty for the 1% clip point
            ++histogram[d[0] >> 4];
            ++histogram[d[1] >> 4];
            ++histogram[d[2] >> 4];
        }
    }
    const size_t allow = size_t(w) * h * 3 / 4 / 100;
    size_t seen = 0;
    int bin = 4095;
    for (; bin > 0; --bin)
    {
        seen += histogram[bin];
        if (seen > allow)
            break;
    }
    linear->auto_gain = 65535.0 / ((bin + 1) * 16.0);
#ifdef HAVE_JPEG
    {
        Image preview;
        std::string ignored;
        if (decode_preview(raw, preview, ignored) && preview.width > 0)
            linear->tone = match_tone_to_preview(*linear, preview);
    }
#endif
    out.allocate(w, h);
    out.linear = std::move(linear);
    out.retone(0.0);
    // dcraw_process leaves the sensor's orientation (only LibRaw's image
    // export turns it): turned as the camera held it, as the preview is.
    out.apply_camera_orientation(flip_to_orientation(raw.imgdata.sizes.flip));
    if (framed)
    {
        const auto& c = raw.imgdata.sizes.raw_inset_crops[0];
        out.add(N_("Raw"), N_("Camera framing"),
                std::to_string(c.cwidth) + " × " + std::to_string(c.cheight) + " at (" + std::to_string(c.cleft) + ", " + std::to_string(c.ctop) + ")");
    }
    return true;
}

// LibRaw with the step of its unpack that reads the black level from the
// sensor's masked border, for a readout unpacked here instead.
class RawAccess : public LibRaw
{
public:
    // LibRaw's loader for this file's readout, as get_decoder_info names it
    // ("nikon_load_raw()"); empty before one's chosen.
    std::string loader() const
    {
        libraw_decoder_info_t info{};
        return const_cast<RawAccess*>(this)->get_decoder_info(&info) == LIBRAW_SUCCESS && info.decoder_name ? info.decoder_name : "";
    }
    using LibRaw::is_phaseone_compressed;  // any of Phase One's loaders, compressed or not
    // A Phase One's (its loaders') black and corrections of its readout,
    // which LibRaw makes in raw2image, not as it unpacks: for the develop
    // to make (phase_one.h). Once unpacked; empty for others.
    dng::Corrections phase_one_corrections(const uint8_t* file, size_t size)
    {
        if (!is_phaseone_compressed() || !imgdata.rawdata.raw_image)
            return {};
        const auto& sz = imgdata.sizes;
        const auto& ph1 = imgdata.color.phase_one_data;
        phase_one::Sensor s;
        s.file = file, s.size = size;
        s.meta = size_t(libraw_internal_data.unpacker_data.meta_offset), s.meta_length = size_t(libraw_internal_data.unpacker_data.meta_length);
        s.raw_w = sz.raw_width, s.raw_h = sz.raw_height;
        s.left = sz.left_margin, s.top = sz.top_margin, s.width = sz.width, s.height = sz.height;
        s.split[0] = ph1.split_col, s.split[1] = ph1.split_row;
        s.black = ph1.t_black;
        s.temperature = ph1.tag_210;
        s.row_black = imgdata.rawdata.ph1_cblack, s.column_black = imgdata.rawdata.ph1_rblack;
        for (int i = 0; i < 4; ++i)
            s.filter[i] = int(imgdata.idata.filters >> (((i >> 1 << 1 & 14) | (i & 1)) << 1) & 3);  // FC(row, col)
        // LibRaw's own spline, into its curve (put back after).
        std::vector<ushort> kept(imgdata.color.curve, imgdata.color.curve + 0x10000);
        s.spline = [this](const int* x, const int* y, int n, uint16_t table[65536])
        {
            cubic_spline(x, y, n);
            std::copy_n(imgdata.color.curve, 0x10000, table);
        };
        dng::Corrections c = phase_one::corrections(s);
        std::copy(kept.begin(), kept.end(), imgdata.color.curve);
        return c;
    }
    // IFRIT_GPU_CORRECTIONS_CHECK: a Phase One's active area as LibRaw
    // corrects it (raw2image's first steps), once unpacked.
    bool phase_one_corrected(std::vector<uint16_t>& out)
    {
        if (!is_phaseone_compressed() || !imgdata.rawdata.raw_alloc)
            return false;
        phase_one_allocate_tempbuffer();
        int rc = phase_one_subtract_black(static_cast<ushort*>(imgdata.rawdata.raw_alloc), imgdata.rawdata.raw_image);
        if (rc == 0 && imgdata.params.use_p1_correction)
            rc = phase_one_correct();
        const auto& sz = imgdata.sizes;
        if (rc == 0)
        {
            out.resize(size_t(sz.width) * sz.height);
            for (unsigned y = 0; y < sz.height; ++y)
                std::memcpy(out.data() + size_t(y) * sz.width, imgdata.rawdata.raw_image + size_t(y + sz.top_margin) * sz.raw_width + sz.left_margin,
                            size_t(sz.width) * 2);
        }
        phase_one_free_tempbuffer();
        return rc == 0;
    }
    // A Nikon NEF's compressed readout as LibRaw's nikon_load_raw reads it:
    // the version, the tree for it and its depth, the first rows' starting
    // samples, the curve. False for others, and for a stream that changes
    // tree partway down ("split"): LibRaw unpacks those.
    bool nikon(const uint8_t* file, size_t size, ljpeg::Prepared& p, std::string& error, bool check_only = false) const
    {
        static const uint8_t trees[6][32] = {
            {0, 1, 5, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 5, 4, 3, 6, 2, 7, 1, 0, 8, 9, 11, 10, 12},  // 12-bit lossy
            {0, 1, 5, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 0x39, 0x5a, 0x38, 0x27, 0x16, 5, 4, 3, 2, 1, 0, 11, 12, 12},  // after a split
            {0, 1, 4, 2, 3, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 4, 6, 3, 7, 2, 8, 1, 9, 0, 10, 11, 12},  // 12-bit lossless
            {0, 1, 4, 3, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 0, 5, 6, 4, 7, 8, 3, 9, 2, 1, 0, 10, 11, 12, 13, 14},  // 14-bit lossy
            {0, 1, 5, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0, 8, 0x5c, 0x4b, 0x3a, 0x29, 7, 6, 5, 4, 3, 2, 1, 0, 13, 14},  // after a split
            {0, 1, 4, 2, 2, 3, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 7, 6, 8, 5, 9, 4, 10, 3, 11, 12, 2, 0, 1, 13, 14}};  // 14-bit lossless
        if (loader() != "nikon_load_raw()")
            return false;
        const auto& u = libraw_internal_data.unpacker_data;
        const bool le = u.order == 0x4949;
        const auto get2 = [&](size_t at) -> uint16_t
        { return at + 2 > size ? 0 : le ? uint16_t(file[at] | file[at + 1] << 8) : uint16_t(file[at] << 8 | file[at + 1]); };
        size_t at = size_t(u.meta_offset);
        if (at + 2 > size || size_t(u.data_offset) >= size)
            return false;
        const uint8_t ver0 = file[at], ver1 = file[at + 1];
        at += 2;
        if (ver0 == 0x49 || ver1 == 0x58)
            at += 2110;
        int tree = ver0 == 0x46 ? 2 : 0;
        if (u.tiff_bps == 14)
            tree += 3;
        const uint16_t start[2][2] = {{get2(at), get2(at + 2)}, {get2(at + 4), get2(at + 6)}};
        if (ver0 == 0x44 && (ver1 == 0x20 || ver1 == 0x40) && get2(size_t(u.meta_offset) + 562))
        {
            error = "a split Nikon stream";
            return false;
        }
        if (check_only)
            return true;
        const auto& sz = imgdata.sizes;
        return ljpeg::prepare_nikon(file + u.data_offset, size - size_t(u.data_offset), sz.raw_width, sz.height, trees[tree], start,
                                    imgdata.color.curve, 0x4000, p, error);
    }
    // A Pentax PEF's compressed readout as LibRaw's pentax_load_raw reads
    // it: its codes from the maker notes (how many, each one's bits and
    // length), the whole readout's rows. False for others.
    bool pentax(const uint8_t* file, size_t size, ljpeg::Prepared& p, std::string& error, bool check_only = false) const
    {
        if (loader() != "pentax_load_raw()")
            return false;
        const auto& u = libraw_internal_data.unpacker_data;
        const FileReader r{file, size, u.order != 0x4949};
        const size_t at = size_t(u.meta_offset);
        const int n = (r.u16(at) + 12) & 15;
        if (u.zero_after_ff || !r.fits(at, 14 + 3 * size_t(n)) || u.data_offset <= 0 || size_t(u.data_offset) >= size)
            return error = "a Pentax readout laid out another way", false;
        if (check_only)
            return true;
        uint16_t codes[15];
        uint8_t lengths[15];
        for (int c = 0; c < n; ++c)
            codes[c] = r.u16(at + 14 + 2 * c), lengths[c] = file[at + 14 + 2 * n + c];
        const auto& sz = imgdata.sizes;
        return ljpeg::prepare_pentax(file + u.data_offset, size - size_t(u.data_offset), sz.raw_width, sz.raw_height, int(u.tiff_bps), codes,
                                     lengths, n, p, error);
    }
    // An Olympus's or OM System's compressed readout, as LibRaw's
    // olympus_load_raw reads it (olympus.h). False for others.
    bool olympus(const uint8_t* file, size_t size, olympus::Prepared& p, std::string& error) const
    {
        if (loader() != "olympus_load_raw()")
            return false;
        const auto& o = imgdata.makernotes.olympus;
        const unsigned tags[14] = {o.tagX640, o.tagX641, o.tagX642, o.tagX643, o.tagX644, o.tagX645, o.tagX646,
                                   o.tagX647, o.tagX648, o.tagX649, o.tagX650, o.tagX651, o.tagX652, o.tagX653};
        const auto& u = libraw_internal_data.unpacker_data;
        if (u.data_offset < 0)
            return error = "no Olympus header", false;
        return olympus::prepare(file, size, uint64_t(u.data_offset), imgdata.sizes.raw_width, imgdata.sizes.raw_height, tags, p, error);
    }

    // A readout stored uncompressed, as LibRaw's packed_dng_load_raw (a DNG's
    // strips: rows whole bytes, 16-bit samples in the file's byte order, the
    // rest most significant bit first; its linearisation curve),
    // unpacked_load_raw (16-bit words in the file's order, shifted down) and
    // packed_load_raw (a stream of words of 8 to 32 bits, little-endian,
    // read most significant bit first; rows padded) and fuji_14bit_load_raw
    // (14-bit samples in 32-bit little-endian words, rows whole 28 bytes)
    // read it. False for
    // others, and for what they read some other way (tiles, two samples a
    // photosite, interlaced rows, a padding byte every ten samples, a Fuji's
    // turned sensor) or past the file's end: LibRaw unpacks those.
    bool packed(const uint8_t* file, size_t size, rawpacked::Prepared& p, std::string& error) const
    {
        const std::string name = loader();
        const auto& u = libraw_internal_data.unpacker_data;
        const auto& sz = imgdata.sizes;
        const bool le = u.order == 0x4949;
        if (name != "packed_dng_load_raw()" && name != "unpacked_load_raw()" && name != "packed_load_raw()" && name != "fuji_14bit_load_raw()")
            return false;
        if (samples() != 1)
            return error = "more than a sample a photosite", false;
        if (super_ccd() || sz.left_margin + sz.width > sz.raw_width ||
            sz.top_margin + sz.height > sz.raw_height)
            return error = "a readout laid out another way", false;
        p.readout_w = sz.raw_width, p.readout_h = sz.raw_height;
        p.bits = int(u.tiff_bps);
        if (name == "packed_dng_load_raw()")
        {
            if (u.tile_length < INT_MAX || u.tiff_samples != 1 || u.zero_after_ff)
                return error = "a DNG's readout in tiles, or more than a sample a photosite", false;
            p.group = p.bits == 16 && le ? 2 : 1;
            p.row_bits = (uint32_t(sz.raw_width) * p.bits + 7) / 8 * 8;  // each row from a fresh byte
            const ushort* curve = imgdata.color.curve;
            for (int i = 0; i < 0x10000; ++i)
                if (curve[i] != i)
                {
                    p.curve.assign(curve, curve + 0x10000);
                    break;
                }
        }
        else if (name == "unpacked_load_raw()")
        {
            if (u.load_flags >= 16)
                return error = "an odd shift", false;
            p.bits = 16;
            p.group = le ? 2 : 1;
            p.row_bits = uint32_t(sz.raw_width) * 16;
            p.shift = int(u.load_flags);
        }
        else if (name == "fuji_14bit_load_raw()")
        {
            if (sz.raw_width % 16)
                return error = "rows not whole 28 bytes", false;
            p.bits = 14;
            p.group = 4;
            p.row_bits = uint32_t(sz.raw_width) * 14;
        }
        else
        {
            const unsigned flags = u.load_flags;
            if (flags & 3)
                return error = "interlaced rows, or padding bytes", false;
            unsigned row_bytes = unsigned(sz.raw_width) * u.tiff_bps / 8;
            row_bytes += row_bytes & flags >> 7;
            if (row_bytes * 8 < unsigned(sz.raw_width) * u.tiff_bps)
                return error = "rows shorter than their samples", false;
            p.row_bits = row_bytes * 8;
            p.group = int(8 + (flags & 24)) / 8;
            p.swap = flags >> 6 & 1;
            if (p.swap && sz.raw_width % 2)
                return error = "an odd width swapped in pairs", false;
        }
        if (p.bits < 1 || p.bits > 16)
            return error = "samples of " + std::to_string(p.bits) + " bits", false;
        const uint64_t bits = uint64_t(p.row_bits) * sz.raw_height;
        p.bytes = size_t((bits + 7) / 8 + p.group - 1) / p.group * p.group;
        p.offset = size_t(u.data_offset);
        if (bits >= (uint64_t(1) << 32) || u.data_offset < 0 || p.offset > size || p.bytes > size - p.offset)
            return error = "a readout past the file's end", false;
        p.file = file;
        return true;
    }
    // Whether LibRaw's crop_masked_pixels measures the sides of the active
    // area (a packed readout's flag says so).
    bool packed_sides() const { return loader() == "packed_load_raw()" && libraw_internal_data.unpacker_data.load_flags & 32; }
    // A filter of three colours the GPU's develop demosaics: Bayer's 2 x 2
    // (LibRaw's filters, the other G its own) or X-Trans's 6 x 6 (filters 9).
    bool developed_pattern() const
    {
        const auto& id = imgdata.idata;
        return (id.filters >= 1000 || id.filters == 9) && id.colors == 3 && !super_ccd();
    }
    // A DNG carrying a colour matrix known to be wrong, given its sensor's
    // from LibRaw's table instead. Samsung's GX20 firmware (a Pentax K20D)
    // writes one that drains the colour (as LibRaw's table notes): the light
    // its own as-shot neutral implies lies far off the blackbody locus
    // (Duv -0.010 against the K20D's -0.001), and Adobe's profile for it is
    // the K20D's. Known by the matrix itself, so a file with another one
    // keeps it. Once opened.
    void fix_known_bad_matrix()
    {
        const auto& id = imgdata.idata;
        const auto& daylight = imgdata.color.dng_color[1].colormatrix;
        const bool samsung_gx20 = id.dng_version && std::strcmp(id.make, "Samsung") == 0 && std::strcmp(id.model, "GX20") == 0 &&
                                  std::fabs(daylight[0][0] - 2.7717f) < 1e-3f && std::fabs(daylight[0][1] + 1.7403f) < 1e-3f &&
                                  std::fabs(daylight[1][1] - 1.9778f) < 1e-3f;
        if (samsung_gx20)
            adobe_coeff(LIBRAW_CAMERAMAKER_Pentax, "K20D");
    }
    // Fuji's SuperCCD: its photosites on a grid turned 45 degrees, which
    // LibRaw's develop turns upright (fuji_rotate); the readout isn't the
    // picture's grid.
    bool super_ccd() const { return libraw_internal_data.internal_output_params.fuji_width != 0; }
    // Samples a photosite in the readout: one (a mosaic, a monochrome
    // sensor), or a linear DNG's colours.
    unsigned samples() const { return imgdata.idata.filters ? 1 : imgdata.idata.colors; }
    // A DNG's tiles as LibRaw's loaders step through them (`tile_length`
    // INT_MAX: its one strip, a tile the readout's size): how many across,
    // and where each one's data starts, the words from data_offset on (as
    // get4 reads them). None, with why, for any past the file's end.
    std::vector<uint64_t> tiles(const uint8_t* file, size_t size, unsigned& across, std::string& error) const
    {
        const auto& u = libraw_internal_data.unpacker_data;
        const auto& sz = imgdata.sizes;
        const bool tiled = u.tile_length < INT_MAX;
        if (!sz.raw_width || !sz.raw_height || !u.tile_width || !u.tile_length || u.data_offset < 0)
            return {};
        across = tiled ? (sz.raw_width + u.tile_width - 1) / u.tile_width : 1;
        const unsigned down = tiled ? (sz.raw_height + u.tile_length - 1) / u.tile_length : 1;
        if (!tiled)
            return {uint64_t(u.data_offset)};
        const FileReader r{file, size, u.order != 0x4949};
        if (!r.fits(size_t(u.data_offset), size_t(across) * down * 4))
            return error = "tile offsets past the file's end", std::vector<uint64_t>{};
        std::vector<uint64_t> offsets(size_t(across) * down);
        for (size_t k = 0; k < offsets.size(); ++k)
            offsets[k] = r.u32(size_t(u.data_offset) + 4 * k);
        return offsets;
    }
    // A DNG's lossless JPEG readout (Compression 7) as LibRaw's
    // lossless_dng_load_raw reads it: its tiles' streams (or its one
    // strip's), each a frame's photosites in rows `layout.tile[0]` wide
    // filling a tile `tile[1]` high, `tile[2]` tiles across, a sample each
    // or a linear DNG's three (a JPEG component each), and its
    // linearisation curve. Or Sony's lossless ARW as sony_ljpeg_load_raw
    // reads it: its tiles' streams of four components, each JPEG sample a
    // 2 x 2 of photosites (`layout.quads`). False for others, and for
    // anything that wouldn't come out as LibRaw has it (two samples a
    // photosite, a DNG before 1.1 (a 16-bit difference read as bits), tiles
    // that aren't their frames' size, a strip short of the readout, Sony's
    // tiles not whole): LibRaw unpacks those. `check_only`: the first
    // stream's header only, for its predictor.
    bool ljpeg_tiles(const uint8_t* file, size_t size, ljpeg::Prepared& p, RawMosaic::Stream& layout, std::string& error, bool check_only = false) const
    {
        const std::string name = loader();
        const bool sony = name == "sony_ljpeg_load_raw()";
        if (!sony && name != "lossless_dng_load_raw()")
            return false;
        const auto& u = libraw_internal_data.unpacker_data;
        const auto& sz = imgdata.sizes;
        const auto& id = imgdata.idata;
        const unsigned n = samples();
        if (u.tiff_samples != n || (n != 1 && n != 3))
            return error = "not a sample a photosite, nor three", false;
        if (!sony && id.dng_version < 0x01010000)
            return error = "a DNG before 1.1", false;
        const unsigned rw = sz.raw_width, rh = sz.raw_height;
        const bool tiled = u.tile_length < INT_MAX;
        if (sony && (n != 1 || !tiled || !u.tile_width || rw % u.tile_width || rh % u.tile_length))
            return error = "Sony's tiles not whole", false;
        unsigned across = 1;
        const std::vector<uint64_t> offsets = tiles(file, size, across, error);
        if (offsets.empty())
            return false;
        int predictor = 0;
        if (offsets[0] >= size || !ljpeg::takes(file + offsets[0], size_t(size - offsets[0]), &predictor))
            return error = "not a lossless JPEG taken here", false;
        if (check_only)
        {
            p.predictor = predictor;
            return true;
        }
        if (!ljpeg::prepare_tiles(file, size, offsets, p, error))
            return false;
        // A JPEG row's samples fill the tile's rows (the readout's, if
        // narrower) one after another, as adobe_copy_pixel places them: a
        // linear DNG's a pixel (its components) at a time. Sony's each fill
        // two rows, a sample's components a 2 x 2.
        const unsigned wrap = std::min(u.tile_width, rw);
        const uint64_t jwide = uint64_t(p.width) * p.components, frame = jwide * p.height;
        if (sony && (p.components != 4 || uint64_t(p.width) * 2 != u.tile_width || uint64_t(p.height) * 2 != u.tile_length))
            return error = "Sony's tiles not a 2 x 2 a sample", false;
        if (!sony && n == 1 && p.components > 1 && jwide * p.components == rw)
            return error = "a JPEG LibRaw reads past its rows", false;
        if (n > 1 && unsigned(p.components) != n)
            return error = "a JPEG of other than a component a colour", false;
        uint64_t held = 0;  // a strip's samples, in all its restart intervals
        for (const auto& part : p.parts)
            held += jwide * part.rows;
        if (tiled ? frame != uint64_t(wrap) * u.tile_length * n : held < uint64_t(rw) * rh * n)
            return error = "tiles not their frames' size", false;
        layout.tile[0] = wrap, layout.tile[1] = tiled ? u.tile_length : rh, layout.tile[2] = across;
        layout.quads = sony;
        const ushort* curve = imgdata.color.curve;  // a DNG's; Sony's samples go in as they are
        for (int v = 0; !sony && v < 0x10000; ++v)
            if (curve[v] != v)
            {
                p.curve.assign(curve, curve + 0x10000);
                break;
            }
        return true;
    }

    // Sony's YCbCr ARW (its medium and small sizes) as sony_ycbcr_load_raw
    // reads it: tiles of lossless JPEG, the luma 2 x 2 or 2 x 1 (ljpeg.h's
    // YCbCr). False for others; and, with why, for what LibRaw wouldn't read so.
    bool sony_ycc(const uint8_t* file, size_t size, ljpeg::Prepared& p, std::string& error) const
    {
        if (loader() != "sony_ycbcr_load_raw()")
            return false;
        const auto& u = libraw_internal_data.unpacker_data;
        const auto& sz = imgdata.sizes;
        if (u.tile_width < 1 || u.tile_width > sz.raw_width || u.tile_length < 1 || u.tile_length > sz.raw_height || u.tile_width % 2 || u.tile_length % 2)
            return error = "Sony's YCbCr not in whole tiles", false;
        unsigned across = 1;
        const std::vector<uint64_t> offsets = tiles(file, size, across, error);
        if (offsets.empty() || offsets.size() > 1024)
            return error = "Sony's YCbCr without its tiles", false;
        if (!ljpeg::prepare_tiles(file, size, offsets, p, error, 0, true))
            return false;
        if (p.luma < 2 || p.components != p.luma + 2 || p.precision < 12 || unsigned(p.width) * 2 != u.tile_width ||
            unsigned(p.height * p.luma / 2) != u.tile_length)
            return error = "Sony's YCbCr tiles not as LibRaw reads them", false;
        return true;
    }
    // Their pixels into RGB as LibRaw's ycc2rgb makes them (BT.601's, in
    // floats, the colours about 16383), raw_width x raw_height, three a
    // photosite; and the levels sony_ycbcr_load_raw sets.
    bool unpack_ycc(const ljpeg::Prepared& p, std::vector<uint16_t>& out, std::string& error)
    {
        ljpeg::Image j;
        if (!ljpeg::decode(p, j, error))
            return false;
        const auto& u = libraw_internal_data.unpacker_data;
        const unsigned rw = imgdata.sizes.raw_width, rh = imgdata.sizes.raw_height, tw = u.tile_width, th = u.tile_length;
        const unsigned across = (rw + tw - 1) / tw;
        const size_t tile = size_t(tw) * th * 3;
        if (unsigned(j.width) != tw || unsigned(j.height) != th || j.samples.size() < tile * across * ((rh + th - 1) / th))
            return error = "unexpected layout", false;
        const auto held = [](float q) { return uint16_t(uint32_t(q < 0.f ? 0.f : q > 65535.f ? 65535.f : q)); };
        out.resize(size_t(rw) * rh * 3);
        parallel(rh,
                 [&](unsigned y)
                 {
                     uint16_t* o = out.data() + size_t(y) * rw * 3;
                     for (unsigned x = 0; x < rw; ++x, o += 3)
                     {
                         const uint16_t* s = j.samples.data() + (size_t(y / th) * across + x / tw) * tile + (size_t(y % th) * tw + x % tw) * 3;
                         const float luma = float(s[0]), cb = float(s[1] - 16383), cr = float(s[2] - 16383);
                         o[0] = held(luma + 1.40200f * cr);
                         o[1] = held(luma - 0.34414f * cb - 0.71414f * cr);
                         o[2] = held(luma + 1.77200f * cb);
                     }
                 });
        auto& col = imgdata.color;
        std::fill_n(col.cblack, 6, 0u);
        col.maximum = 17536;
        col.black = 1024;
        return true;
    }

    // A DNG's readout LibRaw reads a tile after another (dngtiles.h): a
    // lossy DNG's JPEG tiles (lossy_dng_load_raw: the readout's corner the
    // active area's size, through the curves its MapPolynomial opcodes give
    // each colour, sRGB's without them), floats deflated or stored as they
    // are (deflate_dng_load_raw, uncompressed_fp_dng_load_raw); or JPEG XL
    // tiles, which it reads only with the DNG SDK (a linearisation table
    // not taken: nothing to say how they'd go together). False for others,
    // and, with why, for what LibRaw would read another way.
    // (Not const: LibRaw's sRGB curve is made in its curve, put back after.)
    bool dng_tiles(const uint8_t* file, size_t size, dngtiles::Prepared& p, std::string& error)
    {
        const std::string name = loader();
        const auto& u = libraw_internal_data.unpacker_data;
        const auto& sz = imgdata.sizes;
        const unsigned n = samples();
        p.file = file, p.size = size;
        p.samples = n;
        p.tile_w = u.tile_length < INT_MAX ? u.tile_width : sz.raw_width;
        p.tile_h = u.tile_length < INT_MAX ? u.tile_length : sz.raw_height;
        if (name == "lossy_dng_load_raw()")
        {
            if (n != 1 && n != 3)
                return error = "a lossy DNG of " + std::to_string(n) + " colours", false;
            p.format = dngtiles::Format::lossy;
            p.width = sz.width, p.height = sz.height;
            // Each colour's curve as LibRaw makes it: its MapPolynomial
            // (the plane it names, the area and pitch whatever they are).
            p.curves.assign(4 * 256, 0);
            bool have[4] = {};
            if (u.meta_offset && size_t(u.meta_offset) < size)
                dng::each_opcode(file + u.meta_offset, size - size_t(u.meta_offset), [&](uint32_t id, uint32_t, const uint8_t* d, size_t bytes)
                {
                    if (id != 8)
                        return true;
                    const FileReader r{d, bytes, true};
                    const uint32_t c = r.u32(16), degree = r.u32(32);
                    if (c > 3 || degree > 8)
                        return false;
                    double coeff[9];
                    for (uint32_t j = 0; j <= degree; ++j)
                        coeff[j] = r.f64(36 + 8 * j);
                    for (int v = 0; v < 256; ++v)
                    {
                        double total = 0;
                        for (uint32_t j = 0; j <= degree; ++j)
                            total += coeff[j] * std::pow(v / 255.0, int(j));
                        p.curves[c * 256 + v] = uint16_t(std::clamp(total * 0xffff, 0.0, 65535.0));
                    }
                    have[c] = true;
                    return true;
                });
            else
            {
                std::vector<ushort> kept(imgdata.color.curve, imgdata.color.curve + 0x10000);
                gamma_curve(1 / 2.4, 12.92, 1, 255);
                for (int c = 0; c < 4; ++c)
                    std::copy_n(imgdata.color.curve, 256, p.curves.begin() + c * 256), have[c] = true;
                std::copy(kept.begin(), kept.end(), imgdata.color.curve);
            }
            if (!std::all_of(have, have + n, [](bool h) { return h; }))
                return error = "a lossy DNG without a curve for each colour", false;
        }
        else if (name == "deflate_dng_load_raw()" || name == "uncompressed_fp_dng_load_raw()" || name == "jxl_dng_load_raw_placeholder()")
        {
            const tiff_ifd_t* ifd = nullptr;  // the raw's, as LibRaw's find_ifd_by_offset finds it
            for (unsigned i = 0; i < libraw_internal_data.identify_data.tiff_nifds && i < LIBRAW_IFD_MAXCOUNT; ++i)
                if (tiff_ifd[i].offset == u.data_offset)
                {
                    ifd = &tiff_ifd[i];
                    break;
                }
            const bool jxl = name == "jxl_dng_load_raw_placeholder()", deflated = name == "deflate_dng_load_raw()";
            if (!ifd || ifd->samples != int(n) || u.tiff_samples != n || (n != 1 && n != 3))
                return error = "other than a sample a photosite or three", false;
            // Tiles as LibRaw's loaders for floats take them, as tiles() does.
            const bool tiled = u.tile_width <= sz.raw_width && u.tile_length <= sz.raw_height;
            if (tiled != (u.tile_length < INT_MAX) || (ifd->rows_per_strip > 0 && ifd->rows_per_strip < int(sz.raw_height) && ifd->strip_byte_counts_count > 0))
                return error = "strips", false;
            p.width = sz.raw_width, p.height = sz.raw_height;
            p.big_endian = u.order != 0x4949;
            if (jxl)
            {
                const ushort* curve = imgdata.color.curve;
                for (int v = 0; v < 0x10000; ++v)
                    if (curve[v] != v)
                        return error = "JPEG XL with a linearisation table", false;
                p.format = dngtiles::Format::jxl;
            }
            else
            {
                if (ifd->sample_format != 3)
                    return error = "integers deflated", false;
                p.format = deflated ? dngtiles::Format::deflated : dngtiles::Format::floats;
                p.bytes = (ifd->bps + 7) / 8;
                p.factor = !deflated ? 1 : ifd->predictor == 34894 ? 2 : ifd->predictor == 34895 ? 4 : 1;
                if ((deflated && ifd->bps % 8) || p.bytes < 2 || p.bytes > 4 || p.tile_w % p.factor)
                    return error = "floats of " + std::to_string(ifd->bps) + " bits", false;
            }
            p.offsets = tiles(file, size, p.across, error);
            // How long each tile is: a table of them where there are more than one.
            const FileReader r{file, size, p.big_endian};
            if (p.offsets.size() > 1 && !r.fits(size_t(ifd->bytes), p.offsets.size() * 4))
                return error = "tile lengths past the file's end", false;
            for (size_t k = 0; k < p.offsets.size(); ++k)
                p.lengths.push_back(p.offsets.size() > 1 ? r.u32(size_t(ifd->bytes) + 4 * k) : uint64_t(ifd->bytes));
            return !p.offsets.empty();
        }
        else
            return false;
        p.offsets = tiles(file, size, p.across, error);
        return !p.offsets.empty();
    }
    // The readout of dng_tiles' tiles (raw.samples() a photosite) and the
    // levels with it, as LibRaw's unpack leaves them: a lossy DNG's, the
    // active area alone with nothing over 65535; floats', as its
    // convertFloatToInt makes them (its defaults): scaled to 16383 at the
    // greatest where that's outside 4096..32767, the levels with them, held
    // at 0 and truncated. JPEG XL's as they are.
    bool unpack_tiles(const dngtiles::Prepared& p, std::vector<uint16_t>& out, std::string& error)
    {
        auto& col = imgdata.color;
        if (p.format == dngtiles::Format::jxl)
            return dngtiles::decode(p, out, error);
        if (p.format == dngtiles::Format::lossy)
        {
            if (!dngtiles::decode(p, out, error))
                return false;
            auto& sz = imgdata.sizes;
            col.maximum = 0xffff;
            sz.raw_width = sz.width, sz.raw_height = sz.height;
            sz.left_margin = sz.top_margin = 0;
            return true;
        }
        std::vector<float> floats;
        float largest = 0;
        if (!dngtiles::decode(p, floats, largest, error))
            return false;
        const auto most = [](float a, float b) { return a > b ? a : b; };  // LibRaw's MAX, NaN and all
        float top = most(most(float(std::max(col.maximum, 1u)), largest), 1.f), scale = 1.f;
        col.fmaximum = largest;
        col.fnorm = 0;
        if (top < 4096.f || top > 32767.f)
        {
            col.fnorm = scale = 16383.f / top;
            col.maximum = 16383;
            col.black = unsigned(float(col.black) * scale);
            for (int i = 0; i < LIBRAW_CBLACK_SIZE; ++i)
                if (i != 4 && i != 5)
                    col.cblack[i] = unsigned(float(col.cblack[i]) * scale);
        }
        out.resize(floats.size());
        const size_t row = size_t(p.width) * p.samples;
        parallel(p.height,
                 [&](size_t y)
                 {
                     for (size_t i = y * row; i < (y + 1) * row; ++i)
                         out[i] = uint16_t(most(floats[i], 0.f) * scale);
                 });
        return true;
    }

    // A raw compressed in blocks (rawblocks.h): Sony's compressed ARW,
    // Panasonic's RW2 but for encoding 8. False for others; and, with why,
    // for a layout LibRaw would read some other way than rawblocks does.
    bool blocks(const uint8_t* file, size_t size, rawblocks::Prepared& p, std::string& error) const
    {
        const std::string name = loader();
        const auto& u = libraw_internal_data.unpacker_data;
        const auto& sz = imgdata.sizes;
        p.readout_w = sz.raw_width, p.readout_h = sz.raw_height;
        p.bits = u.pana_bpp;
        if (name == "sony_arw2_load_raw()")
        {
            if (u.order != 0x4949 || imgdata.rawparams.specials & LIBRAW_RAWSPECIAL_SONYARW2_ALLFLAGS)
                return error = "an ARW read another way", false;
            p.format = rawblocks::Format::sony;
            p.rows = sz.height;
        }
        else if (name == "panasonic_load_raw()")
        {
            p.format = u.pana_encoding == 5 ? rawblocks::Format::packed : rawblocks::Format::panasonic;
            p.rows = sz.raw_height;
            p.rotated = true;
            p.rotation = u.load_flags;
        }
        else if (name == "panasonicC6_load_raw()" || name == "panasonicC7_load_raw()")
        {
            // Read 16 rows at a time, a last few short of 16 not at all.
            const bool c6 = name == "panasonicC6_load_raw()";
            p.format = c6 ? rawblocks::Format::panasonic6 : rawblocks::Format::packed;
            if (c6)
                p.bits = u.pana_bpp == 12 ? 12 : 14;
            p.rows = sz.raw_height / 16 * 16;
        }
        else
            return false;
        return rawblocks::prepare(file, size, uint64_t(u.data_offset), imgdata.color.curve, p, error);
    }

    // Panasonic's encoding 8 (pana8.h) from the tags LibRaw read. False for
    // others; and, with why, for what pana8 doesn't take.
    bool pana8(const uint8_t* file, size_t size, pana8::Prepared& p, std::string& error) const
    {
        if (loader() != "panasonicC8_load_raw()")
            return false;
        const pana8_tags_t& t = libraw_internal_data.unpacker_data.pana8;
        pana8::Header h;
        std::copy_n(t.tag39, 6, h.curve_shift);
        std::copy_n(t.tag3A, 6, h.curve_start);
        h.top = t.tag3B;
        std::copy_n(t.initial, 4, h.initial);
        std::copy_n(t.tag40a, 17, h.code_length);
        std::copy_n(t.tag40b, 17, h.code);
        std::copy_n(t.tag41, 17, h.shift);
        h.stripes = t.stripe_count;
        for (int i = 0; i < std::min(h.stripes, 5); ++i)
        {
            h.offset[i] = t.stripe_offsets[i], h.left[i] = t.stripe_left[i], h.bits[i] = t.stripe_compressed_size[i];
            h.width[i] = t.stripe_width[i], h.height[i] = t.stripe_height[i];
        }
        return pana8::prepare(file, size, h, imgdata.sizes.raw_width, imgdata.sizes.raw_height, p, error);
    }

    // A CR3's raw track (crx.h), the one LibRaw chose. False for others; and,
    // with why, for one crx doesn't take.
    bool crx(const uint8_t* file, size_t size, crx::Prepared& p, std::string& error) const
    {
        if (loader() != "crxLoadRaw()")
            return false;
        const auto& u = libraw_internal_data.unpacker_data;
        if (u.crx_track_selected < 0 || u.crx_track_selected >= LIBRAW_CRXTRACKS_MAXCOUNT || u.data_offset < 0 || u.data_size < 0)
            return error = "no CR3 track chosen", false;
        const crx_data_header_t& d = u.crx_header[u.crx_track_selected];
        if (d.f_width != int(imgdata.sizes.raw_width) || d.f_height != int(imgdata.sizes.raw_height))
            return error = "a readout laid out another way", false;
        crx::Header h;
        h.version = d.version;
        h.width = d.f_width, h.height = d.f_height;
        h.tile_w = d.tileWidth, h.tile_h = d.tileHeight;
        h.bits = d.nBits;
        h.planes = d.nPlanes, h.cfa = d.cfaLayout;
        h.encoding = d.encType, h.levels = d.imageLevels;
        h.header_size = uint32_t(d.mdatHdrSize);
        h.offset = uint64_t(u.data_offset), h.size = uint64_t(u.data_size);
        return crx::prepare(file, size, h, p, error);
    }

    // A Fuji's compressed RAF (fuji.h), as LibRaw's
    // parse_fuji_compressed_header read its header. False for others; and,
    // with why, for one fuji doesn't take.
    bool fuji(const uint8_t* file, size_t size, fuji::Prepared& p, std::string& error) const
    {
        if (loader() != "fuji_compressed_load_raw()")
            return false;
        const auto& u = libraw_internal_data.unpacker_data;
        const auto& id = imgdata.idata;
        fuji::Header h;
        h.bits = u.fuji_bits;
        h.lossless = u.fuji_lossless != 0;
        h.xtrans = u.fuji_raw_type == 16;
        if (h.xtrans != (id.filters == 9))
            return error = "a pattern other than the header's", false;
        h.block_width = u.fuji_block_width;
        h.blocks = u.fuji_total_blocks;
        h.lines = u.fuji_total_lines;
        h.readout_w = imgdata.sizes.raw_width, h.readout_h = imgdata.sizes.raw_height;
        h.offset = uint64_t(u.data_offset);
        for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 6; ++c)
            {
                const int f = h.xtrans ? id.xtrans_abs[r][c] : int(id.filters >> ((((r << 1) & 14) | (c & 1)) << 1) & 3);  // FC(row, col)
                h.colour[r][c] = f == 3 ? 1 : f;
            }
        return fuji::prepare(file, size, h, p, error);
    }

    // A Canon sRAW's conversion as LibRaw's canon_sraw_load_raw makes it:
    // which formula and hue for this camera and firmware, and sraw_mul.
    void canon_ycc(RawMosaic::Ycc& y) const
    {
        const unsigned long long id = libraw_internal_data.identify_data.unique_id;
        const bool coefficients = id == 0x80000218ULL || id == 0x80000250ULL || id == 0x80000261ULL || id == 0x80000232ULL ||
                                  id == 0x80000287ULL;  // 5D Mark II, 7D, 50D, 1D Mark IV, 60D
        y.formula = coefficients ? 1 : 2;
        y.luma_offset = !coefficients && id < 0x80000218ULL ? 512 : 0;
        int v[3] = {0, 0, 0};
        const char* cp = imgdata.color.model2;
        while (*cp && !std::isdigit(static_cast<unsigned char>(*cp)))
            ++cp;
        std::sscanf(cp, "%d.%d.%d", v, v + 1, v + 2);
        const int ver = (v[0] * 1000 + v[1]) * 1000 + v[2];
        const int sraw = y.block_h == 2 ? 3 : 1;  // the first component's samples, less one
        y.hue = id >= 0x80000281ULL || (id == 0x80000218ULL && ver > 1000006) ? sraw << 1 : (sraw + 1) << 2;
        for (int c = 0; c < 3; ++c)
            y.mul[c] = libraw_internal_data.unpacker_data.sraw_mul[c] ? libraw_internal_data.unpacker_data.sraw_mul[c] / 1024.f : 1.f;
    }
    // The areas crop_masked_pixels measures, for a lossless JPEG readout
    // (LibRaw 0.22's rule), for the GPU to measure instead.
    // A readout for the GPU: its size, where the active area sits, the
    // filter pattern, and where it's masked (masked_areas' `sides`, `inset`).
    std::shared_ptr<RawMosaic::Stream> readout_stream(bool sides, bool inset = true) const
    {
        const auto& sz = imgdata.sizes;
        auto st = std::make_shared<RawMosaic::Stream>();
        st->readout_w = sz.raw_width, st->readout_h = sz.raw_height;
        st->left = sz.left_margin, st->top = sz.top_margin;
        st->filters = imgdata.idata.filters;
        masked_areas(st->mask, sides, inset);
        return st;
    }
    void masked_areas(int m[8][4], bool sides, bool inset = true) const
    {
        const auto& sz = imgdata.sizes;
        std::memcpy(m, sz.mask, sizeof sz.mask);
        if (m[0][3] > 0 || !sides)  // the sides measured for Canon's lossless JPEG readouts, not Nikon's
            return;
        if (inset)  // two columns in, Canon's lossless JPEG's (not a packed readout's)
        {
            m[0][1] = m[1][1] += 2;
            m[0][3] -= 2;
        }
        m[0][0] = m[1][0] = sz.top_margin;
        m[0][2] = m[1][2] = sz.top_margin + sz.height;
        m[0][3] += sz.left_margin;
        m[1][1] += sz.left_margin + sz.width;
        m[1][3] += sz.raw_width;
    }
    // As LibRaw's unpack leaves them (0.22's): measured where the sensor is
    // masked in `readout` (raw_width x raw_height; a sample a photosite
    // only), and the least of the four channels' levels then moved into the
    // common one.
    void black_from_masked(std::vector<uint16_t>& readout)
    {
        auto& sz = imgdata.sizes;
        auto& col = imgdata.color;
        if (samples() == 1)
        {
            sz.raw_pitch = sz.raw_width * 2;
            imgdata.rawdata.raw_image = readout.data();
            crop_masked_pixels();
            imgdata.rawdata.raw_image = nullptr;  // ours, not LibRaw's to free
        }
        const unsigned least = std::min({col.cblack[0], col.cblack[1], col.cblack[2], col.cblack[3]});
        for (int c = 0; c < 4; ++c)
            col.cblack[c] -= least;
        col.black += least;
    }
};

// Canon's CR2: the sensor's readout is a lossless JPEG, the fourth IFD's
// strip, its rows cut into vertical slices (tag 0xC640: how many slices, each
// one's width, the last one's). Decoded here on several threads (ljpeg.h)
// into the readout as LibRaw lays it out, raw_width x raw_height. False for
// anything else (or anything odd), which LibRaw unpacks.
// Where a CR2's readout is: its lossless JPEG's offset and length, and the
// slices. False for anything else.
bool locate_cr2(const uint8_t* file, size_t size, uint32_t& offset, uint32_t& length, unsigned slices[3])
{
    if (size < 16 || std::memcmp(file + 8, "CR\x02", 3) != 0)
        return false;
    const bool le = file[0] == 'I';
    const auto u16 = [&](size_t at) -> uint32_t { return at + 2 > size ? 0 : le ? file[at] | file[at + 1] << 8 : file[at] << 8 | file[at + 1]; };
    const auto u32 = [&](size_t at) -> uint32_t { return at + 4 > size ? 0 : le ? u16(at) | u16(at + 2) << 16 : u16(at) << 16 | u16(at + 2); };
    size_t ifd = u32(4);
    for (int k = 0; k < 3 && ifd; ++k)
        ifd = u32(ifd + 2 + 12 * u16(ifd));
    if (!ifd || ifd + 2 > size)
        return false;
    offset = length = 0;
    slices[0] = slices[1] = slices[2] = 0;
    for (uint32_t k = 0, n = u16(ifd); k < n; ++k)
    {
        const size_t e = ifd + 2 + 12 * k;
        const uint32_t tag = u16(e), type = u16(e + 2);
        const uint32_t value = type == 3 ? u16(e + 8) : u32(e + 8);
        if (tag == 0x111)
            offset = value;
        else if (tag == 0x117)
            length = value;
        else if (tag == 0xC640 && u32(e + 4) == 3)
            for (int j = 0; j < 3; ++j)
                slices[j] = u16(u32(e + 8) + 2 * j);
    }
    return offset && length && uint64_t(offset) + length <= size;
}

// The readout unpacked here, on every core, as LibRaw's loader would: a raw
// in blocks (rawblocks), one stored uncompressed (rawpacked), a CR3 (crx), a
// Fuji's compressed RAF (fuji), Panasonic's stripes (pana8), an ORF
// (olympus.h: its codes on one thread), a NEF's, a PEF's, a DNG's, Sony's
// (Bayer or YCbCr) or a CR2's lossless JPEG; raw.samples() a photosite.
// False for others, and (with why) for one of those laid out in a way this
// doesn't take. `blocks`: a raw in blocks' layout, which says what of the
// readout LibRaw writes (the rest it leaves as it is).
bool unpack_here(RawAccess& raw, const uint8_t* file, size_t size, std::vector<uint16_t>& out, std::string& error,
                 std::shared_ptr<rawblocks::Prepared>* blocks = nullptr)
{
    error.clear();
    if (auto b = std::make_shared<rawblocks::Prepared>(); raw.blocks(file, size, *b, error))
    {
        rawblocks::decode(*b, out);
        if (blocks)
            *blocks = std::move(b);
        return true;
    }
    if (rawpacked::Prepared packed; raw.packed(file, size, packed, error))
    {
        rawpacked::unpack(packed, out);
        return true;
    }
    if (crx::Prepared cr3; error.empty() && raw.crx(file, size, cr3, error))
    {
        std::vector<uint16_t> planes;
        if (!crx::decode(cr3, planes, error))
            return false;
        crx::readout(cr3, planes, out);
        return true;
    }
    if (fuji::Prepared raf; error.empty() && raw.fuji(file, size, raf, error))
    {
        fuji::decode(raf, out);
        return true;
    }
    if (pana8::Prepared stripes; error.empty() && raw.pana8(file, size, stripes, error))
        return pana8::decode(stripes, out, error);
    if (olympus::Prepared orf; error.empty() && raw.olympus(file, size, orf, error))
        return olympus::decode(orf, out, error);
    if (!error.empty())
        return false;
    uint32_t offset, length;
    unsigned slices[3];
    RawMosaic::Stream st;  // where a tiled readout's photosites lie, as the GPU's would
    // A NEF or a PEF: its rows as they come (raw_width wide; a NEF's `height`
    // of them, the rest of the readout none).
    if (ljpeg::Prepared nef; raw.nikon(file, size, nef, error) || (error.empty() && raw.pentax(file, size, nef, error)))
    {
        ljpeg::Image j;
        if (!ljpeg::decode(nef, j, error))
            return false;
        const auto& sz = raw.imgdata.sizes;
        if (size_t(j.width) * j.components != sz.raw_width || j.height > sz.raw_height)
            return error = "unexpected layout", false;
        out.assign(size_t(sz.raw_width) * sz.raw_height, 0);
        std::copy(j.samples.begin(), j.samples.end(), out.begin());
        return true;
    }
    // A DNG's or Sony's tiles: each one's rows into its place, what lies
    // past the readout's edges dropped.
    if (ljpeg::Prepared tiled; raw.ljpeg_tiles(file, size, tiled, st, error))
    {
        ljpeg::Image j;
        if (!ljpeg::decode(tiled, j, error))
            return false;
        const unsigned rw = raw.imgdata.sizes.raw_width, rh = raw.imgdata.sizes.raw_height, n = raw.samples(), wrap = st.tile[0];
        out.resize(size_t(rw) * rh * n);
        parallel(rh,
                 [&](unsigned y)
                 {
                     for (unsigned x = 0; x < rw; x += wrap)  // a tile's row at a time
                     {
                         uint16_t* o = out.data() + (size_t(y) * rw + x) * n;
                         const uint16_t* from = j.samples.data() + st.index(x, y) * n;
                         if (st.quads)  // two of each JPEG sample's four (whole tiles)
                             for (unsigned i = 0; i < wrap; i += 2)
                                 o[i] = from[2 * i], o[i + 1] = from[2 * i + 1];
                         else
                             std::memcpy(o, from, size_t(std::min(wrap, rw - x)) * n * 2);
                     }
                 });
        return true;
    }
    // Sony's YCbCr: its tiles' pixels, then RGB.
    if (ljpeg::Prepared ycc; error.empty() && raw.sony_ycc(file, size, ycc, error))
        return raw.unpack_ycc(ycc, out, error);
    // A DNG's lossy JPEG tiles, or its floats: a tile a thread.
    if (dngtiles::Prepared t; error.empty() && raw.dng_tiles(file, size, t, error))
        return raw.unpack_tiles(t, out, error);
    if (!error.empty())
        return false;
    if (!locate_cr2(file, size, offset, length, slices))
        return false;
    ljpeg::Image j;
    if (!ljpeg::decode(file + offset, length, j, error))
        return false;
    const auto& sz = raw.imgdata.sizes;
    const unsigned rw = sz.raw_width, rh = sz.raw_height;
    const size_t jwide = size_t(j.width) * j.components;
    if (uint64_t(jwide) * j.height != uint64_t(rw) * rh || (slices[0] && (!slices[1] || !slices[2])))
    {
        error = "unexpected layout";
        return false;
    }
    if (slices[0] && size_t(slices[0]) * slices[1] + slices[2] != rw)
    {
        error = "slices not the readout's width";
        return false;
    }
    out.resize(size_t(rw) * rh);
    // As LibRaw's lossless_jpeg_load_raw: the stream fills each slice top to
    // bottom, slice after slice.
    const uint16_t* from = j.samples.data();
    parallel(rh,
             [&](unsigned row)
             {
                 if (!slices[0])
                 {
                     std::memcpy(out.data() + size_t(row) * rw, from + size_t(row) * rw, size_t(rw) * 2);
                     return;
                 }
                 for (unsigned i = 0; i <= slices[0]; ++i)
                 {
                     const unsigned w = i < slices[0] ? slices[1] : slices[2];
                     std::memcpy(out.data() + size_t(row) * rw + size_t(i) * slices[1], from + size_t(i) * slices[1] * rh + size_t(row) * w, size_t(w) * 2);
                 }
             });
    return true;
}

// IFRIT_RAW_CHECK: a raw unpack_here takes unpacked both ways, ours and
// LibRaw's, and every sample compared (and what LibRaw knows only after
// unpacking).
void check_unpack(const uint8_t* file, size_t size, const std::string& path)
{
    // On the heap: a LibRaw is hundreds of KB, and a Windows thread's stack 1 MB.
    auto mine_ptr = std::make_unique<RawAccess>(), theirs_ptr = std::make_unique<RawAccess>();
    RawAccess &mine = *mine_ptr, &theirs = *theirs_ptr;
    if (mine.open_buffer(file, size) != LIBRAW_SUCCESS || theirs.open_buffer(file, size) != LIBRAW_SUCCESS)
        return;
    std::vector<uint16_t> ours;
    std::string error;
    const gint64 t0 = g_get_monotonic_time();
    std::shared_ptr<rawblocks::Prepared> blocks;  // where it's one of these, what LibRaw writes (the rest it leaves as it finds it)
    const bool ok = unpack_here(mine, file, size, ours, error, &blocks);
    const unsigned n = mine.samples();
    if (ok)
        mine.black_from_masked(ours);
    const gint64 t1 = g_get_monotonic_time();
    const std::string loader = theirs.loader();
    if (const int r = theirs.unpack(); r != LIBRAW_SUCCESS)
    {
        const std::string verdict = ok ? "ours alone" : "not ours: " + error;
        std::fprintf(stderr, "rawcheck: %s %s; ours %.0f ms, LibRaw's unpack fails (%s, %s)\n", path.c_str(), verdict.c_str(), (t1 - t0) / 1000.0,
                     loader.c_str(), libraw_strerror(r));
        return;
    }
    const gint64 t2 = g_get_monotonic_time();
    const auto& sz = theirs.imgdata.sizes;
    const auto& rd = theirs.imgdata.rawdata;
    // LibRaw's readout: a sample a photosite, or three or four (a linear DNG's colours).
    const ushort* raw = rd.raw_image ? rd.raw_image : rd.color3_image ? rd.color3_image[0] : rd.color4_image ? rd.color4_image[0] : nullptr;
    const unsigned stride = rd.raw_image ? 1 : rd.color3_image ? 3 : 4;
    std::string verdict = ok ? "same" : "not ours: " + error;
    if (ok && raw && (stride == 1) == (n == 1) && ours.size() == size_t(sz.raw_width) * sz.raw_height * n)
        for (unsigned y = 0; y < sz.raw_height && verdict == "same"; ++y)
            for (unsigned x = 0; x < sz.raw_width * n; ++x)
            {
                const uint16_t mine_v = ours[size_t(y) * sz.raw_width * n + x];
                const uint16_t their_v = raw[size_t(y) * sz.raw_pitch / 2 + x / n * stride + x % n];
                if (mine_v != their_v && (!blocks || rawblocks::written(*blocks, int(x), int(y))))
                {
                    verdict = "DIFFERENT at " + std::to_string(x / n) + "," + std::to_string(y) + (n > 1 ? " (" + std::to_string(x % n) + ")" : "") +
                              ": ours " + std::to_string(mine_v) + ", LibRaw's " + std::to_string(their_v);
                    break;
                }
            }
    else if (ok)
        verdict = "NOT COMPARED: LibRaw's readout laid out otherwise";
    const auto& a = mine.imgdata.color;
    const auto& b = theirs.imgdata.color;
    const bool levels = a.black == b.black && a.maximum == b.maximum && std::memcmp(a.cblack, b.cblack, 4 * sizeof a.cblack[0]) == 0;
    libraw_decoder_info_t info{};
    theirs.get_decoder_info(&info);
    // Whether the GPU's develop would take it (decode_mosaic's test), and if not why.
    const auto& id = theirs.imgdata.idata;
    const char* gpu = !id.filters ? (rd.float3_image || rd.float4_image ? "no: float, demosaiced" : rd.float_image ? "no: float mosaic"
                                     : (rd.color3_image || rd.color4_image) && id.colors == 3     ? (ok ? "yes: linear, here too" : "yes: linear or sRAW")
                                     : rd.color3_image || rd.color4_image                         ? "no: demosaiced, not three colours"
                                     : id.colors == 1 && rd.raw_image                             ? (ok ? "yes: monochrome, here too" : "yes: monochrome")
                                                                                                    : "no: no colour filter")
                      : id.filters < 1000 && id.filters != 9 ? "no: other filter pattern"
                      : id.colors != 3     ? "no: four colours"
                      : rd.float_image     ? "no: float mosaic"
                      : !rd.raw_image      ? "no: unpacked elsewhere"
                                           : ok ? "yes: decoded on the GPU too" : "yes: unpacked by LibRaw";
    std::fprintf(stderr, "rawcheck-gpu: %s %s\n", path.c_str(), gpu);
    std::fprintf(stderr, "rawcheck: %s %s; ours %.0f ms, LibRaw's %.0f ms (%s, %s %s); levels %s\n", path.c_str(), verdict.c_str(), (t1 - t0) / 1000.0,
                 (t2 - t1) / 1000.0, info.decoder_name ? info.decoder_name : "?", theirs.imgdata.idata.make, theirs.imgdata.idata.model,
                 levels ? "same" : "DIFFERENT");
    if (blocks)
        std::fprintf(stderr, "rawcheck:   blocks: %s, %d bits, %d samples a block, %d of %d rows written, %d wide\n",
                     blocks->format == rawblocks::Format::sony        ? "Sony's"
                     : blocks->format == rawblocks::Format::panasonic ? "Panasonic's"
                     : blocks->format == rawblocks::Format::packed    ? "Panasonic's packed"
                                                                      : "Panasonic's C6",
                     blocks->format == rawblocks::Format::sony ? 11 : blocks->bits, blocks->per, blocks->rows, blocks->readout_h, blocks->readout_w);
    if (!levels)
        std::fprintf(stderr, "rawcheck:   black %u / %u, maximum %u / %u, cblack %u %u %u %u / %u %u %u %u\n", a.black, b.black, a.maximum, b.maximum,
                     a.cblack[0], a.cblack[1], a.cblack[2], a.cblack[3], b.cblack[0], b.cblack[1], b.cblack[2], b.cblack[3]);
}

// IFRIT_GPU_CORRECTIONS_CHECK: a Phase One's active area as LibRaw
// corrects it (RawMosaic::corrected), and how long it took.
std::shared_ptr<const std::vector<uint16_t>> libraw_corrected(const uint8_t* file, size_t size)
{
    auto raw = std::make_unique<RawAccess>();  // on the heap: hundreds of KB
    auto out = std::make_shared<std::vector<uint16_t>>();
    const int64_t t0 = g_get_monotonic_time();
    if (raw->open_buffer(file, size) != LIBRAW_SUCCESS || raw->unpack() != LIBRAW_SUCCESS)
        return nullptr;
    const int64_t t1 = g_get_monotonic_time();
    if (!raw->phase_one_corrected(*out))
        return nullptr;
    std::fprintf(stderr, "raw: LibRaw unpacked it in %.0f ms, corrected it in %.0f ms\n", (t1 - t0) / 1000.0, (g_get_monotonic_time() - t1) / 1000.0);
    return out;
}

bool finish_mosaic(RawAccess& raw, std::shared_ptr<RawMosaic> m, Image& out);

// Three colours a photosite already (Canon's sRAW and mRAW, a linear DNG not
// unpacked here): LibRaw unpacks them, its raw2image takes the active area
// out, and they go up as they are, the develop taking off black and scaling
// (develop.hlsl's LINEAR) with no demosaic.
bool decode_linear(RawAccess& raw, Image& out, std::string& error, bool ycc)
{
    const int64_t t0 = g_get_monotonic_time();
    int r = raw.unpack();
    if (r == LIBRAW_SUCCESS)
        r = raw.raw2image();
    if (r != LIBRAW_SUCCESS)
    {
        error = libraw_strerror(r);
        return false;
    }
    const auto& sz = raw.imgdata.sizes;
    const ushort(*image)[4] = raw.imgdata.image;
    if (!image || sz.iwidth != sz.width || sz.iheight != sz.height)
        return false;
    auto m = std::make_shared<RawMosaic>();
    m->width = sz.width;
    m->height = sz.height;
    m->channels = 3;
    auto data = std::make_shared<std::vector<uint16_t>>(size_t(m->width) * m->height * 3);
    parallel(unsigned(m->height),
             [&](unsigned y)
             {
                 uint16_t* o = data->data() + size_t(y) * m->width * 3;
                 const ushort(*i)[4] = image + size_t(y) * m->width;
                 for (int x = 0; x < m->width; ++x, o += 3)
                     o[0] = i[x][0], o[1] = i[x][1], o[2] = i[x][2];
             });
    if (ycc)
    {
        // Which rows and columns the colour's at (the first block's corner):
        // counted over the first rows, any colour but none (8192) marking one.
        int at[2][2] = {};
        for (int y = 0; y < std::min(4, m->height); ++y)
            for (int x = 0; x < std::min(512, m->width); ++x)
            {
                const uint16_t* p = data->data() + (size_t(y) * m->width + x) * 3;
                if (p[1] != 8192 || p[2] != 8192)
                    ++at[y & 1][x & 1];
            }
        const bool both_rows = at[0][0] + at[0][1] && at[1][0] + at[1][1];
        m->ycc.block_h = both_rows ? 1 : 2;
        m->ycc.y0 = both_rows || at[0][0] + at[0][1] ? 0 : 1;
        m->ycc.x0 = at[m->ycc.y0][0] >= at[m->ycc.y0][1] ? 0 : 1;
        raw.canon_ycc(m->ycc);
    }
    m->data = std::move(data);
    raw.free_image();
    if (std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "raw: %s, unpacked by LibRaw in %.0f ms\n", ycc ? "Canon sRAW as stored" : "linear",
                     (g_get_monotonic_time() - t0) / 1000.0);
    return finish_mosaic(raw, std::move(m), out);
}

// Unpacked only: the sensor's Bayer mosaic and what developing it needs,
// for the GPU to develop (gpu_view's develop pass). The CPU's part of a raw
// is just this, a fraction of a second. False for sensors it doesn't take
// (Foveon, four colours, floats): those develop here, on the CPU.
bool decode_mosaic(RawAccess& raw, const uint8_t* file, size_t size, Image& out, std::string& error)
{
    const auto& id = raw.imgdata.idata;
    // A 2 x 2 Bayer pattern or X-Trans's 6 x 6, already three colours a
    // photosite (a linear DNG, Canon's sRAW), or no filter at all (a
    // monochrome sensor): no Foveon or four-colour filters yet.
    const bool linear = !id.filters && id.colors == 3, grey = !id.filters && id.colors == 1;
    if (!linear && !grey && !raw.developed_pattern())
        return false;
    if (raw.loader() == "canon_sraw_load_raw()")
    {
        // Canon's sRAW as stored (its YCbCr), not LibRaw's RGB made from it:
        // the develop converts it (RawMosaic::Ycc).
        raw.imgdata.rawparams.specials |= LIBRAW_RAWSPECIAL_SRAW_NO_INTERPOLATE;
        return decode_linear(raw, out, error, true);
    }
    // A CR2, a NEF, a PEF, a DNG, Sony's lossless ARW or a raw in blocks on
    // the GPU (a CR3 or a Fuji's compressed RAF put together there), or here
    // on every core (an ORF its codes on one); the rest by LibRaw. A linear
    // DNG's three colours a photosite (Sony's YCbCr's, made RGB).
    const int64_t t0 = g_get_monotonic_time();
    std::vector<uint16_t> readout;
    std::string ours_error;
    const ushort* src = nullptr;
    size_t pitch = 0;
    std::shared_ptr<RawMosaic::Stream> stream;
    uint32_t offset, length;
    unsigned slices[3];
    RawMosaic::Stream layout;
    if (decode_options().gpu_decode && locate_cr2(file, size, offset, length, slices))
    {
        auto prepared = std::make_shared<ljpeg::Prepared>();
        const auto& sz = raw.imgdata.sizes;
        if (ljpeg::prepare(file + offset, length, *prepared, ours_error) && prepared->predictor == 1 &&
            uint64_t(prepared->width) * prepared->components * prepared->height == uint64_t(sz.raw_width) * sz.raw_height &&
            (!slices[0] || size_t(slices[0]) * slices[1] + slices[2] == sz.raw_width))
        {
            stream = raw.readout_stream(true);
            stream->jpeg = std::move(prepared);
            std::copy_n(slices, 3, stream->slices);
        }
    }
    else if (ljpeg::Prepared nef; decode_options().gpu_decode && (raw.nikon(file, size, nef, ours_error) || raw.pentax(file, size, nef, ours_error)))
    {
        stream = raw.readout_stream(false);
        stream->jpeg = std::make_shared<ljpeg::Prepared>(std::move(nef));
    }
    else if (ljpeg::Prepared head, tiled; decode_options().gpu_decode && raw.ljpeg_tiles(file, size, head, layout, ours_error, true) &&
                                           head.predictor == 1 && raw.ljpeg_tiles(file, size, tiled, layout, ours_error))
    {
        // A DNG or Sony's ARW predicted from the left: its tiles on the GPU
        // (predicted from above, unpacked here on every core).
        stream = raw.readout_stream(false);
        stream->jpeg = std::make_shared<ljpeg::Prepared>(std::move(tiled));
        std::copy_n(layout.tile, 3, stream->tile);
        stream->quads = layout.quads;
    }
    else if (auto packed = std::make_shared<rawpacked::Prepared>(); decode_options().gpu_decode && raw.packed(file, size, *packed, ours_error))
    {
        // Up as the file stores it, from the bytes already read: no copy.
        stream = raw.readout_stream(raw.packed_sides(), false);
        stream->packed = std::move(packed);
        stream->file = file_bytes_owner(), stream->file_size = size;
    }
    else if (auto blocks = std::make_shared<rawblocks::Prepared>();
             decode_options().gpu_decode && raw.blocks(file, size, *blocks, ours_error) && rawblocks::fits_gpu(*blocks))
    {
        // Read where they lie in the file: no copy.
        stream = raw.readout_stream(false);
        stream->blocks = std::move(blocks);
        stream->file = file_bytes_owner(), stream->file_size = size;
    }
    else if (crx::Prepared cr3;
             decode_options().gpu_decode && raw.crx(file, size, cr3, ours_error) && uint64_t(cr3.header.width) * cr3.header.height * 2 <= gpu::kMaxBuffer)
    {
        // A CR3 decoded here on every core, a plane each place in the 2 x 2
        // (at most the 128 MB the GPU takes); the GPU puts them together and
        // measures the black.
        auto planes = std::make_shared<std::vector<uint16_t>>();
        if (crx::decode(cr3, *planes, ours_error))
        {
            stream = raw.readout_stream(true);
            stream->decoded = std::move(planes);
            stream->planes = true;
        }
    }
    else if (fuji::Prepared raf; decode_options().gpu_decode && raw.fuji(file, size, raf, ours_error) &&
                                 uint64_t(raf.header.readout_w) * raf.header.readout_h * 2 <= gpu::kMaxBuffer)
    {
        // A Fuji's compressed RAF decoded here on every core, a strip a
        // thread (at most the 128 MB the GPU takes); the GPU takes the
        // active area out and measures the black.
        auto rows = std::make_shared<std::vector<uint16_t>>();
        fuji::decode(raf, *rows);
        stream = raw.readout_stream(false);
        stream->decoded = std::move(rows);
    }
    const unsigned n = raw.samples();
    if (stream)
        ;  // decoded on the GPU
    else if (unpack_here(raw, file, size, readout, ours_error))
    {
        raw.black_from_masked(readout);
        src = readout.data();
        pitch = raw.imgdata.sizes.raw_width * n;
    }
    else
    {
        if (!ours_error.empty() && std::getenv("IFRIT_TRACE"))
            std::fprintf(stderr, "raw: not unpacked here (%s), LibRaw does\n", ours_error.c_str());
        if (linear)
            return decode_linear(raw, out, error, false);
        const int r = raw.unpack();
        if (r != LIBRAW_SUCCESS)
        {
            error = libraw_strerror(r);
            return false;
        }
        src = raw.imgdata.rawdata.raw_image;
        pitch = raw.imgdata.sizes.raw_pitch / 2;
    }
    const int64_t t1 = g_get_monotonic_time();
    const auto& sz = raw.imgdata.sizes;
    if ((!src && !stream) || sz.width == 0 || sz.height == 0)
        return false;
    auto m = std::make_shared<RawMosaic>();
    m->width = sz.width;
    m->height = sz.height;
    m->channels = int(n);
    m->grey = grey;
    if (stream)
        m->stream = std::move(stream);
    else
    {
        auto data = std::make_shared<std::vector<uint16_t>>(size_t(m->width) * m->height * n);
        // The active area, out of the readout with its margins.
        parallel(unsigned(m->height),
                 [&](unsigned y)
                 {
                     std::memcpy(data->data() + size_t(y) * m->width * n, src + size_t(y + sz.top_margin) * pitch + size_t(sz.left_margin) * n,
                                 size_t(m->width) * n * 2);
                 });
        m->data = std::move(data);
    }
    if (std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "raw: %s in %.0f ms, active area copied in %.0f ms\n",
                     m->stream ? "prepared for the GPU" : readout.empty() ? "unpacked by LibRaw" : "unpacked here", (t1 - t0) / 1000.0,
                     (g_get_monotonic_time() - t1) / 1000.0);
    return finish_mosaic(raw, std::move(m), out);
}

// What developing it needs besides the data: levels, the camera's balance and
// colour, its framing and how it was held; and the image as it will be shown.
bool finish_mosaic(RawAccess& raw, std::shared_ptr<RawMosaic> m, Image& out)
{
    const auto& id = raw.imgdata.idata;
    const auto& sz = raw.imgdata.sizes;
    const auto& col = raw.imgdata.color;
    // Each site of the filter pattern: dcraw's FC(row, col), the colour (0
    // R, 1 G, 2 B, 3 the second G), or X-Trans's (LibRaw's xtrans, from the
    // active area's corner). Its black level as LibRaw has it: the common
    // one, its colour's, and its place in a repeating pattern of them (DNG's
    // BlackLevelRepeatDim).
    if (m->channels == 3)
        for (int ch = 0; ch < 3; ++ch)  // linear: per channel R, G, B
            m->black[ch] = float(col.black + col.cblack[ch]);
    else
    {
        m->period = id.filters == 9 ? 6 : 2;
        for (int row = 0; row < m->period; ++row)
            for (int c = 0; c < m->period; ++c)
            {
                const int f = id.filters == 9 ? id.xtrans[row][c] : (id.filters >> ((((row << 1) & 14) | (c & 1)) << 1)) & 3;
                unsigned b = col.black + col.cblack[f];
                if (col.cblack[4] && col.cblack[5] && 6 + col.cblack[4] * col.cblack[5] <= LIBRAW_CBLACK_SIZE)
                    b += col.cblack[6 + (row % col.cblack[4]) * col.cblack[5] + c % col.cblack[5]];
                m->filter[m->site(c, row)] = f;
                m->black[m->site(c, row)] = float(b);
            }
    }
    m->white = float(col.maximum);
    if (std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "raw: black %u + %u %u %u %u, pattern %ux%u: %u %u %u %u, white %u, mul %g %g %g, filters %08x, active %ux%u at %u,%u, COLOR %d%d%d%d\n", col.black, col.cblack[0],
                     col.cblack[1], col.cblack[2], col.cblack[3], col.cblack[4], col.cblack[5], col.cblack[6], col.cblack[7], col.cblack[8],
                     col.cblack[9], col.maximum, col.cam_mul[0], col.cam_mul[1], col.cam_mul[2], id.filters, sz.width, sz.height,
                     sz.left_margin, sz.top_margin, raw.COLOR(0, 0), raw.COLOR(0, 1), raw.COLOR(1, 0), raw.COLOR(1, 1));
    // As shot, scaled so the least is 1: a channel clips at 1 after it, and
    // clipping all at 1 keeps highlights neutral.
    float mul[3] = {col.cam_mul[0], col.cam_mul[1], col.cam_mul[2]};
    if (!(mul[0] > 0 && mul[1] > 0 && mul[2] > 0))
        for (int c = 0; c < 3; ++c)
            mul[c] = col.pre_mul[c] > 0 ? col.pre_mul[c] : 1;
    const float least = std::min({mul[0], mul[1], mul[2]});
    for (int c = 0; c < 3; ++c)
        m->as_shot[c] = m->grey ? 1 : mul[c] / least;  // grey: no colours to balance
    m->balanced = (col.as_shot_wb_applied & LIBRAW_ASWB_APPLIED) != 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            m->rgb_cam[i][j] = m->grey ? float(i == j) : col.rgb_cam[i][j];  // grey stays grey
            m->cam_xyz[i][j] = col.cam_xyz[i][j];
        }
    // A DNG's camera LibRaw has no table for: its own matrix, XYZ to camera
    // (ColorMatrix, by CameraCalibration where there is one), the one for
    // D65 if there are two.
    bool empty = true;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            empty &= col.cam_xyz[i][j] == 0;
    if (empty)
    {
        const auto& dng = col.dng_color;
        const int k = dng[1].illuminant == 21 || (dng[0].illuminant != 21 && dng[1].colormatrix[0][0] != 0) ? 1 : 0;
        const auto& cm = dng[k].colormatrix;
        const auto& cc = dng[k].calibration;
        const bool calibrated = cc[0][0] != 0 && cc[1][1] != 0 && cc[2][2] != 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                float v = 0;
                for (int n = 0; n < 3; ++n)
                    v += (calibrated ? cc[i][n] : float(i == n)) * cm[n][j];
                m->cam_xyz[i][j] = v;
            }
    }
    // The camera's framing, as the CPU develop crops to it.
    m->crop[0] = 0, m->crop[1] = 0, m->crop[2] = m->width, m->crop[3] = m->height;
    if (decode_options().raw_camera_crop)
    {
        const auto& c = sz.raw_inset_crops[0];
        const int left = int(c.cleft) - int(sz.left_margin), top = int(c.ctop) - int(sz.top_margin);
        if (c.cwidth && c.cheight && c.cleft != 0xFFFF && c.ctop != 0xFFFF && left >= 0 && top >= 0 &&
            left + int(c.cwidth) <= m->width && top + int(c.cheight) <= m->height)
        {
            m->crop[0] = left, m->crop[1] = top, m->crop[2] = c.cwidth, m->crop[3] = c.cheight;
            out.add(N_("Raw"), N_("Camera framing"),
                    std::to_string(c.cwidth) + " × " + std::to_string(c.cheight) + " at (" + std::to_string(c.cleft) + ", " +
                        std::to_string(c.ctop) + ")");
        }
    }
    m->orientation = flip_to_orientation(sz.flip);
    const bool turned = m->orientation >= 5;
    // What it will be, as shown: no pixels until something asks for them.
    out.width = turned ? m->crop[3] : m->crop[2];
    out.height = turned ? m->crop[2] : m->crop[3];
    out.stride = out.width * 4;
    out.pixels_stale = true;
    out.mosaic = std::move(m);
    return true;
}

// The embedded JPEG, used as the on-screen stand-in while the real decode
// runs; not offered when the configured mode is the preview anyway.
void on_exif_tag(void* context, int tag, int type, int len, unsigned int order, void* ifp, INT64);

bool preview(const std::string& path, Image& out)
{
    if (decode_options().raw_mode == DecodeOptions::RawMode::Preview)
        return false;
    auto raw_ptr = std::make_unique<LibRaw>();  // on the heap: hundreds of KB
    LibRaw& raw = *raw_ptr;
    double bias = 0;
    raw.set_exifparser_handler(on_exif_tag, &bias);  // the slider rests at the camera's compensation even on the preview
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    const int64_t t0 = g_get_monotonic_time();
    if (!file_bytes(path, bytes, size) || raw.open_buffer(bytes, size) != LIBRAW_SUCCESS)
        return false;
    const int64_t t1 = g_get_monotonic_time();
    std::string error;
    int fit_w = 0, fit_h = 0;
    preview_fit(fit_w, fit_h);
    if (!decode_preview(raw, out, error, fit_w, fit_h))
        return false;
    platform::trace("raw: preview %dx%d, opened in %.1f ms, decoded in %.1f ms", out.width, out.height, (t1 - t0) / 1000.0,
                    (g_get_monotonic_time() - t1) / 1000.0);
    out.format = "RAW preview";
    out.is_raw = true;
    out.info = camera_name(raw);
    out.exposure_bias = bias;
    // What the develop will produce, so the window can be sized for it now.
    raw.imgdata.params.half_size = decode_options().raw_mode == DecodeOptions::RawMode::Half ? 1 : 0;
    apply_camera_crop(raw);
    if (raw.adjust_sizes_info_only() == LIBRAW_SUCCESS)
    {
        out.final_width = raw.imgdata.sizes.iwidth;
        out.final_height = raw.imgdata.sizes.iheight;
    }
    return true;
}

// LibRaw keeps no field for ExposureBiasValue (EXIF 0x9204); catch it as
// the EXIF block is parsed.
void on_exif_tag(void* context, int tag, int type, int len, unsigned int order, void* ifp, INT64)
{
    if (tag != 0x9204 || type != 10 || len < 1)
        return;  // SRATIONAL
    auto* stream = static_cast<LibRaw_abstract_datastream*>(ifp);
    uint8_t buf[8];
    if (stream->read(buf, 8, 1) != 1)
        return;
    auto get32 = [&](const uint8_t* p) -> int32_t
    {
        return order == 0x4949 ? int32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24))
                               : int32_t((uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
    };
    const int32_t num = get32(buf), den = get32(buf + 4);
    if (den != 0)
        *static_cast<double*>(context) = double(num) / den;
}

bool decode_raw(const std::string& path, Image& out, std::string& error)
{
    const DecodeOptions options = decode_options();
    auto raw_ptr = std::make_unique<RawAccess>();  // on the heap: hundreds of KB, and a Windows thread's stack is 1 MB
    RawAccess& raw = *raw_ptr;
    double bias = 0;
    raw.set_exifparser_handler(on_exif_tag, &bias);
    raw.imgdata.params.use_camera_wb = 1;
    raw.imgdata.params.output_color = 1;  // sRGB
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    const int64_t t_read = g_get_monotonic_time();
    if (!file_bytes(path, bytes, size))
    {
        error = "cannot read file";
        return false;
    }
    if (const char* check = std::getenv("IFRIT_RAW_CHECK"))
    {
        check_unpack(bytes, size, path);
        if (std::strcmp(check, "only") == 0)
        {
            error = "checked only (IFRIT_RAW_CHECK=only)";
            return false;
        }
    }
    const int64_t t_open = g_get_monotonic_time();
    int r = raw.open_buffer(bytes, size);
    if (std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "raw: file read in %.0f ms, opened in %.0f ms\n", (t_open - t_read) / 1000.0, (g_get_monotonic_time() - t_open) / 1000.0);
    if (r != LIBRAW_SUCCESS)
    {
        error = libraw_strerror(r);
        return false;
    }
    raw.fix_known_bad_matrix();

    bool ok = false;
    std::string mode;
    using RawMode = DecodeOptions::RawMode;
    if (options.raw_mode == RawMode::Preview)
    {
        ok = decode_preview(raw, out, error);
        mode = "embedded preview";
    }
    // Developed on the GPU where there is one: only unpacked here.
    if (!ok && options.raw_mode == RawMode::Full && options.gpu_develop)
    {
        ok = decode_mosaic(raw, bytes, size, out, error);
        mode = "full size, GPU";
    }
    if (!ok)
    {
        const bool half = options.raw_mode != RawMode::Full;
        ok = decode_full(raw, half, out, error);
        mode = half ? "half size" : "full size";
    }
    if (!ok)
        return false;
    // A raw's own corrections, made as it's developed: a DNG's lens shading
    // and vignetting; a Phase One's black and its sensor's corrections.
    static const bool corrections = !std::getenv("IFRIT_NO_DNG_CORRECTIONS");
    const bool phase_one = raw.is_phaseone_compressed();
    if (out.mosaic && out.mosaic->channels == 1 && !out.mosaic->ycc.block_h && (phase_one || (corrections && raw.imgdata.idata.dng_version)))
    {
        const int64_t t0 = g_get_monotonic_time();
        dng::Corrections c = phase_one ? raw.phase_one_corrections(bytes, size)
                                       : dng::corrections(bytes, size, raw.imgdata.sizes.raw_width, raw.imgdata.sizes.raw_height,
                                                          raw.imgdata.sizes.width, raw.imgdata.sizes.height);
        if (!c.summary.empty())
        {
            out.add(N_("Raw"), phase_one ? N_("Phase One corrections") : N_("DNG corrections"), c.summary);
            platform::trace("raw: %s corrections %s, read in %.1f ms", phase_one ? "Phase One" : "DNG", c.summary.c_str(),
                            (g_get_monotonic_time() - t0) / 1000.0);
        }
        if (!c.empty())
        {
            auto m = std::make_shared<RawMosaic>(*out.mosaic);
            m->corrections = std::make_shared<const std::vector<uint32_t>>(std::move(c.words));
            if (phase_one && std::getenv("IFRIT_GPU_CORRECTIONS_CHECK"))
                m->corrected = libraw_corrected(bytes, size);
            out.mosaic = std::move(m);
        }
    }
    // Without any of the lens (a Phase One's are its sensor's), how it was
    // taken: the app finds the lens in lensfun's database.
    if (out.mosaic && (!out.mosaic->corrections || phase_one))
    {
        auto shot = std::make_shared<lenses::Shot>(shot_of(raw));
        std::copy(out.mosaic->crop, out.mosaic->crop + 4, shot->frame);
        auto m = std::make_shared<RawMosaic>(*out.mosaic);
        m->shot = std::move(shot);
        out.mosaic = std::move(m);
    }

    out.bits_per_sample = raw.imgdata.color.raw_bps ? raw.imgdata.color.raw_bps : 14;
    if (out.linear)
        out.color_space = "sRGB (developed)";  // the sensor has no colour space; this is what we rendered to
    else if (out.mosaic)
        out.color_space = "camera (developed on the GPU)";
    std::string make = raw.imgdata.idata.normalized_make[0] ? raw.imgdata.idata.normalized_make : raw.imgdata.idata.make;
    out.format = make.empty() ? "RAW" : make + " RAW";
    out.format += ", " + mode;
    out.info = camera_name(raw);
    out.is_raw = true;

    const auto& id = raw.imgdata.idata;
    const auto& other = raw.imgdata.other;
    const auto& lens = raw.imgdata.lens;
    auto fmt = [](double v, int decimals)
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
        return std::string(buf);
    };
    out.add(N_("Camera"), N_("Make"), id.make);
    out.add(N_("Camera"), N_("Model"), id.model);
    if (id.software[0])
        out.add(N_("Camera"), N_("Firmware"), id.software);
    if (lens.Lens[0])
        out.add(N_("Camera"), N_("Lens"), lens.Lens);
    else if (lens.LensMake[0])
        out.add(N_("Camera"), N_("Lens make"), lens.LensMake);
    if (other.iso_speed > 0)
        out.add(N_("Camera"), N_("ISO"), std::to_string(int(other.iso_speed)));
    if (other.shutter > 0)
        out.add(N_("Camera"), N_("Shutter"), other.shutter >= 1 ? fmt(other.shutter, 1) + " s" : "1/" + std::to_string(int(1 / other.shutter + 0.5)) + " s");
    if (other.aperture > 0)
        out.add(N_("Camera"), N_("Aperture"), "f/" + fmt(other.aperture, 1));
    if (other.focal_len > 0)
        out.add(N_("Camera"), N_("Focal length"), fmt(other.focal_len, 0) + " mm");
    out.exposure_bias = bias;
    if (bias != 0)
        out.add(N_("Camera"), N_("Exposure compensation"), (bias > 0 ? "+" : "") + fmt(bias, 2) + " EV");
    if (other.timestamp)
    {
        char buf[64];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&other.timestamp));
        out.add(N_("Camera"), N_("Taken"), buf);
    }
    if (other.shot_order)
        out.add(N_("Camera"), N_("Shot number"), std::to_string(other.shot_order));
    if (other.artist[0])
        out.add(N_("Camera"), N_("Artist"), other.artist);
    if (other.desc[0])
        out.add(N_("Camera"), N_("Description"), other.desc);
    out.add(N_("Raw"), N_("Sensor size"), std::to_string(raw.imgdata.sizes.raw_width) + " × " + std::to_string(raw.imgdata.sizes.raw_height));
    out.add(N_("Raw"), N_("Output size"), std::to_string(out.width) + " × " + std::to_string(out.height) + " (" + mode + ")");
    if (raw.imgdata.color.raw_bps)
        out.add(N_("Raw"), N_("Bits per sample"), std::to_string(raw.imgdata.color.raw_bps));
    out.add(N_("Raw"), N_("Colour channels"), std::to_string(id.colors) + " (" + id.cdesc + ")");
    if (id.dng_version)
        out.add(N_("Raw"), N_("DNG version"), std::to_string(id.dng_version >> 24) + "." + std::to_string((id.dng_version >> 16) & 0xFF));
    {
        const int bits = raw.imgdata.color.raw_bps ? raw.imgdata.color.raw_bps : 14;
        if (id.filters == 9)
            out.pixel_format = PixelFormat::mosaic_("X-Trans", bits);
        else if (id.filters)
        {
            std::string pat;
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 2; ++col)
                {
                    const int shift = (((row << 1) & 14) + (col & 1)) << 1;  // dcraw's FC(row, col)
                    pat += id.cdesc[(id.filters >> shift) & 3];
                }
            out.pixel_format = PixelFormat::mosaic_("Bayer " + pat, bits);
        }
        else
        {
            out.pixel_format = id.colors == 1 ? PixelFormat::packed(PixelFormat::Model::L, "L", bits)
                                              : PixelFormat::packed(PixelFormat::Model::RGB, "RGB", bits);
            out.pixel_format.notes.push_back("linear");
        }
    }
    if (id.filters == 9)
        out.add(N_("Raw"), N_("Mosaic"), "X-Trans");
    else if (id.filters)
        out.add(N_("Raw"), N_("Mosaic"), "Bayer");
    else
        out.add(N_("Raw"), N_("Mosaic"), id.colors == 1 ? "none (monochrome)" : "none (linear)");
    const float* wb = raw.imgdata.color.cam_mul;
    if (wb[0] > 0 && wb[1] > 0)
        out.add(N_("Raw"), N_("Camera white balance"), fmt(wb[0] / wb[1], 3) + " / 1 / " + fmt(wb[2] / wb[1], 3));
    if (out.linear)
        out.add(N_("Raw"), N_("Auto exposure gain"), fmt(std::log2(out.linear->auto_gain), 2) + " EV");
    return true;
}

}  // namespace

// A raw whose readout the GPU decodes (a CR2, a NEF, a PEF, a DNG or Sony's
// lossless ARW predicted from the left, one uncompressed, Sony's and
// Panasonic's blocks): ready as soon as its preview would be.
bool quick(const std::string& path)
{
    if (!decode_options().gpu_decode || decode_options().raw_mode != DecodeOptions::RawMode::Full)
        return false;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    uint32_t offset, length;
    unsigned slices[3];
    RawMosaic::Stream layout;
    if (!file_bytes(path, bytes, size))
        return false;
    int predictor = 0;
    if (locate_cr2(bytes, size, offset, length, slices))
        return ljpeg::takes(bytes + offset, length, &predictor) && predictor == 1;
    // A NEF, PEF, DNG or Sony's ARW of the kind decoded on the GPU, or a
    // Bayer readout stored uncompressed (opening it reads only its headers).
    // On the heap: a LibRaw is hundreds of KB.
    auto raw = std::make_unique<RawAccess>();
    ljpeg::Prepared unused;  // a NEF's or DNG's header
    rawpacked::Prepared packed;
    rawblocks::Prepared blocks;
    std::string error;
    if (raw->open_buffer(bytes, size) != LIBRAW_SUCCESS)
        return false;
    const auto& id = raw->imgdata.idata;
    return raw->nikon(bytes, size, unused, error, true) || raw->pentax(bytes, size, unused, error, true) ||
           (raw->ljpeg_tiles(bytes, size, unused, layout, error, true) && unused.predictor == 1) ||
           ((raw->developed_pattern() || (!id.filters && id.colors == 1)) && raw->packed(bytes, size, packed, error)) ||
           (raw->blocks(bytes, size, blocks, error) && rawblocks::fits_gpu(blocks));
}

bool raw_shot(const std::string& path, lenses::Shot& out)
{
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    auto raw = std::make_unique<LibRaw>();  // on the heap: hundreds of KB
    if (!file_bytes(path, bytes, size) || raw->open_buffer(bytes, size) != LIBRAW_SUCCESS)
        return false;
    out = shot_of(*raw);
    out.frame[2] = raw->imgdata.sizes.width, out.frame[3] = raw->imgdata.sizes.height;
    return true;
}

const Decoder& raw_decoder()
{
    static const Decoder d{"RAW",
                           {"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "pef", "dng",
                            "raw", "rwl", "3fr", "erf", "kdc", "mef", "mos", "mrw", "x3f", "iiq", "srw", "fff", "dcr"},
                           sniff,
                           decode_raw,
                           preview,
                           false,
                           nullptr,
                           quick};
    return d;
}
