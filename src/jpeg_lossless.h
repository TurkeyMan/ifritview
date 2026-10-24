#pragma once
// Saving a JPEG's turns, flips and crop without decoding and encoding it
// again: libjpeg-turbo's lossless transform redoes them on the file's
// compressed blocks, so not one pixel changes. JPEG's 8 or 16 pixel blocks
// set the limits: a crop's top-left corner moves in to the next block's,
// and a turn or flip that would bring a partial edge block to the top or
// left drops it. Either way the result is the image as shown or a part of
// it, never more; the plan says what it lacks.

#include "image.h"
#include "metadata/file_metadata.h"

#include <cstdint>
#include <string>
#include <vector>

struct LosslessJpeg
{
    std::string why_not;        // empty when it can be done
    std::vector<uint8_t> data;  // the transformed JPEG, without metadata (added when written)
    int width = 0, height = 0;  // its size
    int x = 0, y = 0;           // its top-left in the image as shown (> 0: strips it lacks)
    std::string differs;        // how it differs from the image as shown; empty when it doesn't
};

// The image as shown, from the JPEG at `path` it was read from.
LosslessJpeg plan_lossless_jpeg(const Image& image, const std::string& path);

// Writes the planned JPEG with `metadata` (EXIF, XMP, ICC, IPTC) in place of
// the original's markers.
bool write_lossless_jpeg(const LosslessJpeg& plan, const FileMetadata* metadata, const std::string& out, std::string& error);
