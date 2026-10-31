#pragma once

// Noise reduction and sharpening, Lightroom's Detail panel: what the develop
// does to a pixel's neighbourhood rather than the pixel alone, so it works at
// the image's own resolution, on its scene-linear data (white balanced as
// shot), before every per-pixel control (adjust.h). The view does it on the
// GPU (shaders/detail.hlsl, GpuView: the level-0 region shown, and the full
// image once on the way to the smaller levels); saving and the command line
// on the CPU (apply_detail), the same filter step for step.
//
// Noise reduction is a wavelet shrinkage: the image in a variance-
// stabilised encoding (each channel's square root: shot noise then has
// about the same size at every brightness), split into luminance and two
// colour differences, then into the details at five scales (an "a trous"
// B3 spline, 1, 2, 4, 8 and 16 pixels apart), each detail shrunk towards 0
// by a multiple of the noise at that scale, and put back together. The
// noise is measured from the image itself: the finest scale's median
// absolute detail. Luminance is shrunk gently (its Detail spares the finest
// scales, its Contrast the middle ones); colour harder and across coarser
// scales (its Detail spares the finest, its Smoothness reaches the coarsest),
// as a colour difference vector, so hues aren't bent.
//
// Sharpening is an unsharp mask of the luminance (after the noise is
// taken out, so it isn't sharpened): the detail a Gaussian of `radius`
// leaves, its large swings tempered (Detail: how much of an edge's swing
// gets through, so how much halo), kept off smooth areas (Masking).

struct Detail
{
    double sharpness = 0;           // Sharpness, 0..150: how much
    double sharpen_radius = 1;      // SharpenRadius, 0.5..3: pixels
    double sharpen_detail = 25;     // SharpenDetail, 0..100: halos let through
    double sharpen_masking = 0;     // SharpenEdgeMasking, 0..100: smooth areas spared
    double luminance = 0;           // LuminanceSmoothing, 0..100
    double luminance_detail = 50;   // LuminanceNoiseReductionDetail, 0..100
    double luminance_contrast = 0;  // LuminanceNoiseReductionContrast, 0..100
    double colour = 0;              // ColorNoiseReduction, 0..100
    double colour_detail = 50;      // ColorNoiseReductionDetail, 0..100
    double colour_smoothness = 50;  // ColorNoiseReductionSmoothness, 0..100

    // A raw's with nothing set, as Lightroom has it: Sharpness 40, Color 25.
    static Detail raw_defaults();
    bool active() const { return sharpness > 0 || luminance > 0 || colour > 0; }
    bool operator==(const Detail&) const = default;
};

// What the filter makes of the settings: the shader's parameters, and the
// CPU's.
struct DetailFilter
{
    static constexpr int kLevels = 5;  // the wavelet's scales
    // How far a result reaches for its inputs, in pixels: 2 x (1 + 2 + 4 +
    // 8 + 16) for the wavelet, 9 for the widest sharpening Gaussian, 1 for
    // the mask's gradient, and some to spare. A region filtered with this
    // much around it is what the whole image filtered would have there.
    static constexpr int kReach = 80;
    static constexpr float kFloor = 0.002f;  // the encoding: sqrt(v + kFloor)

    explicit DetailFilter(const Detail& d);
    bool denoise = false, sharpen = false;
    // Each scale's shrinkage threshold, a multiple of the finest scale's
    // noise (measured per image): luminance, colour.
    float luminance[kLevels] = {}, colour[kLevels] = {};
    float amount = 0;     // of the luminance detail added back
    float sigma = 1;      // the sharpening Gaussian's, in pixels
    float halo = 0;       // a detail swing of this much gets through half
    float mask_low = 0, mask_high = 0;  // the luminance gradient the mask opens over; 0: no mask
};

// The CPU's: linear RGB (3 floats a pixel, rows of `width`, white 1) through
// the filter into `out`, of the same layout.
void apply_detail(const DetailFilter& f, const float* rgb, int width, int height, float* out);
