#include "probe.h"

#include "decoders/decoder.h"
#include "encoders/encoder.h"

#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

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
        std::fprintf(stderr, "usage: ifritview --convert in out [setting=value...]\n");
        return 2;
    }
    const std::string in = argv[0], out = argv[1];
    std::string ext = out.substr(out.find_last_of('.') == std::string::npos ? out.size() : out.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == "jpeg")
        ext = "jpg";
    const Encoder* encoder = nullptr;
    for (const Encoder* e : encoders())
        if (ext == e->extension)
            encoder = e;
    if (!encoder)
    {
        std::fprintf(stderr, "no encoder for .%s\n", ext.c_str());
        return 2;
    }
    EncodeOptions options;
    for (int i = 2; i < argc; ++i)
    {
        const std::string kv = argv[i];
        const auto eq = kv.find('=');
        if (eq == std::string::npos || !encoder->setting(kv.substr(0, eq).c_str()))
        {
            std::fprintf(stderr, "%s has no setting \"%s\"; it has:", encoder->name, kv.substr(0, eq).c_str());
            for (const EncodeSetting& s : encoder->settings)
                std::fprintf(stderr, " %s", s.key);
            std::fprintf(stderr, "\n");
            return 2;
        }
        options.values[kv.substr(0, eq)] = std::atoi(kv.c_str() + eq + 1);
    }
    Image img;
    std::string error;
    if (!decode_image(in, img, error))
    {
        std::fprintf(stderr, "%s: %s\n", in.c_str(), error.c_str());
        return 1;
    }
    if (!encoder->encode(img, out, options, error))
    {
        std::fprintf(stderr, "%s: %s\n", out.c_str(), error.c_str());
        return 1;
    }
    return 0;
}
