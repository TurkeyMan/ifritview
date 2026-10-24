#include "jpeg_lossless.h"

#include "platform.h"

#include <type_traits>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>

#include <mutex>
#endif

namespace
{

#ifdef HAVE_TURBOJPEG
// TurboJPEG's functions. On Windows it's linked in; on Linux it's loaded
// when first wanted, so the library is a recommended package, not a
// required one: without it, lossless saving says it needs it.
struct TurboJpeg
{
    decltype(&tj3Init) init = nullptr;
    decltype(&tj3Destroy) destroy = nullptr;
    decltype(&tj3GetErrorStr) error = nullptr;
    decltype(&tj3DecompressHeader) header = nullptr;
    decltype(&tj3Get) get = nullptr;
    decltype(&tj3Transform) transform = nullptr;
    decltype(&tj3Free) free = nullptr;
};

const TurboJpeg* turbojpeg()
{
    static TurboJpeg tj;
    static bool ok = false;
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
#ifdef IFRIT_TURBOJPEG_DLOPEN
                       void* lib = platform::open_library("libturbojpeg.so.0");
                       if (!lib)
                           return;
                       const auto sym = [lib](auto& f, const char* name) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(platform::library_symbol(lib, name)); };
                       sym(tj.init, "tj3Init");
                       sym(tj.destroy, "tj3Destroy");
                       sym(tj.error, "tj3GetErrorStr");
                       sym(tj.header, "tj3DecompressHeader");
                       sym(tj.get, "tj3Get");
                       sym(tj.transform, "tj3Transform");
                       sym(tj.free, "tj3Free");
#else
                       tj = {tj3Init, tj3Destroy, tj3GetErrorStr, tj3DecompressHeader, tj3Get, tj3Transform, tj3Free};
#endif
                       ok = tj.init && tj.destroy && tj.error && tj.header && tj.get && tj.transform && tj.free;
                   });
    return ok ? &tj : nullptr;
}
#endif

bool read_all(const std::string& path, std::vector<uint8_t>& out)
{
    const int64_t n = platform::file_size(path);
    FILE* f = n > 0 ? platform::fopen(path, "rb") : nullptr;
    if (!f)
        return false;
    out.resize(size_t(n));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// "4 px more on the left and 2 px fewer at the bottom".
std::string describe(int left, int top, int right, int bottom)
{
    std::vector<std::string> parts;
    const auto part = [&](int px, const char* side)
    {
        if (px)
            parts.push_back(std::to_string(std::abs(px)) + " px " + (px > 0 ? "more " : "fewer ") + side);
    };
    part(left, "on the left");
    part(top, "at the top");
    part(right, "on the right");
    part(bottom, "at the bottom");
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i)
        s += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
    return s;
}

// A marker segment: FF, the marker, the length (counting itself), the bytes.
void segment(std::vector<uint8_t>& out, uint8_t marker, const char* header, size_t header_len, const uint8_t* body, size_t n)
{
    const size_t len = 2 + header_len + n;
    out.insert(out.end(), {0xFF, marker, uint8_t(len >> 8), uint8_t(len)});
    out.insert(out.end(), header, header + header_len);
    out.insert(out.end(), body, body + n);
}

// EXIF and XMP in APP1, the ICC profile in APP2 chunks, IPTC in APP13: as
// the JPEG encoder writes them (what won't fit a segment is left out).
std::vector<uint8_t> metadata_segments(const FileMetadata& m)
{
    std::vector<uint8_t> out;
    constexpr size_t kMax = 65533;
    if (!m.exif.empty() && 6 + m.exif.size() <= kMax)
        segment(out, 0xE1, "Exif\0\0", 6, m.exif.data(), m.exif.size());
    if (!m.xmp.empty() && 29 + m.xmp.size() <= kMax)
        segment(out, 0xE1, "http://ns.adobe.com/xap/1.0/\0", 29, m.xmp.data(), m.xmp.size());
    if (!m.icc.empty())
    {
        constexpr size_t kChunk = kMax - 14;
        const size_t count = (m.icc.size() + kChunk - 1) / kChunk;
        if (count < 256)
            for (size_t i = 0; i < count; ++i)
            {
                char header[14] = "ICC_PROFILE";
                header[12] = char(i + 1);
                header[13] = char(count);
                const size_t at = i * kChunk;
                segment(out, 0xE2, header, 14, m.icc.data() + at, std::min(kChunk, m.icc.size() - at));
            }
    }
    if (!m.iptc.empty())
    {
        std::vector<uint8_t> irb = {'8', 'B', 'I', 'M', 0x04, 0x04, 0, 0};
        const uint32_t n = uint32_t(m.iptc.size());
        for (int shift : {24, 16, 8, 0})
            irb.push_back(uint8_t(n >> shift));
        irb.insert(irb.end(), m.iptc.begin(), m.iptc.end());
        if (irb.size() & 1)
            irb.push_back(0);
        if (14 + irb.size() <= kMax)
            segment(out, 0xED, "Photoshop 3.0\0", 14, irb.data(), irb.size());
    }
    return out;
}

}  // namespace

LosslessJpeg plan_lossless_jpeg(const Image& image, const std::string& path)
{
    LosslessJpeg plan;
#ifndef HAVE_TURBOJPEG
    (void)image;
    (void)path;
    plan.why_not = "this build has no lossless JPEG transform";
    return plan;
#else
    const Image::Transform& t = image.transform;
    if (!t.broken.empty())
    {
        plan.why_not = t.broken;
        return plan;
    }
    if (image.format.rfind("JPEG", 0) != 0 || image.is_raw)
    {
        plan.why_not = "the original isn't a JPEG";
        return plan;
    }
    std::vector<uint8_t> src;
    if (!read_all(path, src))
    {
        plan.why_not = "the original file can't be read";
        return plan;
    }
    const TurboJpeg* tj = turbojpeg();
    if (!tj)
    {
        plan.why_not = "it needs the TurboJPEG library (the libturbojpeg0 or turbojpeg package)";
        return plan;
    }
    tjhandle handle = tj->init(TJINIT_TRANSFORM);
    if (!handle)
    {
        plan.why_not = "the lossless JPEG transform isn't available";
        return plan;
    }
    struct Done
    {
        const TurboJpeg* tj;
        tjhandle handle;
        ~Done() { tj->destroy(handle); }
    } done{tj, handle};
    if (tj->header(handle, src.data(), src.size()) != 0)
    {
        plan.why_not = std::string("the original can't be read: ") + tj->error(handle);
        return plan;
    }
    const int W = tj->get(handle, TJPARAM_JPEGWIDTH), H = tj->get(handle, TJPARAM_JPEGHEIGHT), subsamp = tj->get(handle, TJPARAM_SUBSAMP);
    // Untouched since it was read, the image is the whole file upright.
    const int orientation = t.width ? t.orientation : 1;
    const bool turned = orientation >= 5;
    int x = 0, y = 0, w = turned ? H : W, h = turned ? W : H;
    if (t.width)
    {
        if (t.width != W || t.height != H)
        {
            plan.why_not = "the original file has changed since it was opened";
            return plan;
        }
        x = t.x, y = t.y, w = t.w, h = t.h;
    }
    if (subsamp < 0 || subsamp >= TJ_NUMSAMP)
    {
        plan.why_not = "the original's chroma subsampling isn't one the transform handles";
        return plan;
    }
    // The original's partial blocks are at its right and bottom edges. A turn
    // or flip that brings them to the left or top drops them (they can't be
    // moved losslessly): that strip goes from the turned image, which the
    // transform's own coordinates then start after.
    const int part_w = W % tjMCUWidth[subsamp], part_h = H % tjMCUHeight[subsamp];
    int trim_left = 0, trim_top = 0;
    switch (orientation)
    {
        case 2: trim_left = part_w; break;                      // flipped across: right to left
        case 3: trim_left = part_w, trim_top = part_h; break;   // half turn
        case 4: trim_top = part_h; break;                       // flipped down: bottom to top
        case 6: trim_left = part_h; break;                      // clockwise: bottom to left
        case 7: trim_left = part_h, trim_top = part_w; break;   // transverse
        case 8: trim_top = part_w; break;                       // anticlockwise: right to top
        default: break;                                         // transpose keeps them at right and bottom
    }
    const int full_w = (turned ? H : W) - trim_left, full_h = (turned ? W : H) - trim_top;
    // The crop in the transform's coordinates, its corner out to a block's.
    const int block_w = turned ? tjMCUHeight[subsamp] : tjMCUWidth[subsamp];
    const int block_h = turned ? tjMCUWidth[subsamp] : tjMCUHeight[subsamp];
    const int cx = std::max(0, x - trim_left), cy = std::max(0, y - trim_top);
    const int x0 = cx / block_w * block_w, y0 = cy / block_h * block_h;
    const int x1 = std::min(full_w, x + w - trim_left), y1 = std::min(full_h, y + h - trim_top);
    if (x1 <= x0 || y1 <= y0)
    {
        plan.why_not = "the crop lies in the edge blocks a lossless turn has to drop";
        return plan;
    }
    static const int kOps[] = {TJXOP_NONE, TJXOP_NONE, TJXOP_HFLIP, TJXOP_ROT180, TJXOP_VFLIP, TJXOP_TRANSPOSE, TJXOP_ROT90, TJXOP_TRANSVERSE, TJXOP_ROT270};
    tjtransform xf{};
    xf.op = kOps[orientation];
    xf.options = TJXOPT_TRIM | TJXOPT_COPYNONE;
    if (x0 != 0 || y0 != 0 || x1 != full_w || y1 != full_h)
    {
        xf.options |= TJXOPT_CROP;
        xf.r = {x0, y0, x1 - x0, y1 - y0};
    }
    unsigned char* dst = nullptr;
    size_t dst_size = 0;
    if (tj->transform(handle, src.data(), src.size(), 1, &dst, &dst_size, &xf) != 0)
    {
        plan.why_not = std::string("the transform failed: ") + tj->error(handle);
        tj->free(dst);
        return plan;
    }
    plan.data.assign(dst, dst + dst_size);
    tj->free(dst);
    if (tj->header(handle, plan.data.data(), plan.data.size()) != 0)
    {
        plan.why_not = "the transform's result can't be read";
        plan.data.clear();
        return plan;
    }
    plan.width = tj->get(handle, TJPARAM_JPEGWIDTH);
    plan.height = tj->get(handle, TJPARAM_JPEGHEIGHT);
    // Against the image as shown, in the turned image's coordinates: the
    // result starts at the trimmed strip plus the block corner.
    const int start_x = trim_left + x0, start_y = trim_top + y0;
    plan.differs = describe(x - start_x, y - start_y, start_x + plan.width - (x + w), start_y + plan.height - (y + h));
    return plan;
#endif
}

bool write_lossless_jpeg(const LosslessJpeg& plan, const FileMetadata* metadata, const std::string& out, std::string& error)
{
    if (plan.data.size() < 4 || plan.data[0] != 0xFF || plan.data[1] != 0xD8)
    {
        error = "nothing to write";
        return false;
    }
    std::vector<uint8_t> file = {0xFF, 0xD8};
    size_t rest = 2;
    if (metadata)
    {
        const std::vector<uint8_t> segments = metadata_segments(*metadata);
        // EXIF's APP1 belongs straight after SOI: a JFIF APP0 there goes.
        if (!metadata->exif.empty() && plan.data.size() > 6 && plan.data[2] == 0xFF && plan.data[3] == 0xE0)
        {
            const size_t len = size_t(plan.data[4]) << 8 | plan.data[5];
            if (4 + len <= plan.data.size())
                rest = 4 + len;
        }
        file.insert(file.end(), segments.begin(), segments.end());
    }
    file.insert(file.end(), plan.data.begin() + long(rest), plan.data.end());
    FILE* f = platform::fopen(out, "wb");
    if (!f)
    {
        error = "cannot create file";
        return false;
    }
    const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    if (std::fclose(f) != 0 || !ok)
    {
        error = "write failed";
        return false;
    }
    return true;
}
