#pragma once

// A raw's look: the rendering its develop controls apply on top of, as a
// camera profile is in Lightroom. With every control at rest a raw shows
// its look; Lightroom's controls (adjust.h) then work relative to it, and
// changing the look keeps them as they are. A look is never turned into
// control values.
//
//  - Neutral: the scene as the develop's models make it, nothing added (the
//    identity; Lightroom's Adobe Neutral).
//  - Camera-like: a camera's JPEG, as fitted to camera JPEGs against our
//    neutral develop (D:\tmp\develop-audit\look\work\final.json, fit4.txt):
//    a contrast S-curve on the cube root of linear light, and in Oklab a
//    chroma gain with a vibrance term (the least saturated gaining most)
//    and a gain and a hue turn per hue sector (blues and cyans held back,
//    foliage raised).
//  - Enhanced: the same model, stronger (look.cpp says by how much).
//  - Monochrome: the camera-like curve, chroma 0.
//
// Where it runs: in the edit stage, after the scene's models (the sensor,
// white balance, exposure) and before the controls. It's built into the
// edit stage's tables with them (build_tables), so the GPU and the CPU
// apply it alike and no shader knows of it:
//  - its curve is the first on the tone axis, under the controls' curves;
//    like theirs it's applied to a colour's largest and smallest components,
//    hue kept, where the fit had it per channel (which adds a little chroma
//    in the midtones that this leaves to the chroma gain);
//  - its chroma gain and hue turns go into the colour map with the
//    controls' (a product of their scales, a sum of their turns): each cell
//    of the map (a hue and saturation of the working space) is worked out
//    for its colour at a reference brightness (kLookReference), in Oklab as
//    fitted. The map comes before the tone curves, so the look's colour is
//    indexed by the colour as it comes in, where the fit measured it after
//    its curve.
//
// The toe's chroma fade near black (shaders/view.hlsli, edit():
// shadow_chroma_fade) is a style parameter of the view's, outside the look;
// the fit found it accounts for much of the shadows' difference from the
// camera's JPEG. A look may come to set it.

#include <optional>
#include <string>
#include <string_view>

enum class Look
{
    Neutral,
    CameraLike,
    Enhanced,
    Monochrome,
};
constexpr int kLookCount = 4;

struct LookParams
{
    // Contrast: an S-curve on the cube root of linear light (0..1), through
    // `pivot` there: below it pivot * (u / pivot)^contrast, above it
    // 1 - (1 - pivot) * ((1 - u) / (1 - pivot))^contrast; 1 is none.
    // Above white the light is left as it is.
    double contrast = 1;
    double pivot = 0.5;
    // Chroma, Oklab's: C' = C * chroma_gain * (1 + vibrance * exp(-C / vibrance_width));
    // then each hue sector's gain and turn (degrees), 12 sectors of 30
    // degrees centred on 15, 45..., interpolated between their centres.
    double chroma_gain = 1;
    double vibrance = 0;
    double vibrance_width = 0.04;
    double hue_gain[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    double hue_shift[12] = {};

    bool identity() const;
};

// The brightness (linear, of the working space's largest component) at
// which a cell of the colour map is worked out.
constexpr double kLookReference = 0.25;

LookParams look_params(Look);
// The curve: a value on the controls' perceptual axis (sRGB's encoding,
// adjust.cpp) to the look's.
double look_tone(const LookParams&, double g);
// The colour: a linear BT.709 colour turned in hue as the look has it (in
// place); its chroma's scale returned.
double look_colour(const LookParams&, double rgb[3]);

// As the settings file names them ("neutral", "camera", "enhanced",
// "monochrome"), and back.
const char* look_id(Look);
std::optional<Look> look_named(std::string_view id);
// Its name in the interface (marked N_, translated where it's shown).
const char* look_label(Look);

// Lightroom's crs:CameraProfile names: the look one renders as (Adobe's
// and the camera matching profiles by their style; anything else unknown,
// custom or third-party, as Camera-like; a monochrome one as Monochrome).
// An empty name, a treated raw without a profile, is Adobe's default:
// Camera-like.
Look look_of_profile(std::string_view profile);
// One of the names the table above lists itself (else it's shown as it's
// named, rendered as what it maps to).
bool known_profile(std::string_view profile);
// The Adobe profile a look is written as: "Adobe Neutral", "Adobe Color",
// "Adobe Vivid", "Adobe Monochrome".
const char* adobe_profile(Look);

// A camera's picture style (Canon's Picture Style, Nikon's Picture Control,
// Fujifilm's film simulation, Sony's Creative Style) by its name: the look
// it maps to. Standard, Portrait, Auto and anything unknown are
// Camera-like.
Look look_of_picture_style(std::string_view style);

// --check-looks DIR: the profile mapping, reading and writing it in a
// sidecar (an unknown name kept), what counts as treated, and Save writing
// only the controls that changed. 0 when it all holds.
int check_looks(const std::string& dir);
