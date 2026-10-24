#pragma once

#include "pixel_buffer.h"
#include "metadata/file_metadata.h"
#include "pixel_format.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Linear scene data kept alongside a developed raw image so exposure can be
// changed without demosaicing again.
struct LinearData
{
    LinearBuffer rgb;        // 16-bit linear RGB, 3 per pixel, white balanced, sRGB primaries
    double auto_gain = 1.0;  // gain that clips 1% of samples: the "as the camera meant it" zero point
    // Tone curve matched to the camera's own rendering (its embedded JPEG):
    // display-linear output for scene-linear input i/4095. Empty = none.
    std::vector<float> tone;
};

// The source's own samples where it has more than 8 bits a channel, kept as
// the file has them (sample type and channels, bit for bit) beside the 8-bit
// pixels that are drawn: 16-bit PNG, TIFF and PSD, signed 16-bit, half and
// float formats, HDR. Not premultiplied. Linear ones (the floats) that go
// past 1 have more than the display shows, and take an exposure like a
// raw's linear data.
struct SourceData
{
    enum class Sample
    {
        U16,  // unsigned, 0..65535 for 0..1
        I16,  // signed, -32767..32767 for -1..1
        F16,  // IEEE half (half.h)
        F32,  // IEEE float
    };
    SourceData(Sample sample, int channels, int width, int height, bool linear);
    Sample sample;
    int channels;  // 1 grey, 2 grey + alpha, 3 RGB, 4 RGBA
    int width, height;
    PixelBuffer data;  // rows of width × channels samples, packed, native byte order
    bool linear;       // scene-linear light; else display-encoded, as the 8-bit pixels are
    float peak = 1;    // the largest colour value (linear sources)

    int sample_bytes() const { return sample == Sample::F32 ? 4 : 2; }
    size_t pixel_bytes() const { return size_t(sample_bytes()) * channels; }
    uint8_t* pixel(int x, int y) { return data.data() + (size_t(y) * width + x) * pixel_bytes(); }
    const uint8_t* pixel(int x, int y) const { return data.data() + (size_t(y) * width + x) * pixel_bytes(); }
    // Channel c of a pixel as a float: integers normalised (0..1, signed -1..1).
    float value(const uint8_t* px, int c) const;
    bool has_alpha() const { return channels == 2 || channels == 4; }
    void measure_peak();
};

// Orientations (EXIF 1..8) as turns and flips: `after` done to an image
// already turned by `before`; and where a rectangle of a W × H image lands.
int compose_orientation(int before, int after);
void orient_rect(int orientation, int W, int H, int& x, int& y, int& w, int& h);

// Edits found in XMP (embedded or sidecar) that the viewer can honour.
struct XmpDevelop
{
    bool present = false;
    double exposure = 0;     // crs:Exposure2012, EV on top of as-shot
    double temperature = 0;  // crs:Temperature in Kelvin, 0 = as shot (parsed, not applied yet)
    int orientation = 1;     // tiff:Orientation
    bool has_crop = false;
    double crop_left = 0, crop_top = 0, crop_right = 1, crop_bottom = 1;  // fractions of the un-rotated image
};

// Key/value metadata, grouped for display: "EXIF", "GPS", "Camera", "PNG"...
struct MetadataItem
{
    std::string key;
    std::string value;
};
struct MetadataGroup
{
    std::string name;
    std::vector<MetadataItem> items;
};

// Frames of an animated image, each a full composed canvas in the same
// ARGB32 layout as Image::pixels, with its display time.
struct Animation
{
    struct Frame
    {
        PixelBuffer pixels;
        int delay_ms;
    };
    std::vector<Frame> frames;
};

// A decoded image in cairo's ARGB32 layout: one native-endian uint32 per
// pixel, alpha premultiplied, so the buffer can be wrapped by a cairo image
// surface without a copy.
struct Image
{
    int width = 0;
    int height = 0;
    int stride = 0;  // bytes per row, always width * 4
    PixelBuffer pixels;
    bool has_alpha = false;
    int bits_per_sample = 8;             // depth of the source file, for the status bar
    std::string format;                  // "JPEG", "Canon RAW", ...
    PixelFormat pixel_format;            // how the file stores its pixels
    std::string color_space;             // declared colour space ("sRGB", "Adobe RGB (1998)", "BT.2020 PQ"); empty when the file says nothing
    std::string info;                    // extra detail for the status bar, may be empty
    std::shared_ptr<LinearData> linear;  // only for raw files
    std::shared_ptr<SourceData> source;  // more than 8 bits a channel; may be null
    // More than the display shows (a linear source past 1): exposure applies.
    bool hdr() const { return source && source->linear && source->peak > 1.0f; }
    // Re-rendered by retone(): a raw's linear data, or an HDR source.
    bool tonable() const { return linear || hdr(); }
    // Halvings of `pixels` (½, ¼, …) for fast downscaled display, built off the
    // main thread after a decode. Empty when absent or stale; anything that
    // rewrites `pixels` clears them.
    struct Mip
    {
        int width, height;
        PixelBuffer pixels;  // ARGB32, stride = width * 4
    };
    std::vector<Mip> mips;
    void build_mips();
    std::shared_ptr<Animation> animation;  // frames when the file animates; pixels hold the first
    double exposure_bias = 0;              // the camera's exposure compensation, in EV, already baked in
    // One Image is one file. A raw starts life as its embedded JPEG (a
    // Preview) and becomes Final when the develop is adopted in place; the
    // object, and so everyone holding it, keeps its identity throughout.
    enum class Stage
    {
        Final,
        Preview
    };
    Stage stage = Stage::Final;
    bool is_preview() const { return stage == Stage::Preview; }
    bool is_raw = false;
    bool edited = false;  // cropped or turned since it was read: a stored thumbnail no longer matches
    // How the pixels relate to the file's: a turn or flip of the decoded
    // image (as an EXIF orientation, 1..8), then a crop of that. While only
    // these have been done, a JPEG can be saved losslessly by redoing them on
    // its compressed data; anything else that changes the pixels sets
    // `transform.broken` (and says what).
    struct Transform
    {
        int width = 0, height = 0;  // the decoded image, before any of it (0 until the first)
        int orientation = 1;
        int x = 0, y = 0, w = 0, h = 0;  // the crop, in the turned image
        std::string broken;
    } transform;
    void break_transform(std::string why)
    {
        if (transform.broken.empty())
            transform.broken = std::move(why);
    }
    bool is_video = false;
    bool has_audio = false;  // videos: an audio stream exists  // the pixels are a frame (the first, until the player takes over)
    double duration = 0;     // seconds, for videos  // a camera raw (its preview is the embedded JPEG; it has an exposure control)
    // While a preview: the size the final pixels will have, so the window and
    // zoom are chosen for that. 0 = unknown.
    int final_width = 0, final_height = 0;
    // Edits made while a preview, replayed on the final pixels on adoption.
    struct PendingEdit
    {
        int orientation;    // != 1: rotate/mirror
        double x, y, w, h;  // == 1: crop, as fractions of the image
    };
    std::vector<PendingEdit> pending_edits;
    double exposure_edit = 0;  // EV chosen by the user or XMP, relative to the camera's bias

    // Take the final pixels of this same file: pending edits are replayed on
    // them, then they replace ours in place (metadata merged, identity kept).
    void adopt(Image&& final);
    XmpDevelop xmp;                       // edits read from XMP, applied by the app if enabled
    std::vector<MetadataGroup> metadata;  // whatever the decoder could dig out
    std::shared_ptr<const FileMetadata> stored;  // the metadata as the file holds it, for saving; may be null
    bool has_location = false;            // GPS position from EXIF, decimal degrees
    double latitude = 0, longitude = 0;
    std::string place;  // reverse-geocoded name, filled in asynchronously

    std::string location_text() const;  // "48.8584° N, 2.2945° E"

    void add(const std::string& group, std::string key, std::string value);

    // Re-render pixels from `linear` with an exposure offset in stops
    // around the automatic gain, or from an HDR source (0 EV: its values as
    // they are, 1 being white). No-op for anything else.
    void retone(double ev);
    // Keeps `src` as the image's source and renders the 8-bit pixels from it
    // (allocated to its size): linear ones at 0 EV, the rest as they're
    // encoded. Sets has_alpha.
    void set_source(std::shared_ptr<SourceData> src);
    // Same rendering into a fresh buffer, safe to run off the main thread while
    // `pixels` is being displayed; the caller swaps it in.
    PixelBuffer retone_buffer(double ev) const;
    // 8-bit pixels from `source` (linear ones at exposure `ev`).
    PixelBuffer render_source(double ev) const;

    void allocate(int w, int h);
    uint32_t* row(int y) { return reinterpret_cast<uint32_t*>(pixels.data() + size_t(y) * stride); }
    const uint32_t* row(int y) const { return reinterpret_cast<const uint32_t*>(pixels.data() + size_t(y) * stride); }

    // Converters from common decoder outputs into the ARGB32 buffer. The
    // image must already be allocated to the source dimensions. src_stride is
    // in bytes. The source may alias the image's own buffer when it has four
    // bytes per pixel. The 16-bit ones keep the samples as `source` too.
    void from_rgb8(const uint8_t* src, int src_stride);
    void from_bgrx8(const uint8_t* src, int src_stride);
    void from_rgba8(const uint8_t* src, int src_stride, bool premultiplied = false);
    void from_bgra8(const uint8_t* src, int src_stride, bool premultiplied = false);
    void from_rgb16(const uint16_t* src, int src_stride);
    void from_rgba16(const uint16_t* src, int src_stride);

    // A copy of the given rectangle (image coordinates, clamped), keeping
    // linear data so a cropped raw still takes exposure changes.
    Image crop(int x, int y, int w, int h) const;

    // Rotate/flip in place per an EXIF orientation value 1..8.
    void apply_orientation(int exif_orientation);
};
