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

#include <rapport/gpu.h>
#include <rapport/widgets.h>

#include <memory>
#include <string>
#include <vector>

struct Image;

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
        rap::color background;
        bool checkerboard = false;  // under transparency
        int checker_size = 8;       // device pixels
        ScaleFilter shrink = ScaleFilter::Lanczos3, enlarge = ScaleFilter::Lanczos3;
        FrontBuffer front = FrontBuffer::Srgb8;
        float sdr_white_nits = 203;  // PQ and scRGB: what SDR white is
        bool histogram = false;      // the histogram panel, at `panel` (device pixels: x, y, width, height)
        int panel[4] = {};
    };

    // The frame, into a texture for canvas::present, with what it holds;
    // `generation` changes whenever the image's data does. False (with why)
    // when it can't, and the view draws on the CPU.
    bool render(const std::shared_ptr<Image>& image, uint64_t generation, const Frame& frame, rap::gpu::texture_ptr& out,
                rap::gpu::ticket& ready, rap::frame_color& colour, std::string& error);

    const std::shared_ptr<rap::gpu::device>& device() const { return device_; }

private:
    bool programs(std::string& error);
    // The image's data on the device, decoded to linear light (level 0).
    bool upload(const Image& image, std::string& error);
    // Level `n` (0 the image itself), made from the one above if it isn't yet.
    rap::gpu::texture_ptr level(int n);

    // The level the measures and the histogram count: at most 2048 pixels.
    rap::gpu::texture_ptr counted_level();

    std::shared_ptr<rap::gpu::device> device_;
    rap::gpu::texture_ptr blank_;  // for inputs a pass doesn't use
    rap::gpu::program_ptr decode_, resample_, compose_, stats_program_, peak_, histogram_program_, histogram_max_;
    rap::gpu::buffer_ptr stats_;      // the image's measures (view.hlsli's layout), for raw and HDR images
    bool measured_ = false;           // `stats_` is this image's
    rap::gpu::buffer_ptr histogram_;  // R, G, B, Y counts of what's shown, and the tallest
    double histogram_exposure_ = -1e9;  // what it was counted at (and the image's generation below)
    uint64_t histogram_generation_ = 0;

    std::weak_ptr<Image> image_;
    uint64_t generation_ = 0;
    std::vector<rap::gpu::texture_ptr> levels_;  // linear, premultiplied, half float
    rap::gpu::texture_ptr tone_;                  // a raw's camera-matched curve, 4096 x 1
    float scene_gain_ = 1;                        // the scene's scale before exposure (a raw's gain without a curve)
    bool tonable_ = false;                        // exposure applies
    rap::gpu::texture_ptr rows_;                  // the first pass's output, reused while big enough
    rap::gpu::texture_ptr frames_[2];
    int next_frame_ = 0;
};
