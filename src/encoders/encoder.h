#pragma once

#include "../image.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// One setting a format offers when saving; the save dialog shows a format's
// settings when it's chosen.
struct EncodeSetting
{
    enum class Kind
    {
        Range,   // min..max
        Toggle,  // 0 / 1
        Choice   // an index into `choices`
    };
    const char* key;
    const char* label;
    Kind kind;
    int min = 0, max = 1, initial = 0;
    std::vector<const char*> choices = {};
    const char* note = nullptr;      // a few words beside the setting
    const char* off_when = nullptr;  // greyed while this toggle is on ("quality" under "lossless")
};

// Values by setting key; a missing one is the setting's initial value.
struct EncodeOptions
{
    std::map<std::string, int> values;
    // Written into the file as it is, where the format can hold it: the
    // caller prepares it (prepare_metadata) for this file and choice.
    std::shared_ptr<const FileMetadata> metadata;
    int get(const char* key, int fallback) const
    {
        auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
};

struct Encoder
{
    const char* name;       // shown in the format chooser
    const char* extension;  // written onto the file name
    std::vector<EncodeSetting> settings;
    // A std::function so a plugin's encoder can carry its format along.
    std::function<bool(const Image& image, const std::string& path, const EncodeOptions& options, std::string& error)> encode;
    // Whether these options lose detail; unset means "has a quality and
    // isn't set lossless". The save dialog previews what loses detail.
    bool (*reduces)(const EncodeOptions& options) = nullptr;
    bool metadata = true;  // can hold EXIF, XMP and the rest

    const EncodeSetting* setting(const char* key) const
    {
        for (const EncodeSetting& s : settings)
            if (std::string(s.key) == key)
                return &s;
        return nullptr;
    }
    // The value of `key` in `options`, else its initial value.
    int value(const EncodeOptions& options, const char* key) const
    {
        const EncodeSetting* s = setting(key);
        return options.get(key, s ? s->initial : 0);
    }
    // Whether these options lose detail (worth previewing).
    bool lossy(const EncodeOptions& options) const
    {
        return reduces ? reduces(options) : setting("quality") && !value(options, "lossless");
    }
};

// Encoders for the formats compiled in, PNG first, then the plugins'.
const std::vector<const Encoder*>& encoders();

// Straight-alpha RGBA8 or RGB8 rows from the premultiplied ARGB32 buffer,
// which is what every encoder wants.
std::vector<uint8_t> to_rgba8(const Image& image);
std::vector<uint8_t> to_rgb8(const Image& image);
