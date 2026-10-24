#pragma once
// Comparing two images of the same size: how far apart they are, and where.
// For Save As (the original against the image as it would save) and for
// diffing two files.

#include "image.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ImageDifference
{
    int width = 0, height = 0;
    int max_error = 0;            // the largest difference in any channel, 0..255
    double mean_error = 0;        // per channel, over every pixel
    double psnr = 0;              // dB over the colour channels; infinite when identical
    double changed = 0;           // the share of pixels differing by more than 2 levels
    std::vector<uint8_t> errors;  // per pixel, its largest channel difference
    std::string summary;          // said in place of the numbers, where they'd mislead
};

// `a` and `b` must be the same size (an empty result otherwise). The
// work of a moment per megapixel: for a worker thread on big images.
ImageDifference compare_images(const Image& a, const Image& b);

// Where they differ, as an image: `a` dimmed to grey, with the error
// amplified `gain` times over it, from red through yellow to white.
std::shared_ptr<Image> difference_image(const Image& a, const ImageDifference& d, int gain);
