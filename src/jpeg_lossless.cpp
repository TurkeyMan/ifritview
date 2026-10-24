#include "jpeg_lossless.h"

#include "i18n.h"
#include "platform.h"

#include <type_traits>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>

#include <mutex>
#endif

namespace
{

#ifdef HAVE_TURBOJPEG
// TurboJPEG's functions. On Windows it's linked in; on Linux it's loaded
// when first wanted, so the library is a recommended package, not a
// required one: without it, lossless saving says it needs it. The 2.x
// API, which 3.x keeps: Debian 13 still has 2.1, under the same soname.
struct TurboJpeg
{
    decltype(&tjInitTransform) init = nullptr;
    decltype(&tjDestroy) destroy = nullptr;
    decltype(&tjGetErrorStr2) error = nullptr;
    decltype(&tjDecompressHeader3) header = nullptr;
    decltype(&tjTransform) transform = nullptr;
    decltype(&tjFree) free = nullptr;
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
                       sym(tj.init, "tjInitTransform");
                       sym(tj.destroy, "tjDestroy");
                       sym(tj.error, "tjGetErrorStr2");
                       sym(tj.header, "tjDecompressHeader3");
                       sym(tj.transform, "tjTransform");
                       sym(tj.free, "tjFree");
#else
                       tj.init = tjInitTransform;
                       tj.destroy = tjDestroy;
                       tj.error = tjGetErrorStr2;
                       tj.header = tjDecompressHeader3;
                       tj.transform = tjTransform;
                       tj.free = tjFree;
#endif
                       ok = tj.init && tj.destroy && tj.error && tj.header && tj.transform && tj.free;
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
    // Whole phrases, so each language words them its own way.
    std::vector<std::string> parts;
    const auto part = [&](int px, const char* more, const char* fewer)
    {
        if (px)
            parts.push_back(rap::i18n::format(px > 0 ? more : fewer, std::abs(px)));
    };
    part(left, _("{0} px more on the left"), _("{0} px fewer on the left"));
    part(top, _("{0} px more at the top"), _("{0} px fewer at the top"));
    part(right, _("{0} px more on the right"), _("{0} px fewer on the right"));
    part(bottom, _("{0} px more at the bottom"), _("{0} px fewer at the bottom"));
    std::string s = parts.empty() ? std::string() : parts.back();
    for (size_t i = parts.size(); i-- > 1;)
        s = i + 1 == parts.size() ? rap::i18n::format(_("{0} and {1}"), parts[i - 1], s) : parts[i - 1] + ", " + s;
    return s;
}

}  // namespace

LosslessJpeg plan_lossless_jpeg(const Image& image, const std::string& path)
{
    LosslessJpeg plan;
#ifndef HAVE_TURBOJPEG
    (void)image;
    (void)path;
    plan.why_not = _("this build has no lossless JPEG transform");
    return plan;
#else
    const Image::Transform& t = image.transform;
    if (!t.broken.empty())
    {
        plan.why_not = t.broken;
        return plan;
    }
    if (image.encoded_as != "jpg" || image.is_raw)  // as the decoder saw the file
    {
        plan.why_not = _("the original isn't a JPEG");
        return plan;
    }
    std::vector<uint8_t> src;
    if (!read_all(path, src))
    {
        plan.why_not = _("the original file can't be read");
        return plan;
    }
    if (src.size() > std::numeric_limits<unsigned long>::max() / 2)  // the API's sizes, 32 bits on Windows
    {
        plan.why_not = _("the original is too large for the transform");
        return plan;
    }
    const TurboJpeg* tj = turbojpeg();
    if (!tj)
    {
        plan.why_not = _("it needs the TurboJPEG library (the libturbojpeg0 or turbojpeg package)");
        return plan;
    }
    tjhandle handle = tj->init();
    if (!handle)
    {
        plan.why_not = _("the lossless JPEG transform isn't available");
        return plan;
    }
    struct Done
    {
        const TurboJpeg* tj;
        tjhandle handle;
        ~Done() { tj->destroy(handle); }
    } done{tj, handle};
    int W = 0, H = 0, subsamp = -1, colorspace = 0;
    if (tj->header(handle, src.data(), static_cast<unsigned long>(src.size()), &W, &H, &subsamp, &colorspace) != 0)
    {
        plan.why_not = rap::i18n::format(_("the original can't be read: {0}"), tj->error(handle));
        return plan;
    }
    // Untouched since it was read, the image is the whole file upright.
    const int orientation = t.width ? t.orientation : 1;
    const bool turned = orientation >= 5;
    int x = 0, y = 0, w = turned ? H : W, h = turned ? W : H;
    if (t.width)
    {
        if (t.width != W || t.height != H)
        {
            plan.why_not = _("the original file has changed since it was opened");
            return plan;
        }
        x = t.x, y = t.y, w = t.w, h = t.h;
    }
    if (subsamp < 0 || subsamp >= TJ_NUMSAMP)
    {
        plan.why_not = _("the original's chroma subsampling isn't one the transform handles");
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
    // The crop in the transform's coordinates, its corner in to the next
    // block's: the result never has what the crop left out.
    const int block_w = turned ? tjMCUHeight[subsamp] : tjMCUWidth[subsamp];
    const int block_h = turned ? tjMCUWidth[subsamp] : tjMCUHeight[subsamp];
    const int cx = std::max(0, x - trim_left), cy = std::max(0, y - trim_top);
    const int x0 = (cx + block_w - 1) / block_w * block_w, y0 = (cy + block_h - 1) / block_h * block_h;
    const int x1 = std::min(full_w, x + w - trim_left), y1 = std::min(full_h, y + h - trim_top);
    if (x1 <= x0 || y1 <= y0)
    {
        plan.why_not = _("the crop is within one block of JPEG's grid, or in edge blocks a turn has to drop");
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
    unsigned long dst_size = 0;
    if (tj->transform(handle, src.data(), static_cast<unsigned long>(src.size()), 1, &dst, &dst_size, &xf, 0) != 0)
    {
        plan.why_not = rap::i18n::format(_("the transform failed: {0}"), tj->error(handle));
        tj->free(dst);
        return plan;
    }
    plan.data.assign(dst, dst + dst_size);
    tj->free(dst);
    int out_subsamp = 0;
    if (tj->header(handle, plan.data.data(), static_cast<unsigned long>(plan.data.size()), &plan.width, &plan.height, &out_subsamp, &colorspace) != 0)
    {
        plan.why_not = _("the transform's result can't be read");
        plan.data.clear();
        return plan;
    }
    // Against the image as shown, in the turned image's coordinates: the
    // result starts at the trimmed strip plus the block corner.
    const int start_x = trim_left + x0, start_y = trim_top + y0;
    plan.x = start_x - x;
    plan.y = start_y - y;
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
        const std::vector<uint8_t> segments = jpeg_metadata_segments(*metadata);
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
