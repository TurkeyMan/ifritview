#pragma once

#include "image.h"

// High-quality downscale of an image to an exact size, for drawing 1:1:
// starts from the smallest mip level still at least that size, then box
// filters (each output pixel averages the source area it covers, with
// fractional edge weights). Premultiplied BGRA in and out.
// Upscaling isn't its job; the caller draws the original with a filter.
PixelBuffer downscale(const Image& image, int width, int height);
