#pragma once

#include "../image.h"

#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// One image format backend. Decoders are plain tables of functions so the
// registry can try them by extension first, then by sniffing the file header.
// The built-in ones are free functions; a plugin's carry their format along
// (src/plugins.cpp), hence std::function.
struct Decoder
{
    const char* name;
    std::vector<std::string> extensions;                          // lowercase, without the dot
    std::function<bool(const uint8_t* head, size_t len)> sniff;   // may be empty
    std::function<bool(const std::string& path, Image& out, std::string& error)> decode;
    // Optional quick approximation (a raw's embedded JPEG) shown while the
    // real decode runs. May be empty; returns false when there is none.
    std::function<bool(const std::string& path, Image& out)> preview = nullptr;
    // Reads the file itself (videos): no preload, no container metadata pass.
    bool streaming = false;
    // Any page and mip level of the file (ImagePart), for a format that has
    // more than one; the decoded image says how many there are. Unset: the
    // file is one image, and `decode` makes it.
    std::function<bool(const std::string& path, ImagePart part, Image& out, std::string& error)> decode_part = nullptr;
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
    // Raws (Bayer) only unpacked, for the GPU to develop (Image::mosaic): the
    // viewer, when it draws on the GPU. Else LibRaw develops them here.
    bool gpu_develop = false;
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

// Decoders in priority order: the compiled-in ones, then the plugins' once
// they're loaded, then the gdk-pixbuf catch-all. Safe from any thread; the
// list returned stays valid (a new one replaces it, the old one is kept).
const std::vector<const Decoder*>& decoders();

// Adds the plugins' decoders after the built-in ones (src/plugins.cpp).
void register_decoders(const std::vector<const Decoder*>& extra);

// Decode a file with whichever decoder claims it; `error` describes the
// failure of the most relevant decoder.
bool decode_image(const std::string& path, Image& out, std::string& error);
// One page and mip level of it; {0, 0} is decode_image.
bool decode_image_part(const std::string& path, ImagePart part, Image& out, std::string& error);

// A quick stand-in for the file if its decoder offers one.
bool decode_preview(const std::string& path, Image& out);

// Every extension handled by the decoders, in priority order.
std::vector<std::string> default_extensions();

// Whether some available decoder handles this extension. Optional libraries
// (GStreamer, poppler) that aren't installed take their extensions with them.
bool extension_supported(const std::string& ext);

// A path's extension, lower case, without the dot ("jpg"); empty when its
// last part has none. Either slash separates parts.
std::string file_extension(const std::string& path);
// An extension as typed in a list (" .JPG"): lower case, no dot or spaces.
std::string normalise_extension(std::string ext);

// Load the optional libraries (GStreamer, poppler) on a low-priority thread,
// so the first video or PDF opens without the wait. Called once the first
// image is on screen; later calls do nothing.
void preload_optional_libraries();

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
bool pdf_available();  // poppler installed (cheap: no load)
bool pdf_supported();  // loads poppler if needed; whether PDFs can render
const Decoder& tim_decoder();
const Decoder& video_decoder();
const Decoder& ilbm_decoder();
const Decoder& pcx_decoder();
const Decoder& netpbm_decoder();
const Decoder& dds_decoder();
const Decoder& vtf_decoder();
const Decoder& blp_decoder();
const Decoder& tim2_decoder();
const Decoder& gim_decoder();
const Decoder& wal_decoder();
const Decoder& lmp_decoder();

// The 256-colour VGA palette trailing an 8-bit PCX, as straight 0xFFRRGGBB.
// False if the file has none. (Quake 2 keeps its palette in colormap.pcx.)
bool pcx_vga_palette(const uint8_t* data, size_t size, uint32_t palette[256]);
const Decoder& pkm_decoder();
const Decoder& astc_decoder();
const Decoder& ktx_decoder();
const Decoder& ktx2_decoder();
const Decoder& tpl_decoder();
const Decoder& bti_decoder();
const Decoder& pvr_decoder();

#ifdef HAVE_JPEG
// Used by the raw decoder for embedded JPEG previews.
bool decode_jpeg_memory(const uint8_t* data, size_t size, Image& out, std::string& error);
// The samples as stored, `components` bytes a pixel: four-component data
// is left as it is (no CMYK or YCCK conversion; BLP1 keeps B, G, R, A
// there), three-component data is converted to RGB as usual.
bool decode_jpeg_samples(const uint8_t* data, size_t size, std::vector<uint8_t>& samples, int& width, int& height,
                         int& components, std::string& error);
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
ExifSummary read_exif_blob(const uint8_t* data, size_t size, Image& out);
#endif

const Decoder& exr_decoder();
