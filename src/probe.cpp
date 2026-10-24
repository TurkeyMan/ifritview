#include "probe.h"

#include "pipeline.h"

#include "decoders/decoder.h"
#include "encoders/encoder.h"

#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{

uint32_t crc32(const uint8_t* d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
    {
        c ^= d[i];
        for (int k = 0; k < 8; ++k)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

}  // namespace

int probe(int argc, char** argv)
{
    int failures = 0;
    for (int i = 0; i < argc; ++i)
    {
        Image img;
        std::string error;
        const gint64 t0 = g_get_monotonic_time();
        const bool ok = decode_image(argv[i], img, error);
        const double ms = (g_get_monotonic_time() - t0) / 1000.0;
        if (ok)
        {
            std::printf("ok    %-40s %5dx%-5d %s%s %s (%.0f ms)\n", argv[i], img.width, img.height, img.format.c_str(), img.has_alpha ? " +alpha" : "",
                        img.info.c_str(), ms);
            if (!img.pixel_format.empty())
                std::printf("        pixel format: %s%s%s\n", img.pixel_format.describe(img.width, img.height).c_str(), img.color_space.empty() ? "" : " · ",
                            img.color_space.c_str());
            if (img.source)
            {
                // The deep samples kept beside the 8-bit pixels, and a checksum of
                // them (CRC-32 of the bytes), for checking they arrived intact.
                static const char* const types[] = {"u16", "i16", "f16", "f32"};
                static const char* const layouts[] = {"", "grey", "grey+alpha", "RGB", "RGBA"};
                const SourceData& s = *img.source;
                std::printf("        source: %s %s%s, peak %g, crc %08x\n", types[int(s.sample)], layouts[s.channels], s.linear ? " linear" : "",
                            double(s.peak), unsigned(crc32(s.data.data(), s.data.size())));
            }
            if (img.stored)
                std::printf("        stored: exif %zu, xmp %zu, iptc %zu, icc %zu bytes\n", img.stored->exif.size(), img.stored->xmp.size(),
                            img.stored->iptc.size(), img.stored->icc.size());
            if (img.xmp.present)
                std::printf("        xmp develop: exposure %+.2f temp %.0f orientation %d crop %d\n", img.xmp.exposure, img.xmp.temperature,
                            img.xmp.orientation, int(img.xmp.has_crop));
            for (const MetadataGroup& g : img.metadata)
            {
                std::printf("        %s (%zu)", g.name.c_str(), g.items.size());
                if (g_getenv("IFRIT_PROBE_VERBOSE"))
                    for (const MetadataItem& m : g.items)
                        std::printf("\n          %s = %s", m.key.c_str(), m.value.c_str());
                std::printf("\n");
            }
        }
        else
        {
            std::printf("FAIL  %-40s %s\n", argv[i], error.c_str());
            ++failures;
        }
    }
    return failures ? 1 : 0;
}

int convert(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: ifritview --convert in out [setting=value...] [metadata=CHOICE]\n");
        return 2;
    }
    // --process in save:out,setting=value,...: the one way the command line saves.
    std::string save = std::string("save:") + argv[1];
    for (int i = 2; i < argc; ++i)
        save += std::string(",") + argv[i];
    std::vector<std::string> args = {argv[0], save};
    std::vector<char*> ptrs;
    for (std::string& a : args)
        ptrs.push_back(a.data());
    return process(int(ptrs.size()), ptrs.data());
}
