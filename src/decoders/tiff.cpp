#include "decoder.h"
#include "../i18n.h"
#include "../platform.h"

#include "icc.h"

#include <glib.h>
#include <tiffio.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 4 && (std::memcmp(h, "II*\0", 4) == 0 || std::memcmp(h, "MM\0*", 4) == 0 || std::memcmp(h, "II+\0", 4) == 0 ||
                      std::memcmp(h, "MM\0+", 4) == 0);  // BigTIFF
}

// The deep cases keep their samples as they are, as the image's source:
// 16-bit integer (display-encoded; signed too) and 16/32-bit float (linear
// light, which may go past 1), grey or RGB, alpha unassociated or absent,
// in strips, the right way up. False for anything else, which the RGBA
// path below takes, at 8 bits.
bool read_deep(TIFF* tif, uint32_t w, uint32_t h, uint16_t bps, uint16_t spp, bool alpha, Image& out)
{
    uint16_t photometric = PHOTOMETRIC_RGB, sample_format = SAMPLEFORMAT_UINT, planar = PLANARCONFIG_CONTIG, orientation = ORIENTATION_TOPLEFT;
    uint16_t extra_count = 0;
    uint16_t* extra = nullptr;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation);
    using S = SourceData::Sample;
    S sample;
    if (sample_format == SAMPLEFORMAT_IEEEFP && (bps == 16 || bps == 32))
        sample = bps == 32 ? S::F32 : S::F16;
    else if (sample_format == SAMPLEFORMAT_UINT && bps == 16)
        sample = S::U16;
    else if (sample_format == SAMPLEFORMAT_INT && bps == 16)
        sample = S::I16;
    else
        return false;
    const int colours = photometric == PHOTOMETRIC_RGB ? 3 : photometric == PHOTOMETRIC_MINISBLACK ? 1 : 0;
    if (!colours || spp < colours + (alpha ? 1 : 0) || planar != PLANARCONFIG_CONTIG || orientation != ORIENTATION_TOPLEFT || TIFFIsTiled(tif))
        return false;
    if (alpha && TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extra_count, &extra) && extra_count && extra[0] == EXTRASAMPLE_ASSOCALPHA)
        return false;  // premultiplied samples: the RGBA path undoes that
    const size_t sample_bytes = bps / 8;
    const tmsize_t line = TIFFScanlineSize(tif);
    if (line < tmsize_t(size_t(w) * spp * sample_bytes))
        return false;
    const bool is_float = sample == S::F16 || sample == S::F32;
    auto deep = std::make_shared<SourceData>(sample, colours + (alpha ? 1 : 0), int(w), int(h), is_float);
    std::vector<uint8_t> buf(static_cast<size_t>(line));
    for (uint32_t y = 0; y < h; ++y)
    {
        if (TIFFReadScanline(tif, buf.data(), y, 0) < 0)
            return false;
        // The colours and alpha, past any other extra samples (in this byte order already).
        for (uint32_t x = 0; x < w; ++x)
            std::memcpy(deep->pixel(int(x), int(y)), buf.data() + size_t(x) * spp * sample_bytes, deep->pixel_bytes());
    }
    out.set_source(std::move(deep));
    return true;
}

bool decode_part(const std::string& path, ImagePart part, Image& out, std::string& error)
{
    static const bool quiet = []
    {
        TIFFSetErrorHandler(nullptr);
        TIFFSetWarningHandler(nullptr);
        return true;
    }();
    (void)quiet;

#ifdef _WIN32
    TIFF* tif = TIFFOpenW(platform::widen(path).c_str(), "r");
#else
    TIFF* tif = TIFFOpen(path.c_str(), "r");
#endif
    if (!tif)
    {
        error = "cannot open TIFF";
        return false;
    }
    // Each directory is a page (a scan's pages, a layered export's layers).
    const int pages = int(TIFFNumberOfDirectories(tif));
    if (part.page > 0 && (part.page >= pages || !TIFFSetDirectory(tif, tdir_t(part.page))))
    {
        TIFFClose(tif);
        error = "the TIFF has no such page";
        return false;
    }
    out.page_count = std::max(1, pages);
    if (uint32_t kind = 0; TIFFGetField(tif, TIFFTAG_SUBFILETYPE, &kind) && (kind & FILETYPE_REDUCEDIMAGE))
        out.page_label = _("(a smaller copy)");
    uint32_t w = 0, h = 0;
    uint16_t bps = 8, spp = 0, extra_count = 0;
    uint16_t* extra = nullptr;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    bool alpha = false;
    if (TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extra_count, &extra) && extra_count > 0)
        alpha = extra[0] == EXTRASAMPLE_ASSOCALPHA || extra[0] == EXTRASAMPLE_UNASSALPHA;
    if (!Image::fits(w, h))
    {
        error = "invalid TIFF dimensions";
        TIFFClose(tif);
        return false;
    }

    // Otherwise libtiff composes any photometric/compression combination
    // into packed RGBA for us; this is the pragmatic route for a viewer.
    if (!read_deep(tif, w, h, bps, spp, alpha, out))
    {
        std::vector<uint32_t> raster(size_t(w) * h);
        if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT, 0))
        {
            error = "TIFF decode failed";
            TIFFClose(tif);
            return false;
        }
        out.allocate(int(w), int(h));
        out.from_rgba8(reinterpret_cast<const uint8_t*>(raster.data()), int(w) * 4);
    }
    out.has_alpha = alpha;
    out.bits_per_sample = bps;
    out.format = "TIFF";
    uint16_t compression = COMPRESSION_NONE;
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
    // As the save dialog's choices: deflate, LZW, none.
    out.encoded_as = "tif";
    switch (compression)
    {
        case COMPRESSION_LZW:
            out.format += ", LZW";
            out.encoding["compression"] = 1;
            break;
        case COMPRESSION_JPEG:
            out.format += ", JPEG";
            break;
        case COMPRESSION_DEFLATE:
        case COMPRESSION_ADOBE_DEFLATE:
            out.format += ", deflate";
            out.encoding["compression"] = 0;
            break;
        case COMPRESSION_NONE:
            out.encoding["compression"] = 2;
            break;
        case COMPRESSION_PACKBITS:
            out.format += ", PackBits";
            break;
        default:
            break;
    }

    const struct
    {
        ttag_t tag;
        const char* name;
    } strings[] = {
        {TIFFTAG_IMAGEDESCRIPTION, "Description"}, {TIFFTAG_MAKE, "Make"},     {TIFFTAG_MODEL, "Model"},         {TIFFTAG_SOFTWARE, "Software"},
        {TIFFTAG_DATETIME, "Date/time"},           {TIFFTAG_ARTIST, "Artist"}, {TIFFTAG_COPYRIGHT, "Copyright"}, {TIFFTAG_HOSTCOMPUTER, "Host computer"},
    };
    for (const auto& t : strings)
    {
        char* v = nullptr;
        if (TIFFGetField(tif, t.tag, &v) && v && *v)
            out.add("TIFF", t.name, v);
    }
    {
        using M = PixelFormat::Model;
        uint16_t photometric = PHOTOMETRIC_RGB, sample_format = SAMPLEFORMAT_UINT, planar = PLANARCONFIG_CONTIG;
        TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric);
        TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);
        TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
        std::string names;
        M model = M::RGB;
        switch (photometric)
        {
            case PHOTOMETRIC_PALETTE:
                model = M::Palette;
                break;
            case PHOTOMETRIC_SEPARATED:
                model = M::CMYK;
                names = "CMYK";
                break;
            case PHOTOMETRIC_YCBCR:
                model = M::YCbCr;
                names = "YUV";
                break;
            case PHOTOMETRIC_CIELAB:
            case PHOTOMETRIC_ICCLAB:
                model = M::Lab;
                names = "Lab";
                break;
            case PHOTOMETRIC_RGB:
                names = "RGB";
                break;
            default:
                model = M::L;
                names = "L";
                break;
        }
        if (model == M::Palette)
        {
            out.pixel_format = PixelFormat::palette(bps, 1 << std::min<int>(bps, 16));
        }
        else
        {
            for (int i = int(names.size()); i < spp; ++i)
                names += alpha && i == int(names.size()) ? 'A' : 'X';
            out.pixel_format = PixelFormat::packed(model, names.c_str(), bps);
            if (sample_format == SAMPLEFORMAT_IEEEFP)
                out.pixel_format.sample = PixelFormat::Sample::Float;
            else if (sample_format == SAMPLEFORMAT_INT)
                out.pixel_format.sample = PixelFormat::Sample::Signed;
            if (planar == PLANARCONFIG_SEPARATE)
                out.pixel_format.planar = true;
            if (alpha && extra && extra[0] == EXTRASAMPLE_ASSOCALPHA)
                out.pixel_format.premultiplied = true;
        }
    }
    {
        uint32_t icc_len = 0;
        void* icc = nullptr;
        if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &icc_len, &icc) && icc && icc_len)
            out.color_space = icc_profile_description(static_cast<const uint8_t*>(icc), icc_len);
    }
    out.add(N_("TIFF"), N_("Bits per sample"), std::to_string(bps));
    out.add(N_("TIFF"), N_("Samples per pixel"), std::to_string(spp));
    out.add(N_("TIFF"), N_("Compression"), out.format.size() > 4 ? out.format.substr(6) : "none");
    float xres = 0;
    uint16_t unit = RESUNIT_INCH;
    if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres) && xres > 0)
    {
        TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &unit);
        out.add(N_("TIFF"), N_("Resolution"), std::to_string(int(unit == RESUNIT_CENTIMETER ? xres * 2.54 : xres)) + " dpi");
    }
    if (pages > 1)
        out.add(N_("TIFF"), N_("Pages"), std::to_string(pages));
    FileMetadata stored;  // as the file has it, for saving
    {
        uint32_t len = 0;
        void* bytes = nullptr;
        if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &len, &bytes) && bytes && len)
            stored.icc.assign(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + len);
        if (TIFFGetField(tif, TIFFTAG_XMLPACKET, &len, &bytes) && bytes && len)
            stored.xmp.assign(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + len);
    }
    uint32_t iptc_count = 0;
    void* iptc = nullptr;
    if (TIFFGetField(tif, TIFFTAG_RICHTIFFIPTC, &iptc_count, &iptc) && iptc && iptc_count)
    {
        // Older writers store this as LONGs; libtiff byte-swaps those, so undo it.
        std::vector<uint8_t> blob(
            static_cast<const uint8_t*>(iptc),
            static_cast<const uint8_t*>(iptc) + size_t(iptc_count) * (TIFFFieldDataType(TIFFFieldWithTag(tif, TIFFTAG_RICHTIFFIPTC)) == TIFF_LONG ? 4 : 1));
        if (TIFFIsByteSwapped(tif) && TIFFFieldDataType(TIFFFieldWithTag(tif, TIFFTAG_RICHTIFFIPTC)) == TIFF_LONG)
            for (size_t i = 0; i + 3 < blob.size(); i += 4)
                std::swap(blob[i], blob[i + 3]), std::swap(blob[i + 1], blob[i + 2]);
        stored.iptc = blob;
#ifdef HAVE_IPTC
        read_iptc_blob(blob.data(), blob.size(), out);
#endif
    }
    if (!stored.empty())
        out.stored = std::make_shared<const FileMetadata>(std::move(stored));
    toff_t exif_offset = 0;
    if (TIFFGetField(tif, TIFFTAG_EXIFIFD, &exif_offset) && TIFFReadEXIFDirectory(tif, exif_offset))
    {
        char* sv = nullptr;
        float fv = 0;
        if (TIFFGetField(tif, EXIFTAG_DATETIMEORIGINAL, &sv) && sv)
            out.add(N_("EXIF"), N_("Date/time original"), sv);
        if (TIFFGetField(tif, EXIFTAG_EXPOSURETIME, &fv))
            out.add(N_("EXIF"), N_("Exposure time"), fv >= 1 ? std::to_string(fv).substr(0, 4) + " s" : "1/" + std::to_string(int(1 / fv + 0.5)) + " s");
        if (TIFFGetField(tif, EXIFTAG_FNUMBER, &fv))
            out.add(N_("EXIF"), N_("F-number"), "f/" + std::to_string(fv).substr(0, 4));
        if (TIFFGetField(tif, EXIFTAG_FOCALLENGTH, &fv))
            out.add(N_("EXIF"), N_("Focal length"), std::to_string(fv).substr(0, 5) + " mm");
        uint16_t count = 0;
        uint16_t* iso = nullptr;
        if (TIFFGetField(tif, EXIFTAG_ISOSPEEDRATINGS, &count, &iso) && count > 0)
            out.add(N_("EXIF"), N_("ISO"), std::to_string(iso[0]));
        if (TIFFGetField(tif, EXIFTAG_LENSMODEL, &sv) && sv)
            out.add(N_("EXIF"), N_("Lens"), sv);
    }
    TIFFClose(tif);
    return true;
}

}  // namespace

const Decoder& tiff_decoder()
{
    static const Decoder d{"TIFF", {"tif", "tiff"}, sniff, [](const std::string& path, Image& out, std::string& error)
                           { return decode_part(path, {}, out, error); }, nullptr, false, decode_part};
    return d;
}
