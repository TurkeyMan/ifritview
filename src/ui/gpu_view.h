#pragma once

// The view drawn on the GPU (docs/gpu-pipeline.md). The image's own data
// (16-bit, half or float samples, a raw's linear data; 8-bit pixels only for
// 8-bit images) goes to the canvas's device once and is decoded to linear
// light; the levels below it are made as zooming out needs them; and each
// frame is resampled from the right one with the filter the settings choose,
// exposed and tone-mapped, composited over the checkerboard or the
// background, and encoded for the front buffer, which says what it holds.

#include "../config.h"

#include <rapport/gpu.h>
#include <rapport/widgets.h>

#include <atomic>
#include <functional>
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
    // `changed` runs (on any thread) when something measured in the
    // background changes how the frame looks: draw again.
    GpuView(std::shared_ptr<rap::gpu::device> device, std::function<void()> changed);

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
    };

    // The frame, into a texture for canvas::present, with what it holds;
    // `generation` changes whenever the image's data does. False (with why)
    // when it can't, and the view draws on the CPU.
    bool render(const std::shared_ptr<Image>& image, uint64_t generation, const Frame& frame, rap::gpu::texture_ptr& out,
                rap::gpu::ticket& ready, rap::frame_color& colour, std::string& error);

    const std::shared_ptr<rap::gpu::device>& device() const { return device_; }
    // The image's measures are in (the frame looks as it finally will).
    bool measured() const { return measuring_ && measuring_->done; }

    // What the image holds, measured once it's on the device (raw and HDR
    // images): how bright it goes, and how far from white in each hue
    // direction (1 = the display's edge), so the display mapping only
    // compresses what's really there.
    struct Measures
    {
        float peak = 0;         // luminance, 1 = SDR white: the 99.9th percentile (a few hot pixels don't count)
        float extents[72] = {}; // per 5 degrees of hue round the white point
        bool outside = false;   // anything past the display's gamut
    };

private:
    bool programs(std::string& error);
    // The image's data on the device, decoded to linear light (level 0).
    bool upload(const Image& image, std::string& error);
    // Level `n` (0 the image itself), made from the one above if it isn't yet.
    rap::gpu::texture_ptr level(int n);

    // Measures the image from a level of at most 2048 pixels, on the
    // device's thread; `measures_` is filled when it's done.
    void measure();

    std::shared_ptr<rap::gpu::device> device_;
    std::function<void()> changed_;
    struct Measuring
    {
        std::atomic<bool> done{false};
        Measures measures;
    };
    std::shared_ptr<Measuring> measuring_;
    rap::gpu::texture_ptr extents_;  // measures_.extents, once there
    rap::gpu::texture_ptr blank_;    // for inputs a pass doesn't use
    rap::gpu::program_ptr decode_, resample_, compose_;

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
