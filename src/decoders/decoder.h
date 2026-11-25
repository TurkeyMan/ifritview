#pragma once

#include "../image.h"

#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
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
    // Whether the real decode of this file is as quick as its stand-in (a
    // raw the GPU decodes): then it goes without one. May be empty.
    std::function<bool(const std::string& path)> quick = nullptr;
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
    // And those of them this decoder takes (a CR2's lossless JPEG) left
    // compressed for the GPU to decode too (RawMosaic::stream): the CPU only
    // reads the file.
    bool gpu_decode = false;
    // Raws as LibRaw develops them at dcraw's defaults, a reference shown as
    // it is (Image::reference), rather than our linear CPU develop: only for
    // the develop panel's LibRaw option (develop_raws_as_reference()).
    bool libraw_reference = false;
};
void set_decode_options(const DecodeOptions& options);
DecodeOptions decode_options();
// This thread's decodes develop raws on the CPU (LibRaw's develop) whatever
// the options say: for comparing the GPU's develop with it.
void develop_raws_on_cpu(bool on);
// This thread's decodes develop raws as LibRaw's reference (on the CPU,
// DecodeOptions::libraw_reference): for the develop panel's LibRaw option.
void develop_raws_as_reference(bool on);

// The file being decoded, read once per decode on this thread. Every reader
// (the decoder, EXIF, XMP, the format sniffers) takes its bytes from here
// rather than opening the path again, which matters on slow filesystems.
// Returns false if the file can't be read.
bool file_bytes(const std::string& path, const uint8_t*& data, size_t& size);
// A FILE* over those bytes, for libraries that want one (libjpeg, libpng).
FILE* open_file(const std::string& path);
// Forget the cached bytes (the next file_bytes reads afresh).
void release_file_bytes();
// What holds the bytes file_bytes gave last, for what keeps pointing into
// them after the decode (a raw's readout, uploaded as the file stores it).
std::shared_ptr<const void> file_bytes_owner();
// A whole file read into memory, to hand to whichever thread decodes it.
struct FileBlob
{
    PixelBuffer data;  // huge-page backed when large: a 25 MB raw faults in a dozen times, not thousands
    bool ok = false;
    FileStamp stamp;  // the file's size and time as it was read
};
std::shared_ptr<const FileBlob> read_file(const std::string& path);
// This thread's next decode of `path` takes its bytes from `blob` (read by
// another thread) instead of reading the file, until release_file_bytes.
void provide_file_bytes(const std::string& path, std::shared_ptr<const FileBlob> blob);

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
// Whether the file's decode is quick enough to need no stand-in (Decoder::quick).
bool decode_is_quick(const std::string& path);
// This thread's stand-ins (decode_preview) need cover only width x height
// pixels, either way round: a JPEG one is decoded at 1/2, 1/4 or 1/8 size
// when that still does. 0: full size.
void set_preview_fit(int width, int height);
void preview_fit(int& width, int& height);

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
// How a raw was taken (camera, lens, focal length, aperture), from its
// headers alone, framed as its whole active area; false if it isn't one.
namespace lenses
{
struct Shot;
}
bool raw_shot(const std::string& path, lenses::Shot& out);
const Decoder& stb_decoder();
const Decoder& pixbuf_decoder();
const Decoder& pdf_decoder();
bool pdf_available();  // poppler installed (cheap: no load)
bool pdf_supported();  // loads poppler if needed; whether PDFs can render
const Decoder& tim_decoder();
const Decoder& video_decoder();
const Decoder& ilbm_decoder();
const Decoder& pcx_decoder();
const Decoder& xcf_decoder();
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
// Used by the raw decoder for embedded JPEG previews. `fit_w` x `fit_h`
// (either way round), when given: decoded at 1/2, 1/4 or 1/8 size if that
// still covers it. Its EXIF is read without the Camera and Image rows: the
// raw's own (from LibRaw) are the file's, and the preview's may not agree.
bool decode_jpeg_memory(const uint8_t* data, size_t size, Image& out, std::string& error, int fit_w = 0, int fit_h = 0);
// The samples as stored, `components` bytes a pixel: four-component data
// is left as it is (no CMYK or YCCK conversion; BLP1 keeps B, G, R, A
// there), three-component data is converted to RGB as usual.
bool decode_jpeg_samples(const uint8_t* data, size_t size, std::vector<uint8_t>& samples, int& width, int& height,
                         int& components, std::string& error);
#endif

#ifdef HAVE_JXL
// The samples as stored, `channels` a pixel, 16 bits each (as libjxl gives
// them: scaled to 65535 from `bits` bits; 0 for floats); on threads of its
// own when `threaded`.
bool decode_jxl_samples(const uint8_t* data, size_t size, std::vector<uint16_t>& samples, int& width, int& height, int& channels, int& bits,
                        std::string& error, bool threaded = true);
#endif

#ifdef HAVE_IPTC
void read_iptc_jpeg(const uint8_t* data, size_t size, Image& out);
void read_iptc_blob(const uint8_t* data, size_t size, Image& out);
#endif
// Embedded XMP packet plus any .xmp sidecar next to the file.
void read_xmp(const std::string& path, const uint8_t* data, size_t size, Image& out);
// The sidecar Lightroom reads beside `path`: photo.xmp for photo.CR3.
std::string xmp_sidecar_path(const std::string& path);
// Lightroom's develop settings (`edits`' exposure, white balance and
// controls, under its process version) into that sidecar: merged into one
// that's there, everything else in it kept (other schemas, the crs settings
// not applied here); written whole or not at all. The image file itself is
// never touched (Lightroom keeps a DNG's or a JPEG's settings inside it).
// Its profile (crs:CameraProfile) is the one the settings were read with,
// or the Adobe one their look maps to (look.h). Of the controls, only those
// moved from `rest` (where they are as the image opens untreated) are
// written, and those the sidecar has already.
bool write_xmp_sidecar(const std::string& path, const XmpDevelop& edits, const Adjustments& rest, std::string& error);

// An EXIF date ("2017:06:13 16:05:44") as the Image group shows a capture
// time, the way the raw decoder writes it: "2017-06-13 16:05:44".
inline std::string exif_date_text(std::string text)
{
    if (text.size() >= 10 && text[4] == ':' && text[7] == ':')
        text[4] = text[7] = '-';
    return text;
}

// How a photo was taken, as the Camera group lists it for every format: a
// raw's from LibRaw, anything else's from its EXIF, in the same rows with
// the same labels and formatting. Zero or empty: not known, so no row.
struct CameraRows
{
    std::string make, model;
    std::string firmware;  // a raw's maker note's, where it says one apart from EXIF's Software
    std::string software;  // EXIF's Software: whatever wrote the file (firmware or a program), under Image
    std::string lens, lens_make;
    double iso = 0;
    double shutter = 0;       // seconds
    double aperture = 0;      // f-number
    double focal_length = 0;  // mm, the lens's own
    double focal_35mm = 0;    // its 35 mm equivalent; shown alone without a focal length
    std::string picture_style;
    double bias = 0;  // exposure compensation, EV
    unsigned shot_number = 0;
};
// Adds the Camera group's rows, and the software to the Image group.
void add_camera_rows(const CameraRows& c, Image& out);
// The 35 mm equivalent of `focal` mm from the sensor's size, as EXIF gives
// it: its focal plane resolution (pixels per `unit`, EXIF's
// FocalPlaneResolutionUnit: 2 inch, 3 cm, 4 mm, 5 µm) for an image of
// `width` x `height` pixels. 0 when that can't be relied on: no absolute
// unit, photosites far from square, or a crop factor no camera has. Only
// for a raw: an editor that resizes or crops a JPEG rewrites its pixel
// dimensions but not the resolution, which is the sensor's.
double focal_35mm_from_plane(double focal, double x_res, double y_res, int unit, double width, double height);

#ifdef HAVE_EXIF
// From a TIFF-based raw's own Exif directory (a CR2's, a NEF's): its
// FocalLengthIn35mmFilm, or that worked out from its focal plane
// resolution; 0 when neither is there or reliable.
double tiff_focal_35mm(const uint8_t* data, size_t size, double focal);
// Parse an EXIF payload (a JPEG file, a bare "Exif\0\0..." blob, or a TIFF
// header) into the image's metadata groups. Returns the orientation, 1 when
// absent, and the camera model when known.
struct ExifSummary
{
    int orientation = 1;
    std::string camera;
    std::string color_space;  // "sRGB" / "Adobe RGB (1998)" when declared
    int64_t taken = 0;        // DateTimeOriginal in seconds, the camera's local time; 0 when absent
};
// `camera_rows` false, for a raw's embedded preview: everything but the
// Camera group and the Image group's rows, which the raw's own decode
// writes (its location, orientation and the rest of its EXIF group kept).
ExifSummary read_exif_blob(const uint8_t* data, size_t size, Image& out, bool camera_rows = true);
// The preview stored in an EXIF payload (usually a JPEG); empty if none.
std::vector<uint8_t> exif_thumbnail(const uint8_t* data, size_t size);
// A Canon CR3's EXIF, which it keeps as separate TIFF structures (its CMT1,
// CMT2 and CMT4 boxes): IFD0, the Exif IFD and the GPS IFD, any of them
// possibly empty. Only the orientation, camera, time taken and position
// are read, for the folder index.
ExifSummary read_cr3_exif(std::span<const uint8_t> ifd0, std::span<const uint8_t> exif, std::span<const uint8_t> gps, Image& out);
#endif

#ifdef HAVE_HEIF
// The primary image's EXIF (as a TIFF structure) and size, nothing decoded.
// False only if the file couldn't be read; one that isn't really a HEIF, or
// has no EXIF, is read fine and leaves `exif` empty.
bool heif_stored_exif(const std::string& path, std::vector<uint8_t>& exif, int& width, int& height);
#endif

const Decoder& exr_decoder();
const Decoder& svg_decoder();
