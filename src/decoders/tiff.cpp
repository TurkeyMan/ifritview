#include "decoder.h"
#include "icc.h"

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

bool decode(const std::string& path, Image& out, std::string& error)
{
    static const bool quiet = []
    {
        TIFFSetErrorHandler(nullptr);
        TIFFSetWarningHandler(nullptr);
        return true;
    }();
    (void)quiet;

    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif)
    {
        error = "cannot open TIFF";
        return false;
    }
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
    if (w == 0 || h == 0 || size_t(w) * h > size_t(1) << 30)
    {
        error = "invalid TIFF dimensions";
        TIFFClose(tif);
        return false;
    }

    // libtiff composes any photometric/compression combination into packed
    // RGBA for us; this is the pragmatic route for a viewer.
    std::vector<uint32_t> raster(size_t(w) * h);
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT, 0))
    {
        error = "TIFF decode failed";
        TIFFClose(tif);
        return false;
    }
    out.allocate(int(w), int(h));
    out.from_rgba8(reinterpret_cast<const uint8_t*>(raster.data()), int(w) * 4);
    out.has_alpha = alpha;
    out.bits_per_sample = bps;
    out.format = "TIFF";
    uint16_t compression = COMPRESSION_NONE;
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
    switch (compression)
    {
        case COMPRESSION_LZW:
            out.format += ", LZW";
            break;
        case COMPRESSION_JPEG:
            out.format += ", JPEG";
            break;
        case COMPRESSION_DEFLATE:
        case COMPRESSION_ADOBE_DEFLATE:
            out.format += ", deflate";
            break;
        case COMPRESSION_PACKBITS:
            out.format += ", PackBits";
            break;
        default:
            break;
    }
    const int pages = int(TIFFNumberOfDirectories(tif));
    if (pages > 1)
        out.info = std::to_string(pages) + " pages";

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
                model = M::Gray;
                names = "Y";
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
    out.add("TIFF", "Bits per sample", std::to_string(bps));
    out.add("TIFF", "Samples per pixel", std::to_string(spp));
    out.add("TIFF", "Compression", out.format.size() > 4 ? out.format.substr(6) : "none");
    float xres = 0;
    uint16_t unit = RESUNIT_INCH;
    if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres) && xres > 0)
    {
        TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &unit);
        out.add("TIFF", "Resolution", std::to_string(int(unit == RESUNIT_CENTIMETER ? xres * 2.54 : xres)) + " dpi");
    }
    if (pages > 1)
        out.add("TIFF", "Pages", std::to_string(pages));
#ifdef HAVE_IPTC
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
        read_iptc_blob(blob.data(), blob.size(), out);
    }
#endif
    toff_t exif_offset = 0;
    if (TIFFGetField(tif, TIFFTAG_EXIFIFD, &exif_offset) && TIFFReadEXIFDirectory(tif, exif_offset))
    {
        char* sv = nullptr;
        float fv = 0;
        if (TIFFGetField(tif, EXIFTAG_DATETIMEORIGINAL, &sv) && sv)
            out.add("EXIF", "Date/time original", sv);
        if (TIFFGetField(tif, EXIFTAG_EXPOSURETIME, &fv))
            out.add("EXIF", "Exposure time", fv >= 1 ? std::to_string(fv).substr(0, 4) + " s" : "1/" + std::to_string(int(1 / fv + 0.5)) + " s");
        if (TIFFGetField(tif, EXIFTAG_FNUMBER, &fv))
            out.add("EXIF", "F-number", "f/" + std::to_string(fv).substr(0, 4));
        if (TIFFGetField(tif, EXIFTAG_FOCALLENGTH, &fv))
            out.add("EXIF", "Focal length", std::to_string(fv).substr(0, 5) + " mm");
        uint16_t count = 0;
        uint16_t* iso = nullptr;
        if (TIFFGetField(tif, EXIFTAG_ISOSPEEDRATINGS, &count, &iso) && count > 0)
            out.add("EXIF", "ISO", std::to_string(iso[0]));
        if (TIFFGetField(tif, EXIFTAG_LENSMODEL, &sv) && sv)
            out.add("EXIF", "Lens", sv);
    }
    TIFFClose(tif);
    return true;
}

}  // namespace

const Decoder& tiff_decoder()
{
    static const Decoder d{"TIFF", {"tif", "tiff"}, sniff, decode};
    return d;
}
