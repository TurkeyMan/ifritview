#pragma once

#include "../image.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

// One image format backend. Decoders are plain tables of functions so the
// registry can try them by extension first, then by sniffing the file header.
struct Decoder
{
    const char* name;
    std::vector<std::string> extensions;             // lowercase, without the dot
    bool (*sniff)(const uint8_t* head, size_t len);  // may be null
    bool (*decode)(const std::string& path, Image& out, std::string& error);
    // Optional quick approximation (a raw's embedded JPEG) shown while the
    // real decode runs. May be null; returns false when there is none.
    bool (*preview)(const std::string& path, Image& out) = nullptr;
};

// Options the decoders consult. Set from the main thread, read from the
// loader thread; copied under a lock so a settings change can't tear.
struct DecodeOptions
{
    enum class RawMode
    {
        Full,
        Half,
        Preview
    };
    RawMode raw_mode = RawMode::Full;
    bool raw_camera_crop = true;  // frame as the camera did, dropping the sensor margins it never shows
};
void set_decode_options(const DecodeOptions& options);
DecodeOptions decode_options();

// The file being decoded, read once per decode on this thread. Every reader
// (the decoder, EXIF, XMP, the format sniffers) takes its bytes from here
// rather than opening the path again, which matters on slow filesystems.
// Returns false if the file can't be read.
bool file_bytes(const std::string& path, const uint8_t*& data, size_t& size);
// A FILE* over those bytes, for libraries that want one (libjpeg, libpng).
FILE* open_file(const std::string& path);
// Forget the cached bytes (the next file_bytes reads afresh).
void release_file_bytes();

// Compiled-in decoders in priority order.
const std::vector<const Decoder*>& decoders();

// Decode a file with whichever decoder claims it; `error` describes the
// failure of the most relevant decoder.
bool decode_image(const std::string& path, Image& out, std::string& error);

// A quick stand-in for the file if its decoder offers one.
bool decode_preview(const std::string& path, Image& out);

// Every extension handled by the compiled-in decoders, in priority order.
std::vector<std::string> default_extensions();

// Backend constructors, each defined in its own file; only those compiled in
// are referenced from decoder.cpp.
const Decoder& jpeg_decoder();
const Decoder& png_decoder();
const Decoder& webp_decoder();
const Decoder& heif_decoder();
const Decoder& jxl_decoder();
const Decoder& tiff_decoder();
const Decoder& raw_decoder();
const Decoder& stb_decoder();
const Decoder& pixbuf_decoder();
const Decoder& pdf_decoder();
const Decoder& tim_decoder();
const Decoder& ilbm_decoder();
const Decoder& pcx_decoder();

#ifdef HAVE_JPEG
// Used by the raw decoder for embedded JPEG previews.
bool decode_jpeg_memory(const uint8_t* data, size_t size, Image& out, std::string& error);
#endif

#ifdef HAVE_IPTC
void read_iptc_jpeg(const uint8_t* data, size_t size, Image& out);
void read_iptc_blob(const uint8_t* data, size_t size, Image& out);
#endif
#ifdef HAVE_XMP
// Embedded XMP packet plus any .xmp sidecar next to the file.
void read_xmp(const std::string& path, const uint8_t* data, size_t size, Image& out);
#endif

#ifdef HAVE_EXIF
// Parse an EXIF payload (a JPEG file, a bare "Exif\0\0..." blob, or a TIFF
// header) into the image's metadata groups. Returns the orientation, 1 when
// absent, and the camera model when known.
struct ExifSummary
{
    int orientation = 1;
    std::string camera;
    std::string color_space;  // "sRGB" / "Adobe RGB (1998)" when declared
};
ExifSummary read_exif_file(const std::string& path, Image& out);
ExifSummary read_exif_blob(const uint8_t* data, size_t size, Image& out);
#endif
