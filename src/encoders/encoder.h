#pragma once

#include "../image.h"

#include <string>
#include <vector>

struct EncodeOptions
{
    int quality = 90;  // 1..100 for lossy formats; ignored otherwise
};

struct Encoder
{
    const char* name;       // shown in the format chooser
    const char* extension;  // written onto the file name
    bool lossy;             // whether `quality` applies
    bool (*encode)(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error);
};

// Encoders for the formats compiled in, PNG first.
const std::vector<const Encoder*>& encoders();

// Straight-alpha RGBA8 or RGB8 rows from the premultiplied ARGB32 buffer,
// which is what every encoder wants.
std::vector<uint8_t> to_rgba8(const Image& image);
std::vector<uint8_t> to_rgb8(const Image& image);
