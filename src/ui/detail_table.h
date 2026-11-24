#pragma once

#include "../detail.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// A snapshot of noise.hlsl's completed fit. The GPU has already made the
// pixel laws and expectation inverses: do not fit or tabulate them again.
// Owns its data so a source/settings change cannot invalidate worker work.
struct DetailNoise
{
    NoiseModel model;
    float sigma_luminance = 0, sigma_colour = 0;
};

// The noise buffer read back after measure_noise. A malformed/truncated
// snapshot is refused; an unfitted green is valid and needs a zero shift.
std::optional<DetailNoise> read_detail_noise(std::span<const std::byte> words);

// Shared with the CPU develop: shaping and the exact same decode table.
// Pure CPU, suitable for a worker. RGBA32F, 1024 x 1, RGB shifts, alpha 0.
std::vector<float> make_detail_table(const DetailNoise& noise, const Detail& settings);
