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
    TIFFClose(tif);
    return true;
}

}  // namespace

const Decoder& tiff_decoder() {
    static const Decoder d{"TIFF", {"tif", "tiff"}, sniff, decode};
    return d;
}
