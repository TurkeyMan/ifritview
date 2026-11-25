#pragma once

#include "../color_space.h"

#include <cstddef>
#include <cstdint>
#include <string>

// The human-readable description ('desc' tag) of an ICC profile, e.g.
// "sRGB IEC61966-2.1" or "Adobe RGB (1998)". Empty if unreadable.
std::string icc_profile_description(const uint8_t* data, size_t size);

// A matrix profile with BT.709/sRGB primaries (independent of its transfer
// curve). Unknown and LUT profiles are conservative: they may be wide gamut.
bool icc_bt709_primaries(const uint8_t* data, size_t size);

// Which standard space a matrix profile's colorants are (gamut_of's):
// Unknown where it has none to read.
Gamut icc_gamut(const uint8_t* data, size_t size);
