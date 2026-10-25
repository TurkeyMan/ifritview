#pragma once

// The image pipeline on the GPU (docs/gpu-pipeline.md): OpenGL ES 3 in a
// context of our own, offscreen, usable from any thread (one at a time).
// For now it renders an image's 8-bit pixels from its own samples, as
// Image::retone_buffer() and render_source() do, so the two can be checked
// against each other; the view and HDR presentation come later.

#include "pixel_buffer.h"

#include <string>

struct Image;

namespace gpu
{

// A context could be made (tried once, on first use). IFRIT_NO_GPU=1 says no.
bool available();
// What the GPU is, for traces and --version ("llvmpipe", "Mesa Intel …").
std::string renderer();

// The image's 8-bit premultiplied ARGB from its own samples (a raw's linear
// data, or its source data) at exposure `ev`: what Image::retone_buffer(ev)
// gives a tonable image, and render_source(ev) any other. False (with why)
// when there's no GPU, no such samples, or the image is too big for one
// texture.
bool render(const Image& image, double ev, PixelBuffer& out, std::string& error);

}  // namespace gpu
