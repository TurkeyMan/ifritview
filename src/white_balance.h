#pragma once

// White balance for a raw the GPU develops (RawMosaic): a colour temperature
// and tint as the camera's own channel multipliers, through its colour
// matrix (XYZ to camera RGB), and the as-shot multipliers back as a
// temperature and tint for the controls to start from. A DNG's by its own
// colour model (DngColour), as its specification has it: the white's
// temperature picks the matrices, which in turn find the white.

#include "color_space.h"

struct DngColour;
struct RawMosaic;

// The white of `kelvin` (1667..25000), moved off the Planckian/daylight
// locus by `tint` (-150..150: + magenta, - green), as the R, G, B
// multipliers that make it neutral in this camera, the least 1.
void wb_multipliers(const RawMosaic& m, double kelvin, double tint, float out[3]);

// The temperature and tint nearest the camera's as-shot multipliers.
void wb_as_shot(const RawMosaic& m, double& kelvin, double& tint);

// A DNG's: the camera's RGB of the white `white`, the greatest 1.
Vec3 dng_neutral(const DngColour& d, ColorSpace::XY white);

// The white whose camera RGB is `neutral`: the matrices for a guess at its
// temperature, the white they give, until it settles (from D50).
ColorSpace::XY dng_white(const DngColour& d, const Vec3& neutral);

// The camera's RGB (as read, unbalanced) to XYZ seen under D65, balanced
// for `neutral` (the greatest 1), which comes out white at Y = 1: with
// ForwardMatrix, through D50; without, ColorMatrix inverted, the white
// adapted to D65 (Bradford).
Mat3 dng_camera_to_xyz(const DngColour& d, const Vec3& neutral);
