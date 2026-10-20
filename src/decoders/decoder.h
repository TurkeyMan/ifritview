#pragma once

#include "../image.h"

#include <cstddef>
#include <string>
#include <vector>

// One image format backend. Decoders are plain tables of functions so the
// registry can try them by extension first, then by sniffing the file header.
struct Decoder {
    const char* name;
    std::vector<std::string> extensions;                        // lowercase, without the dot
    bool (*sniff)(const uint8_t* head, size_t len);             // may be null
    bool (*decode)(const std::string& path, Image& out, std::string& error);
};

// Options the decoders consult. Set from the main thread, read from the
// loader thread; copied under a lock so a settings change can't tear.
struct DecodeOptions {
    enum class RawMode { Full, Half, Preview };
    RawMode raw_mode = RawMode::Half;
};
void set_decode_options(const DecodeOptions& options);
DecodeOptions decode_options();

// Compiled-in decoders in priority order.
const std::vector<const Decoder*>& decoders();

// Decode a file with whichever decoder claims it; `error` describes the
// failure of the most relevant decoder.
bool decode_image(const std::string& path, Image& out, std::string& error);

// Every extension handled by the compiled-in decoders, in priority order.
std::vector<std::string> default_extensions();

// Backend constructors, each defined in its own file; only those compiled in
// are referenced from decoder.cpp.
const Decoder& jpeg_decoder();
const Decoder& png_decoder();
const Decoder& webp_decoder();
const Decoder& heif_decoder();
const Decoder& jxl_decoder();
const Decoder& tiff_decoder();
const Decoder& raw_decoder();
const Decoder& stb_decoder();
const Decoder& pixbuf_decoder();

#ifdef HAVE_JPEG
// Used by the raw decoder for embedded JPEG previews.
bool decode_jpeg_memory(const uint8_t* data, size_t size, Image& out, std::string& error);
#endif
