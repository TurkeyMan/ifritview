#pragma once

// Lightroom's develop controls (Camera Raw's crs: settings), as parameters:
// the edit stage applies them to the scene-linear image after its white
// balance, matrix and exposure, before anything the display does. Their
// effect is worked out here once, on the CPU, into two small tables
// (AdjustTables) that the GPU's edit stage (shaders/edit.hlsli) and the
// CPU's (apply_adjustments) both just look up:
//  - tone: what each tone becomes (contrast, highlights, shadows, whites,
//    blacks, the parametric and point curves; Process 2010's brightness,
//    contrast, recovery, fill light and blacks): a curve of linear light,
//    built on a perceptual axis (sRGB's encoding, carried on above white so
//    highlights over it can be brought back), applied to the largest and
//    smallest of a colour's components with the one between kept where it
//    was between them, so hue holds; and the red, green and blue point
//    curves, a channel each;
//  - colour: a hue x saturation map (as DNG's ProfileHueSatMap) of a hue
//    shift, a saturation scale and a value scale: vibrance, saturation and
//    the HSL panel.
// Both work in linear ProPhoto RGB, Lightroom's working space.
//
// Noise reduction and sharpening, the Detail panel, are in `detail`: they
// work on neighbourhoods, at the image's own resolution, before the rest
// (detail.h); the tables leave them out.
//
// Global versions of what in Lightroom is partly local: its Highlights,
// Shadows, Whites and Blacks adapt to the image's neighbourhoods (tone
// mapping that keeps local contrast); here each is one curve for the image.

#include "detail.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct Adjustments
{
    // crs:ProcessVersion: "6.7" (2012) and later share the controls below;
    // "5.7" (2010) and "5.0" (2003) have the legacy ones.
    bool legacy = false;

    // Basic, Process 2012: -100..100.
    double contrast = 0, highlights = 0, shadows = 0, whites = 0, blacks = 0;
    // Presence (every process): -100..100.
    double vibrance = 0, saturation = 0;
    // Process 2010: Brightness -150..150, Contrast -50..100, Recovery,
    // Fill Light and Blacks 0..100.
    double brightness = 0, contrast2010 = 0, recovery = 0, fill_light = 0, blacks2010 = 0;

    // The parametric curve: shadows, darks, lights, highlights (-100..100),
    // and the splits between them (percent).
    double parametric[4] = {};
    double splits[3] = {25, 50, 75};
    // The point curves, 0..255 both ways: the master and red, green, blue.
    // Empty or the two corners: linear.
    using Curve = std::vector<std::pair<int, int>>;
    Curve curve, red, green, blue;
    std::string curve_name;  // ToneCurveName(2012): "Linear", "Medium Contrast", "Strong Contrast", "Custom"

    // HSL: red, orange, yellow, green, aqua, blue, purple, magenta; -100..100.
    double hue[8] = {}, sat[8] = {}, lum[8] = {};

    // The Detail panel: noise reduction and sharpening.
    Detail detail;

    // Settings read but not applied yet (Clarity, Texture, Dehaze, colour
    // grading, calibration, detail, effects, the profile...): crs field
    // name to its value as written, kept so writing XMP loses nothing.
    std::map<std::string, std::string> kept;

    // Nothing for the tables to apply (the kept settings and the detail
    // aside).
    bool neutral() const;
    bool operator==(const Adjustments&) const = default;
};

// The settings by their crs: names, for reading and writing XMP: each
// field's value, and which process has it (2010, 2012; 0 both).
struct CrsField
{
    std::string name;
    double* value;
    int process;
    bool sign = true;  // written with a sign ("+15"), as the -100..100 ones are
    int decimals = 0;  // and with these (SharpenRadius: "+1.0")
};
std::vector<CrsField> crs_fields(Adjustments& a);
// The Detail panel's, among them.
std::vector<CrsField> detail_fields(Detail& d);
// The point curves by name: ToneCurvePV2012 (and Red, Green, Blue) for
// 2012, ToneCurve for 2010; and the name of the curve's preset.
struct CrsCurve
{
    const char* name;
    Adjustments::Curve* curve;
};
std::vector<CrsCurve> crs_curves(Adjustments& a);
const char* crs_curve_name(const Adjustments& a);  // ToneCurveName or ToneCurveName2012

// The tables an Adjustments makes. Built once a change (a few
// milliseconds), shared by whatever draws or saves with them.
struct AdjustTables
{
    // Tone: kTone samples of the curve at input g = kToneTop * i / (kTone - 1)
    // on sRGB's encoding (carried on above 1); each the linear output for
    // master, red, green, blue. Past the top, the scale at the top holds.
    static constexpr int kTone = 4096;
    static constexpr double kToneTopLinear = 16;  // four stops over white
    std::vector<float> tone;  // kTone x 4
    float tone_top = 0;       // the encoding of kToneTopLinear
    bool channels = false;    // the red, green, blue curves do something
    // Colour: kHues x kSats entries (hue across, from 0 at red; saturation
    // from 0 to 1 down) of hue shift (turns), saturation scale, value scale,
    // and 0; looked up bilinearly, hue wrapping.
    static constexpr int kHues = 72, kSats = 17;
    std::vector<float> colour;  // kHues x kSats x 4
    // Linear XYZ (D65, as the view has colours) to the working space and
    // back, and BT.709 to it and back (the CPU's linear data).
    float to_working[3][3], from_working[3][3];
    float from_709[3][3], to_709[3][3];
    uint64_t serial = 0;  // different for every table made: what caches key on
};

std::shared_ptr<const AdjustTables> build_tables(const Adjustments& a);

// The CPU's edit stage: a linear BT.709 colour through the tables, as
// shaders/edit.hlsli takes one.
void apply_adjustments(const AdjustTables& t, float rgb[3]);
