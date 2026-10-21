#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The human-readable description ('desc' tag) of an ICC profile, e.g.
// "sRGB IEC61966-2.1" or "Adobe RGB (1998)". Empty if unreadable.
std::string icc_profile_description(const uint8_t* data, size_t size);
