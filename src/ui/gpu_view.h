#pragma once

// The view drawn on the GPU (docs/gpu-pipeline.md). The image's own data
// (16-bit, half or float samples, a raw's linear data; 8-bit pixels only for
// 8-bit images) goes to the canvas's device once and is decoded to linear
// light; the levels below it are made as zooming out needs them; and each
// frame is resampled from the right one with the filter the settings choose,
// exposed and tone-mapped, composited over the checkerboard or the
// background, and encoded for the front buffer, which says what it holds.
// Raw and HDR images are measured on the device when they arrive (their
// peak, and how far from white they go in each hue), so the display
// mapping compresses only what's there; and a classic histogram of what's
// shown is counted there and drawn into the frame. None of it comes back
// to the CPU.

#include "../config.h"
#include "../pixel_buffer.h"

#include <rapport/gpu.h>
#include <rapport/widgets.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct Image;
struct RawMosaic;

// What the front buffer holds.
enum class FrontBuffer
{
    Srgb8,    // sRGB, 8 bits a channel, dithered
    Srgb10,   // sRGB, 10 bits (rgb10a2)
    Pq10,     // HDR10: PQ, 10 bits (BT.2020 once the gamut is mapped to it)
    ScRgb16,  // linear scRGB, half float (HDR, or a 12-bit panel)
};

class GpuView
{
public:
    explicit GpuView(std::shared_ptr<rap::gpu::device> device);

    struct Frame
    {
        int width = 0, height = 0;              // the canvas's frame target, device pixels
        double zoom = 1, pan_x = 0, pan_y = 0;  // device pixels per image pixel; the image's top left
        double exposure = 0;                    // EV, for images with more than the display shows
        float white_balance[3] = {};            // a raw's R, G, B multipliers (the least 1); 0: as shot
        rap::color background;
        bool checkerboard = false;  // under transparency
        int checker_size = 8;       // device pixels
        ScaleFilter shrink = ScaleFilter::Lanczos3, enlarge = ScaleFilter::Lanczos3;
        FrontBuffer front = FrontBuffer::Srgb8;
        float sdr_white_nits = 203;  // PQ and scRGB: what SDR white is
        bool count_histogram = false;  // count what's shown, for the overlay or the docked panel
        bool histogram = false;        // draw it into the frame, at `panel` (device pixels: x, y, width, height)
        int histogram_channels = 15;   // which: bits 1 R, 2 G, 4 B, 8 luminance
        int panel[4] = {};
    };

    // The frame, into a texture for canvas::present, with what it holds;
    // `generation` changes whenever the image's data does. False (with why)
    // when it can't, and the view draws on the CPU.
    bool render(const std::shared_ptr<Image>& image, uint64_t generation, const Frame& frame, rap::gpu::texture_ptr& out,
                rap::gpu::ticket& ready, rap::frame_color& colour, std::string& error);

    const std::shared_ptr<rap::gpu::device>& device() const { return device_; }

    // The histogram counted by the last render() (count_histogram), drawn
    // into a `width` x `height` texture for the docked panel's canvas. False
    // while there's nothing counted.
    bool draw_histogram(int width, int height, int channels, rap::gpu::texture_ptr& out, rap::gpu::ticket& ready);

    // The numbers the docked panel shows, read back (a few kilobytes) after
    // the last count: luminance of the scene as exposed (1 = SDR white), and
    // how much of what's shown is clipped.
    struct Details
    {
        bool known = true;   // false: nothing measured (the image isn't drawn on the GPU)
        bool scene = false;  // raw or HDR: luminance means something past white
        float minimum = 0, maximum = 0, peak = 0, average = 0;
        float black = 0, white = 0;  // fractions of pixels shown at 0 and at 255 (any channel)
    };
    // `done` runs on a rapport thread once they're in (if a count has run).
    void read_details(std::function<void(const Details&)> done);

    // The image at `exposure` as an 8-bit sRGB rendering, full size, read back
    // (premultiplied BGRA, as Image keeps pixels): for what reads pixels on
    // the CPU (saving, printing, comparing) when the GPU developed it. Waits.
    bool export_pixels(const std::shared_ptr<Image>& image, uint64_t generation, double exposure, PixelBuffer& out,
                       std::string& error);

private:
    bool programs(std::string& error);
    // The image's data on the device, decoded to linear light (level 0).
    bool upload(const Image& image, std::string& error);
    // A raw's readout still compressed (RawMosaic::stream) decoded here: its
    // active area into `sites` (a photosite a texel), and what's masked
    // summed into `levels` for the develop's black level.
    bool decode_stream(const RawMosaic& m, rap::gpu::texture_ptr& sites, rap::gpu::buffer_ptr& levels, std::string& error);
    // Level `n` (0 the image itself), made from the one above if it isn't yet.
    rap::gpu::texture_ptr level(int n);

    // The level the measures and the histogram count: at most 2048 pixels.
    rap::gpu::texture_ptr counted_level();

    // Images left behind stay on the device (level 0, the levels made, the
    // measures), most recent first, so flipping back to one is a redraw:
    // up to about half the memory the GPU says the app has.
    struct Retained
    {
        std::weak_ptr<Image> image;
        uint64_t generation = 0;
        std::vector<rap::gpu::texture_ptr> levels;
        rap::gpu::texture_ptr tone;
        rap::gpu::buffer_ptr stats;
        float scene_gain = 1, input_to_xyz[3][3] = {}, as_shot[3] = {1, 1, 1};
        bool auto_exposure = false, tonable = false, measured = false;
        size_t bytes = 0;
    };
    std::vector<Retained> retained_;
    std::shared_ptr<Image> pending_image_;  // what render() is moving to, while it retains the last
    uint64_t pending_generation_ = 0;
    void retain_current();                                       // the current image's, into retained_
    bool restore(const std::shared_ptr<Image>& image, uint64_t generation);  // from retained_, if it's there
    size_t current_bytes() const;

    std::shared_ptr<rap::gpu::device> device_;
    rap::gpu::texture_ptr blank_;  // for inputs a pass doesn't use
    rap::gpu::program_ptr decode_, resample_, compose_, stats_program_, peak_, histogram_program_, histogram_max_, histogram_view_;
    rap::gpu::texture_ptr panels_[2];  // the docked histogram, drawn
    int next_panel_ = 0;
    rap::gpu::buffer_ptr stats_;      // the image's measures (view.hlsli's layout), for raw and HDR images
    bool measured_ = false;           // `stats_` is this image's
    rap::gpu::buffer_ptr histogram_;  // R, G, B, Y counts of what's shown, and the tallest
    double histogram_exposure_ = -1e9;  // what it was counted at (and the image's generation below)
    uint64_t histogram_generation_ = 0;
    float histogram_scale_ = 1;  // the scene's scale it was counted at

    std::weak_ptr<Image> image_;
    uint64_t generation_ = 0;
    std::vector<rap::gpu::texture_ptr> levels_;  // linear, premultiplied, half float
    rap::gpu::texture_ptr tone_;                  // a raw's camera-matched curve, 4096 x 1
    float scene_gain_ = 1;                        // the scene's scale before exposure (a raw's gain without a curve)
    float input_to_xyz_[3][3] = {};               // the image's data -> XYZ (a raw's: its colour matrix, before white balance)
    float as_shot_[3] = {1, 1, 1};                // a raw's: what its level 0 was developed with, the least 1
    bool auto_exposure_ = false;                  // a raw developed here: scaled by its own 99th percentile
    rap::gpu::program_ptr develop_;
    rap::gpu::program_ptr mosaic_, ljpeg_guess_, ljpeg_sync_, ljpeg_resolve_, ljpeg_decode_, ljpeg_columns_, ljpeg_rows_, ljpeg_black_;
    bool tonable_ = false;                        // exposure applies
    rap::gpu::texture_ptr rows_;                  // the first pass's output, reused while big enough
    rap::gpu::texture_ptr frames_[2];
    int next_frame_ = 0;
};
