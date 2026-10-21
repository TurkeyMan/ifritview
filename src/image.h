#pragma once

#include "pixel_format.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Linear scene data kept alongside a developed raw image so exposure can be
// changed without demosaicing again.
struct LinearData
{
    std::vector<uint16_t> rgb;  // 16-bit linear RGB, 3 per pixel, white balanced, sRGB primaries
    double auto_gain = 1.0;     // gain that clips 1% of samples: the "as the camera meant it" zero point
    // Tone curve matched to the camera's own rendering (its embedded JPEG):
    // display-linear output for scene-linear input i/4095. Empty = none.
    std::vector<float> tone;
};

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
        std::vector<uint8_t> pixels;
        int delay_ms;
    };
    std::vector<Frame> frames;
    int loops = 0;  // 0 = forever
};

// A decoded image in cairo's ARGB32 layout: one native-endian uint32 per
// pixel, alpha premultiplied, so the buffer can be wrapped by a cairo image
// surface without a copy.
struct Image
{
    int width = 0;
    int height = 0;
    int stride = 0;  // bytes per row, always width * 4
    std::vector<uint8_t> pixels;
    bool has_alpha = false;
    int bits_per_sample = 8;               // depth of the source file, for the status bar
    std::string format;                    // "JPEG", "Canon RAW", ...
    PixelFormat pixel_format;              // how the file stores its pixels
    std::string color_space;               // declared colour space ("sRGB", "Adobe RGB (1998)", "BT.2020 PQ"); empty when the file says nothing
    std::string info;                      // extra detail for the status bar, may be empty
    std::shared_ptr<LinearData> linear;    // only for raw files
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
    bool has_location = false;            // GPS position from EXIF, decimal degrees
    double latitude = 0, longitude = 0;
    std::string place;  // reverse-geocoded name, filled in asynchronously

    std::string location_text() const;  // "48.8584° N, 2.2945° E"

    void add(const std::string& group, std::string key, std::string value);

    // Re-render pixels from `linear` with an exposure offset in stops
    // around the automatic gain. No-op without linear data.
    void retone(double ev);
    // Same rendering into a fresh buffer, safe to run off the main thread while
    // `pixels` is being displayed; the caller swaps it in.
    std::vector<uint8_t> retone_buffer(double ev) const;

    void allocate(int w, int h);
    uint32_t* row(int y) { return reinterpret_cast<uint32_t*>(pixels.data() + size_t(y) * stride); }
    const uint32_t* row(int y) const { return reinterpret_cast<const uint32_t*>(pixels.data() + size_t(y) * stride); }

    // Converters from common decoder outputs into the ARGB32 buffer. The
    // image must already be allocated to the source dimensions. src_stride is
    // in bytes. The source may alias the image's own buffer when it has four
    // bytes per pixel.
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
