#define IFRIT_GST_NO_REDIRECT  // here the names are the real declarations
#include "gst_api.h"

#include <dlfcn.h>

#include <cstdio>
#include <type_traits>

GstApi g_gst;

namespace
{

bool load()
{
    if (g_getenv("IFRIT_NO_VIDEO"))
        return false;
    const char* libraries[] = {"libgstreamer-1.0.so.0", "libgstapp-1.0.so.0", "libgstvideo-1.0.so.0", "libgstaudio-1.0.so.0"};
    void* handles[4] = {};
    for (int i = 0; i < 4; ++i)
        if (!(handles[i] = dlopen(libraries[i], RTLD_NOW | RTLD_LOCAL)))
        {
            if (g_getenv("IFRIT_TRACE"))
                std::fprintf(stderr, "video: %s\n", dlerror());
            return false;  // the handles stay open; harmless, and it only happens once
        }
    GstApi api;
    bool ok = true;
    auto resolve = [&](auto& fn, const char* name)
    {
        for (void* h : handles)
            if (void* sym = dlsym(h, name))
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

bool gst_api_load()
{
    static const bool loaded = load();
    return loaded;
}
