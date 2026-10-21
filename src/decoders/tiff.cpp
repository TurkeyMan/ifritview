#include "decoder.h"

#include <tiffio.h>

#include <cstring>

namespace {

bool sniff(const uint8_t* h, size_t n) {
    return n >= 4 && (std::memcmp(h, "II*\0", 4) == 0 || std::memcmp(h, "MM\0*", 4) == 0 ||
                      std::memcmp(h, "II+\0", 4) == 0 || std::memcmp(h, "MM\0+", 4) == 0);  // BigTIFF
}

bool decode(const std::string& path, Image& out, std::string& error) {
    static const bool quiet = [] {
        TIFFSetErrorHandler(nullptr);
        TIFFSetWarningHandler(nullptr);
        return true;
    }();
    (void)quiet;

    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) {
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
    if (w == 0 || h == 0 || size_t(w) * h > size_t(1) << 30) {
        error = "invalid TIFF dimensions";
        TIFFClose(tif);
        return false;
    }

    // libtiff composes any photometric/compression combination into packed
    // RGBA for us; this is the pragmatic route for a viewer.
    std::vector<uint32_t> raster(size_t(w) * h);
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT, 0)) {
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
    switch (compression) {
        case COMPRESSION_LZW: out.format += ", LZW"; break;
        case COMPRESSION_JPEG: out.format += ", JPEG"; break;
        case COMPRESSION_DEFLATE: case COMPRESSION_ADOBE_DEFLATE: out.format += ", deflate"; break;
        case COMPRESSION_PACKBITS: out.format += ", PackBits"; break;
        default: break;
    }
    const int pages = int(TIFFNumberOfDirectories(tif));
    if (pages > 1) out.info = std::to_string(pages) + " pages";

    const struct { ttag_t tag; const char* name; } strings[] = {
        {TIFFTAG_IMAGEDESCRIPTION, "Description"}, {TIFFTAG_MAKE, "Make"}, {TIFFTAG_MODEL, "Model"},
        {TIFFTAG_SOFTWARE, "Software"}, {TIFFTAG_DATETIME, "Date/time"}, {TIFFTAG_ARTIST, "Artist"},
        {TIFFTAG_COPYRIGHT, "Copyright"}, {TIFFTAG_HOSTCOMPUTER, "Host computer"},
    };
    for (const auto& t : strings) {
        char* v = nullptr;
        if (TIFFGetField(tif, t.tag, &v) && v && *v) out.add("TIFF", t.name, v);
    }
    out.add("TIFF", "Bits per sample", std::to_string(bps));
    out.add("TIFF", "Samples per pixel", std::to_string(spp));
    out.add("TIFF", "Compression", out.format.size() > 4 ? out.format.substr(6) : "none");
    float xres = 0;
    uint16_t unit = RESUNIT_INCH;
    if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres) && xres > 0) {
        TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &unit);
        out.add("TIFF", "Resolution", std::to_string(int(unit == RESUNIT_CENTIMETER ? xres * 2.54 : xres)) + " dpi");
    }
    if (pages > 1) out.add("TIFF", "Pages", std::to_string(pages));
    toff_t exif_offset = 0;
    if (TIFFGetField(tif, TIFFTAG_EXIFIFD, &exif_offset) && TIFFReadEXIFDirectory(tif, exif_offset)) {
        char* sv = nullptr;
        float fv = 0;
        if (TIFFGetField(tif, EXIFTAG_DATETIMEORIGINAL, &sv) && sv) out.add("EXIF", "Date/time original", sv);
        if (TIFFGetField(tif, EXIFTAG_EXPOSURETIME, &fv)) out.add("EXIF", "Exposure time", fv >= 1 ? std::to_string(fv).substr(0, 4) + " s" : "1/" + std::to_string(int(1 / fv + 0.5)) + " s");
        if (TIFFGetField(tif, EXIFTAG_FNUMBER, &fv)) out.add("EXIF", "F-number", "f/" + std::to_string(fv).substr(0, 4));
        if (TIFFGetField(tif, EXIFTAG_FOCALLENGTH, &fv)) out.add("EXIF", "Focal length", std::to_string(fv).substr(0, 5) + " mm");
        uint16_t count = 0;
        uint16_t* iso = nullptr;
        if (TIFFGetField(tif, EXIFTAG_ISOSPEEDRATINGS, &count, &iso) && count > 0) out.add("EXIF", "ISO", std::to_string(iso[0]));
        if (TIFFGetField(tif, EXIFTAG_LENSMODEL, &sv) && sv) out.add("EXIF", "Lens", sv);
    }
    TIFFClose(tif);
    return true;
}

}  // namespace

const Decoder& tiff_decoder() {
    static const Decoder d{"TIFF", {"tif", "tiff"}, sniff, decode};
    return d;
}
