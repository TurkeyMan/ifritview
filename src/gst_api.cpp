#define IFRIT_GST_NO_REDIRECT  // here the names are the real declarations
#include "gst_api.h"

#include "platform.h"

#include <cstdio>
#include <type_traits>

GstApi g_gst;

namespace
{

const char* const kLibraries[] = {
    IFRIT_LIBRARY_NAME("libgstreamer-1.0.so.0", "libgstreamer-1.0-0.dll"),
    IFRIT_LIBRARY_NAME("libgstapp-1.0.so.0", "libgstapp-1.0-0.dll"),
    IFRIT_LIBRARY_NAME("libgstvideo-1.0.so.0", "libgstvideo-1.0-0.dll"),
    IFRIT_LIBRARY_NAME("libgstaudio-1.0.so.0", "libgstaudio-1.0-0.dll"),
};

bool load()
{
    if (g_getenv("IFRIT_NO_VIDEO"))
        return false;
    void* handles[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        std::string error;
        if (!(handles[i] = platform::open_library(kLibraries[i], &error)))
        {
            if (g_getenv("IFRIT_TRACE"))
                std::fprintf(stderr, "video: %s\n", error.c_str());
            return false;  // the handles stay open; harmless, and it only happens once
        }
    }
    GstApi api;
    bool ok = true;
    auto resolve = [&](auto& fn, const char* name)
    {
        for (void* h : handles)
            if (void* sym = platform::library_symbol(h, name))
            {
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(sym);
                return;
            }
        if (g_getenv("IFRIT_TRACE"))
            std::fprintf(stderr, "video: %s missing\n", name);
        ok = false;
    };
#define IFRIT_GST_RESOLVE(name) resolve(api.name, #name);
    IFRIT_GST_FUNCTIONS(IFRIT_GST_RESOLVE)
#undef IFRIT_GST_RESOLVE
    if (!ok)
        return false;
    g_gst = api;
    if (!g_gst.gst_init_check(nullptr, nullptr, nullptr))
    {
        g_gst = GstApi{};
        return false;
    }
    return true;
}

}  // namespace

bool gst_api_available()
{
    if (g_getenv("IFRIT_NO_VIDEO"))
        return false;
    for (const char* lib : kLibraries)
        if (!platform::library_installed(lib))
            return false;
    return true;
}

bool gst_api_load()
{
    static const bool loaded = load();
    return loaded;
}
