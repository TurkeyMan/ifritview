#include "probe.h"

#include "decoders/decoder.h"

#include <glib.h>

#include <cstdio>

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
