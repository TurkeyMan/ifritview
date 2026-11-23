// Where a camera's colours stop being linear, remembered per camera and
// format: a property of how it encodes (a Canon mRAW's green rolls off from
// about 0.85 of its clip without piling up), which an image shows only where
// it has smooth surfaces bright enough to drift (the develop's DRIFT,
// shaders/drift.hlsli). Each sure measurement is kept (the last
// kLinearityKept of each colour); an image that doesn't show its own takes
// their median. Kept as a GKeyFile beside the settings,
// $XDG_CONFIG_HOME/ifritview/linearity.conf: a group a camera and format,
// a list of fractions of its clip a colour (1: linear to its clip).
#pragma once

#include <string>

struct RawMosaic;

namespace linearity
{

constexpr int kLinearityKept = 8;

// The raw's format, as far as it changes how its colours stop: how it's
// stored (Bayer, X-Trans, monochrome, linear, an sRAW's YCbCr and its size)
// and its stated white (a 12-bit readout's or a 14-bit's).
std::string format_of(const RawMosaic& m);

// The camera's remembered limits, each colour's a fraction of its clip; 0
// where nothing's remembered.
void recall(const std::string& camera, const std::string& format, float limit[3]);

// An image's measurement, each colour's kept where it's sure. Safe across
// processes: the file's read, changed and saved under a lock file.
void remember(const std::string& camera, const std::string& format, const float limit[3], const bool sure[3]);

std::string path();

}  // namespace linearity
