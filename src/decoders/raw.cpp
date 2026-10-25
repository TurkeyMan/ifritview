#include "decoder.h"
#include "dng_opcodes.h"
#include "ljpeg.h"
#include "../i18n.h"
#include "../platform.h"

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
    const auto& sz = raw.imgdata.sizes;
    const int w = sz.iwidth, h = sz.iheight;
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

// Rows [0, n) split among the cores: copies of tens of megabytes, mostly
// the first touch of new memory, go as fast as the cores fault them in.
template <typename F>
void rows_in_parallel(unsigned n, F f)
{
    const unsigned t = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> pool;
    for (unsigned k = 0; k < t; ++k)
        pool.emplace_back([&, k] { for (unsigned r = n * k / t; r < n * (k + 1) / t; ++r) f(r); });
    for (std::thread& th : pool)
        th.join();
}

// LibRaw with the step of its unpack that reads the black level from the
// sensor's masked border, for a readout unpacked here instead.
class RawAccess : public LibRaw
{
public:
    // A compressed Phase One's black (its level, less each row's and
    // column's offset) and its flat-field corrections, which LibRaw makes
    // in raw2image, not as it unpacks: into a buffer of their own that
    // raw_image then points at, until phase_one_done(). False when it isn't one.
    bool phase_one_prepare()
    {
        if (!is_phaseone_compressed() || !imgdata.rawdata.raw_alloc)
            return false;
        phase_one_allocate_tempbuffer();
        int rc = phase_one_subtract_black(static_cast<ushort*>(imgdata.rawdata.raw_alloc), imgdata.rawdata.raw_image);
        if (rc == 0 && imgdata.params.use_p1_correction)
            rc = phase_one_correct();
        if (rc != 0)
        {
            phase_one_free_tempbuffer();
            return false;
        }
        return true;
    }
    void phase_one_done() { phase_one_free_tempbuffer(); }
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
        libraw_decoder_info_t info{};
        if (const_cast<RawAccess*>(this)->get_decoder_info(&info) != LIBRAW_SUCCESS || !info.decoder_name ||
            std::strcmp(info.decoder_name, "nikon_load_raw()") != 0)
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
    void masked_areas(int m[8][4], bool sides) const
    {
        const auto& sz = imgdata.sizes;
        std::memcpy(m, sz.mask, sizeof sz.mask);
        if (m[0][3] > 0 || !sides)  // the sides measured for Canon's lossless JPEG readouts, not Nikon's
            return;
        m[0][1] = m[1][1] += 2;
        m[0][3] -= 2;
        m[0][0] = m[1][0] = sz.top_margin;
        m[0][2] = m[1][2] = sz.top_margin + sz.height;
        m[0][3] += sz.left_margin;
        m[1][1] += sz.left_margin + sz.width;
        m[1][3] += sz.raw_width;
    }
    // As LibRaw's unpack leaves them (0.22's): measured where the sensor is
    // masked in `readout` (raw_width x raw_height), and the least of the
    // four channels' levels then moved into the common one.
    void black_from_masked(std::vector<uint16_t>& readout)
    {
        auto& sz = imgdata.sizes;
        auto& col = imgdata.color;
        sz.raw_pitch = sz.raw_width * 2;
        imgdata.rawdata.raw_image = readout.data();
        crop_masked_pixels();
        imgdata.rawdata.raw_image = nullptr;  // ours, not LibRaw's to free
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

bool unpack_cr2(LibRaw& raw, const uint8_t* file, size_t size, std::vector<uint16_t>& out, std::string& error)
{
    uint32_t offset, length;
    unsigned slices[3];
    // A NEF: its rows as they come (raw_width wide, `height` of them), the rest of the readout none.
    if (ljpeg::Prepared nef; static_cast<const RawAccess&>(raw).nikon(file, size, nef, error))
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
    rows_in_parallel(rh,
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
                             std::memcpy(out.data() + size_t(row) * rw + size_t(i) * slices[1], from + size_t(i) * slices[1] * rh + size_t(row) * w,
                                         size_t(w) * 2);
                         }
                     });
    return true;
}

// IFRIT_RAW_CHECK: a CR2 unpacked both ways, ours and LibRaw's, and every
// sample compared (and what LibRaw knows only after unpacking).
void check_cr2(const uint8_t* file, size_t size, const std::string& path)
{
    // On the heap: a LibRaw is hundreds of KB, and a Windows thread's stack 1 MB.
    auto mine_ptr = std::make_unique<RawAccess>(), theirs_ptr = std::make_unique<RawAccess>();
    RawAccess &mine = *mine_ptr, &theirs = *theirs_ptr;
    if (mine.open_buffer(file, size) != LIBRAW_SUCCESS || theirs.open_buffer(file, size) != LIBRAW_SUCCESS)
        return;
    std::vector<uint16_t> ours;
    std::string error;
    const gint64 t0 = g_get_monotonic_time();
    const bool ok = unpack_cr2(mine, file, size, ours, error);
    if (ok)
        mine.black_from_masked(ours);
    const gint64 t1 = g_get_monotonic_time();
    if (theirs.unpack() != LIBRAW_SUCCESS)
        return;
    const gint64 t2 = g_get_monotonic_time();
    const auto& sz = theirs.imgdata.sizes;
    const ushort* raw = theirs.imgdata.rawdata.raw_image;
    std::string verdict = ok ? "same" : "not ours: " + error;
    if (ok && raw)
        for (unsigned y = 0; y < sz.raw_height && verdict == "same"; ++y)
            for (unsigned x = 0; x < sz.raw_width; ++x)
                if (ours[size_t(y) * sz.raw_width + x] != raw[size_t(y) * sz.raw_pitch / 2 + x])
                {
                    verdict = "DIFFERENT at " + std::to_string(x) + "," + std::to_string(y) + ": ours " +
                              std::to_string(ours[size_t(y) * sz.raw_width + x]) + ", LibRaw's " + std::to_string(raw[size_t(y) * sz.raw_pitch / 2 + x]);
                    break;
                }
    const auto& a = mine.imgdata.color;
    const auto& b = theirs.imgdata.color;
    const bool levels = a.black == b.black && a.maximum == b.maximum && std::memcmp(a.cblack, b.cblack, 4 * sizeof a.cblack[0]) == 0;
    libraw_decoder_info_t info{};
    theirs.get_decoder_info(&info);
    // Whether the GPU's develop would take it (decode_mosaic's test), and if not why.
    const auto& id = theirs.imgdata.idata;
    const auto& rd = theirs.imgdata.rawdata;
    const char* gpu = !id.filters ? (rd.float3_image || rd.float4_image ? "no: float, demosaiced" : rd.float_image ? "no: float mosaic"
                                     : (rd.color3_image || rd.color4_image) && id.colors == 3     ? "yes: linear or sRAW"
                                     : rd.color3_image || rd.color4_image                         ? "no: demosaiced, not three colours"
                                                                                                    : "no: no colour filter")
                      : id.filters == 9    ? "no: X-Trans"
                      : id.filters < 1000  ? "no: other filter pattern"
                      : id.colors != 3     ? "no: four colours"
                      : rd.float_image     ? "no: float mosaic"
                      : !rd.raw_image      ? "no: unpacked elsewhere"
                                           : ok ? "yes: decoded on the GPU too" : "yes: unpacked by LibRaw";
    std::fprintf(stderr, "rawcheck-gpu: %s %s\n", path.c_str(), gpu);
    std::fprintf(stderr, "rawcheck: %s %s; ours %.0f ms, LibRaw's %.0f ms (%s, %s %s); levels %s\n", path.c_str(), verdict.c_str(), (t1 - t0) / 1000.0,
                 (t2 - t1) / 1000.0, info.decoder_name ? info.decoder_name : "?", theirs.imgdata.idata.make, theirs.imgdata.idata.model,
                 levels ? "same" : "DIFFERENT");
    if (!levels)
        std::fprintf(stderr, "rawcheck:   black %u / %u, maximum %u / %u, cblack %u %u %u %u / %u %u %u %u\n", a.black, b.black, a.maximum, b.maximum,
                     a.cblack[0], a.cblack[1], a.cblack[2], a.cblack[3], b.cblack[0], b.cblack[1], b.cblack[2], b.cblack[3]);
}

bool finish_mosaic(RawAccess& raw, std::shared_ptr<RawMosaic> m, Image& out);

// Three colours a photosite already (a linear DNG, Canon's sRAW and mRAW):
// LibRaw unpacks them, its raw2image takes the active area out, and they go
// up as they are, the develop taking off black and scaling (develop.hlsl's
// LINEAR) with no demosaic.
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
    rows_in_parallel(unsigned(m->height),
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
// (X-Trans, Foveon, linear DNGs): those develop here, on the CPU.
bool decode_mosaic(RawAccess& raw, const uint8_t* file, size_t size, Image& out, std::string& error)
{
    const auto& id = raw.imgdata.idata;
    // A 2 x 2 Bayer pattern, or already three colours a photosite (a linear
    // DNG, Canon's sRAW): no X-Trans, Foveon or four-colour filters yet.
    const bool linear = !id.filters && id.colors == 3;
    if (!linear && (id.filters == 9 || id.filters < 1000 || id.colors != 3))
        return false;
    if (linear)
    {
        // Canon's sRAW as stored (its YCbCr), not LibRaw's RGB made from it:
        // the develop converts it (RawMosaic::Ycc).
        libraw_decoder_info_t info{};
        raw.get_decoder_info(&info);
        const bool ycc = info.decoder_name && std::strcmp(info.decoder_name, "canon_sraw_load_raw()") == 0;
        if (ycc)
            raw.imgdata.rawparams.specials |= LIBRAW_RAWSPECIAL_SRAW_NO_INTERPOLATE;
        return decode_linear(raw, out, error, ycc);
    }
    // A CR2 here, on every core; the rest by LibRaw.
    const int64_t t0 = g_get_monotonic_time();
    std::vector<uint16_t> readout;
    std::string ours_error;
    const ushort* src = nullptr;
    size_t pitch = 0;
    bool phase_one = false;  // its black taken off first (RawAccess::phase_one_prepare)
    std::shared_ptr<RawMosaic::Stream> stream;
    uint32_t offset, length;
    unsigned slices[3];
    if (decode_options().gpu_decode && locate_cr2(file, size, offset, length, slices))
    {
        auto prepared = std::make_shared<ljpeg::Prepared>();
        const auto& sz = raw.imgdata.sizes;
        if (ljpeg::prepare(file + offset, length, *prepared, ours_error) &&
            uint64_t(prepared->width) * prepared->components * prepared->height == uint64_t(sz.raw_width) * sz.raw_height &&
            (!slices[0] || size_t(slices[0]) * slices[1] + slices[2] == sz.raw_width))
        {
            stream = std::make_shared<RawMosaic::Stream>();
            stream->jpeg = std::move(prepared);
            stream->readout_w = sz.raw_width, stream->readout_h = sz.raw_height;
            std::copy_n(slices, 3, stream->slices);
            stream->left = sz.left_margin, stream->top = sz.top_margin;
            stream->filters = raw.imgdata.idata.filters;
            raw.masked_areas(stream->mask, true);
        }
    }
    else if (ljpeg::Prepared nef; decode_options().gpu_decode && raw.nikon(file, size, nef, ours_error))
    {
        const auto& sz = raw.imgdata.sizes;
        stream = std::make_shared<RawMosaic::Stream>();
        stream->jpeg = std::make_shared<ljpeg::Prepared>(std::move(nef));
        stream->readout_w = sz.raw_width, stream->readout_h = sz.raw_height;
        stream->left = sz.left_margin, stream->top = sz.top_margin;
        stream->filters = raw.imgdata.idata.filters;
        raw.masked_areas(stream->mask, false);
    }
    if (stream)
        ;  // decoded on the GPU
    else if (unpack_cr2(raw, file, size, readout, ours_error))
    {
        raw.black_from_masked(readout);
        src = readout.data();
        pitch = raw.imgdata.sizes.raw_width;
    }
    else
    {
        if (!ours_error.empty() && std::getenv("IFRIT_TRACE"))
            std::fprintf(stderr, "raw: not unpacked here (%s), LibRaw does\n", ours_error.c_str());
        const int r = raw.unpack();
        if (r != LIBRAW_SUCCESS)
        {
            error = libraw_strerror(r);
            return false;
        }
        phase_one = raw.phase_one_prepare();
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
    if (stream)
        m->stream = std::move(stream);
    else
    {
        auto data = std::make_shared<std::vector<uint16_t>>(size_t(m->width) * m->height);
        // The active area, out of the readout with its margins.
        rows_in_parallel(unsigned(m->height),
                         [&](unsigned y)
                         {
                             std::memcpy(data->data() + size_t(y) * m->width, src + size_t(y + sz.top_margin) * pitch + sz.left_margin,
                                         size_t(m->width) * 2);
                         });
        m->data = std::move(data);
    }
    if (phase_one)
        raw.phase_one_done();
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
    // dcraw's FC(row, col): the filter colour, 0 R, 1 G, 2 B, 3 the second G.
    // Its black level as LibRaw has it: the common one, its colour's, and its
    // place in a repeating pattern of them (DNG's BlackLevelRepeatDim).
    for (int row = 0; row < 2; ++row)
        for (int c = 0; c < 2; ++c)
        {
            if (m->channels == 3)
            {
                // Linear: per channel R, G, B.
                const int ch = row * 2 + c;
                m->black[ch] = float(col.black + (ch < 3 ? col.cblack[ch] : 0));
                continue;
            }
            const int f = (id.filters >> ((((row << 1) & 14) | (c & 1)) << 1)) & 3;
            m->colour[row * 2 + c] = f == 3 ? 1 : f;
            m->filter[row * 2 + c] = f;
            unsigned b = col.black + col.cblack[f];
            if (col.cblack[4] && col.cblack[5] && 6 + col.cblack[4] * col.cblack[5] <= LIBRAW_CBLACK_SIZE)
                b += col.cblack[6 + (row % col.cblack[4]) * col.cblack[5] + c % col.cblack[5]];
            m->black[row * 2 + c] = float(b);
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
        m->as_shot[c] = mul[c] / least;
    m->balanced = (col.as_shot_wb_applied & LIBRAW_ASWB_APPLIED) != 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            m->rgb_cam[i][j] = col.rgb_cam[i][j];
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
        check_cr2(bytes, size, path);
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
    // A DNG's own lens shading and vignetting, applied as it's developed.
    static const bool corrections = !std::getenv("IFRIT_NO_DNG_CORRECTIONS");
    if (corrections && out.mosaic && out.mosaic->channels == 1 && !out.mosaic->ycc.block_h && raw.imgdata.idata.dng_version)
    {
        dng::Corrections c = dng::corrections(bytes, size, raw.imgdata.sizes.raw_width, raw.imgdata.sizes.raw_height,
                                                       raw.imgdata.sizes.width, raw.imgdata.sizes.height);
        if (!c.summary.empty())
        {
            out.add(N_("Raw"), N_("DNG corrections"), c.summary);
            platform::trace("raw: DNG corrections %s", c.summary.c_str());
        }
        if (!c.empty())
        {
            auto m = std::make_shared<RawMosaic>(*out.mosaic);
            m->corrections = std::make_shared<const std::vector<uint32_t>>(std::move(c.words));
            out.mosaic = std::move(m);
        }
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
            out.pixel_format = PixelFormat::packed(PixelFormat::Model::RGB, "RGB", bits);
            out.pixel_format.notes.push_back("linear");
        }
    }
    if (id.filters == 9)
        out.add(N_("Raw"), N_("Mosaic"), "X-Trans");
    else if (id.filters)
        out.add(N_("Raw"), N_("Mosaic"), "Bayer");
    else
        out.add(N_("Raw"), N_("Mosaic"), "none (linear)");
    const float* wb = raw.imgdata.color.cam_mul;
    if (wb[0] > 0 && wb[1] > 0)
        out.add(N_("Raw"), N_("Camera white balance"), fmt(wb[0] / wb[1], 3) + " / 1 / " + fmt(wb[2] / wb[1], 3));
    if (out.linear)
        out.add(N_("Raw"), N_("Auto exposure gain"), fmt(std::log2(out.linear->auto_gain), 2) + " EV");
    return true;
}

}  // namespace

// A raw whose readout the GPU decodes (a CR2, a NEF): ready as soon as its preview would be.
bool quick(const std::string& path)
{
    if (!decode_options().gpu_decode || decode_options().raw_mode != DecodeOptions::RawMode::Full)
        return false;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    uint32_t offset, length;
    unsigned slices[3];
    if (!file_bytes(path, bytes, size))
        return false;
    if (locate_cr2(bytes, size, offset, length, slices))
        return ljpeg::takes(bytes + offset, length);
    // A NEF of the kind decoded on the GPU (opening it reads only its
    // headers). On the heap: a LibRaw is hundreds of KB.
    auto raw = std::make_unique<RawAccess>();
    ljpeg::Prepared unused;
    std::string error;
    return raw->open_buffer(bytes, size) == LIBRAW_SUCCESS && raw->nikon(bytes, size, unused, error, true);
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
