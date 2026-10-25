#pragma once

// White balance for a raw the GPU develops (RawMosaic): a colour temperature
// and tint as the camera's own channel multipliers, through its colour
// matrix (XYZ to camera RGB), and the as-shot multipliers back as a
// temperature and tint for the controls to start from.

struct RawMosaic;

// The white of `kelvin` (1667..25000), moved off the Planckian/daylight
// locus by `tint` (-150..150: + magenta, - green), as the R, G, B
// multipliers that make it neutral in this camera, the least 1.
void wb_multipliers(const RawMosaic& m, double kelvin, double tint, float out[3]);

// The temperature and tint nearest the camera's as-shot multipliers.
void wb_as_shot(const RawMosaic& m, double& kelvin, double& tint);
