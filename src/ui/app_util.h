#pragma once

#include <cstdlib>
// Small things the App's parts share (app.cpp and the app_*.cpp beside it).

#include "config.h"
#include "decoders/decoder.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <chrono>
#include <cstdint>
#include <random>
#include <string>

namespace app_util
{

inline int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline bool tracing()
{
    return g_getenv("IFRIT_TRACE") != nullptr;
}

inline DecodeOptions decode_options_for(const Config& cfg)
{
    DecodeOptions o;
    using RawMode = DecodeOptions::RawMode;
    o.raw_mode = cfg.raw_mode == "full" ? RawMode::Full : cfg.raw_mode == "preview" ? RawMode::Preview : RawMode::Half;
    o.raw_camera_crop = cfg.raw_camera_crop;
#ifdef HAVE_GPU
    // Raws developed on the GPU (the CPU only unpacks), unless the view is told off it.
    o.gpu_develop = !std::getenv("IFRIT_NO_GPU_VIEW") && !std::getenv("IFRIT_CPU_DEVELOP");
    o.gpu_decode = o.gpu_develop && !std::getenv("IFRIT_CPU_RAW_DECODE");
#endif
    return o;
}

inline std::string human_size(uint64_t bytes)
{
    gchar* s = g_format_size(bytes);
    std::string r = s;
    g_free(s);
    return r;
}

inline std::string file_time(const std::string& path, const char* format)
{
    GStatBuf st;
    if (g_stat(path.c_str(), &st) != 0)
        return {};
    GDateTime* t = g_date_time_new_from_unix_local(st.st_mtime);
    if (!t)
        return {};
    gchar* text = g_date_time_format(t, format);
    std::string s = text ? text : "";
    g_free(text);
    g_date_time_unref(t);
    return s;
}

inline std::mt19937& rng()
{
    static std::mt19937 r{std::random_device{}()};
    return r;
}

inline double random01()
{
    return std::uniform_real_distribution<double>(0, 1)(rng());
}

}  // namespace app_util

using namespace app_util;
