#pragma once
// Lens corrections from lensfun's database (lensfun.github.io; its data is
// CC BY-SA 3.0) for raws that carry none of their own: the lens's
// distortion, its lateral (transverse) chromatic aberration and its
// vignetting, as lensfun models them, for the develop to apply on the GPU
// (the same words a DNG's own corrections fill: dng_opcodes.h).
//
// Only the database is used, not the library: its XML read here, and the
// models (distortion poly3, poly5, ptlens; TCA linear, poly3; vignetting
// pa) worked as lensfun's modifier works them: coordinates in units of the
// lens's real focal length, the calibration's crop factor and aspect ratio
// against the camera's, distortion and TCA interpolated between calibrated
// focal lengths with lensfun's splines, vignetting between focal length,
// aperture and distance by its inverse distance weighting, and the image
// scaled so the corrected frame has no empty edges (its autoscale).
//
// The lens is matched conservatively: the camera by make and model (or by a
// model name no other camera has, where the maker's name changed), for its
// mount and crop factor; then a lens on that mount (or one it takes) whose
// name has exactly the words of the one the camera recorded, give or take
// the maker's name, focal lengths and aperture alike; or, for a camera with
// a lens of its own (lensfun names its mount in lower case), that lens. A
// wrong match is worse than none, so anything less, or two lenses that fit,
// matches nothing.

#include "decoders/dng_opcodes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lenses
{

// How a picture was taken, as its raw says.
struct Shot
{
    std::string make, model;  // the camera's (LibRaw's normalised ones)
    std::string lens;         // the lens's name, as the camera recorded it
    float focal = 0;          // mm
    float aperture = 0;       // f-number
    float distance = 0;       // focus distance, m; 0: unknown (taken as far)
    float lens_min_focal = 0, lens_max_focal = 0;  // the lens's range, if the maker notes say; 0: unknown
    int frame[4] = {};  // the camera's framing in the active area (x, y, width, height): the image lensfun corrects
    bool empty() const { return lens.empty() || model.empty(); }
};

struct Camera
{
    std::vector<std::string> makers, models;  // every language's
    std::string mount;
    float crop = 0;
};

struct Lens
{
    std::vector<std::string> makers, models;
    std::vector<std::string> mounts;
    float min_focal = 0, max_focal = 0, min_aperture = 0, max_aperture = 0;
    float crop = 1, aspect = 1.5f;      // the calibration's sensor
    float centre_x = 0, centre_y = 0;   // the optical centre's offset, in half the image's shorter side
    struct Distortion
    {
        int model;  // 1 poly3 (k1), 2 poly5 (k1, k2), 3 ptlens (a, b, c)
        float focal, real_focal;
        float terms[3];
    };
    struct Tca
    {
        int model;  // 1 linear (kr, kb), 2 poly3 (vr, vb, cr, cb, br, bb)
        float focal;
        float terms[6];
    };
    struct Vignetting  // pa: k1, k2, k3
    {
        float focal, aperture, distance;
        float terms[3];
    };
    std::vector<Distortion> distortion;
    std::vector<Tca> tca;
    std::vector<Vignetting> vignetting;
    std::vector<std::string> words;  // the first model name's, as matching splits them
};

struct Mount
{
    std::string name;
    std::vector<std::string> compat;
};

class Database
{
public:
    // One of the database's XML files added; false (with why) if it isn't
    // one. What it held before an error is kept.
    bool add_xml(const char* data, size_t size, std::string& error);
    bool empty() const { return lenses_.empty(); }
    size_t lens_count() const { return lenses_.size(); }
    size_t camera_count() const { return cameras_.size(); }

    const Camera* camera(const Shot& shot) const;
    // The lens `shot` was taken with, or null, with why in `why`.
    const Lens* lens(const Camera& camera, const Shot& shot, std::string& why) const;

private:
    friend struct Parser;
    std::vector<Camera> cameras_;
    std::vector<Lens> lenses_;
    std::vector<Mount> mounts_;
};

// The corrections for a raw's develop, as dng_opcodes.h lays them out
// (lensfun's models in place of the DNG's: warp model 1, vignette model 2)
// for the shot's framing, in the active area's coordinates, with what they
// are ("distortion, TCA, vignetting"). Empty when the lens's calibration
// has none of them for the shot.
dng::Corrections corrections(const Lens& lens, const Camera& camera, const Shot& shot);

// A lens name split into words as lensfun's matching splits them: by
// character class, case folded, ".0" dropped from numbers, a lone "f" and
// lone punctuation dropped (but "+" and "*" kept), sorted.
std::vector<std::string> words(const std::string& name);

}  // namespace lenses
