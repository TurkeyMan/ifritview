#include "config.h"

#include "decoders/decoder.h"

#include <glib.h>

#include <algorithm>
#include <cstdio>

namespace
{

const char* kFiles = "files";
const char* kWindow = "window";
const char* kView = "view";
const char* kControlsGroup = "controls";
const char* kFullscreenControlsGroup = "controls_fullscreen";
const char* kMaps = "maps";
const char* kOverlay = "overlay";

bool get_bool(GKeyFile* kf, const char* group, const char* key, bool fallback)
{
    GError* err = nullptr;
    gboolean v = g_key_file_get_boolean(kf, group, key, &err);
    if (err)
    {
        g_error_free(err);
        return fallback;
    }
    return v;
}

int get_int(GKeyFile* kf, const char* group, const char* key, int fallback)
{
    GError* err = nullptr;
    int v = g_key_file_get_integer(kf, group, key, &err);
    if (err)
    {
        g_error_free(err);
        return fallback;
    }
    return v;
}

std::string get_string(GKeyFile* kf, const char* group, const char* key, const std::string& fallback)
{
    gchar* v = g_key_file_get_string(kf, group, key, nullptr);
    if (!v)
        return fallback;
    std::string s = v;
    g_free(v);
    return s;
}

}  // namespace

std::string Config::path()
{
    gchar* p = g_build_filename(g_get_user_config_dir(), "ifritview", "ifritview.conf", nullptr);
    std::string s = p;
    g_free(p);
    return s;
}

void Config::reset_extensions()
{
    extensions = default_extensions();
}

std::string Config::map_link(double latitude, double longitude) const
{
    char lat[32], lon[32];
    std::snprintf(lat, sizeof lat, "%.6f", latitude);
    std::snprintf(lon, sizeof lon, "%.6f", longitude);
    std::string url = map_url;
    for (auto pos = url.find("{lat}"); pos != std::string::npos; pos = url.find("{lat}"))
        url.replace(pos, 5, lat);
    for (auto pos = url.find("{lon}"); pos != std::string::npos; pos = url.find("{lon}"))
        url.replace(pos, 5, lon);
    return url;
}

bool Config::accepts_extension(const std::string& ext) const
{
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

void Config::load()
{
    reset_extensions();
    GKeyFile* kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path().c_str(), G_KEY_FILE_NONE, nullptr))
    {
        g_key_file_free(kf);
        return;
    }
    gsize n = 0;
    if (gchar** list = g_key_file_get_string_list(kf, kFiles, "extensions", &n, nullptr))
    {
        extensions.clear();
        for (gsize i = 0; i < n; ++i)
        {
            std::string e = list[i];
            e.erase(std::remove_if(e.begin(), e.end(), [](unsigned char c) { return std::isspace(c) || c == '.'; }), e.end());
            std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
            if (!e.empty() && !accepts_extension(e))
                extensions.push_back(e);
        }
        g_strfreev(list);
    }
    wrap_around = get_bool(kf, kFiles, "wrap_around", wrap_around);
    show_hidden = get_bool(kf, kFiles, "show_hidden", show_hidden);
    prefetch_ahead = std::clamp(get_int(kf, kFiles, "prefetch_ahead", prefetch_ahead), 0, 16);
    prefetch_behind = std::clamp(get_int(kf, kFiles, "prefetch_behind", prefetch_behind), 0, 16);
    cache_megabytes = std::max(0, get_int(kf, kFiles, "cache_megabytes", cache_megabytes));
    raw_mode = get_string(kf, kFiles, "raw_mode", raw_mode);
    if (raw_mode != "full" && raw_mode != "half" && raw_mode != "preview")
        raw_mode = "full";
    apply_xmp_edits = get_bool(kf, kFiles, "apply_xmp_edits", apply_xmp_edits);
    raw_camera_crop = get_bool(kf, kFiles, "raw_camera_crop", raw_camera_crop);

    fit_window_to_image = get_bool(kf, kWindow, "fit_to_image", fit_window_to_image);
    min_window_width = std::max(200, get_int(kf, kWindow, "min_width", min_window_width));
    min_window_height = std::max(150, get_int(kf, kWindow, "min_height", min_window_height));

    smooth_upscale = get_bool(kf, kView, "smooth_upscale", smooth_upscale);
    smooth_downscale = get_bool(kf, kView, "smooth_downscale", smooth_downscale);
    fullscreen_upscale = get_bool(kf, kView, "fullscreen_upscale", fullscreen_upscale);
    checkerboard = get_bool(kf, kView, "checkerboard", checkerboard);
    animate = get_bool(kf, kView, "animate", animate);
    background = get_string(kf, kView, "background", background);
    fullscreen_background = get_string(kf, kView, "fullscreen_background", fullscreen_background);
    overlay_file = get_bool(kf, kOverlay, "file", overlay_file);
    overlay_format = get_bool(kf, kOverlay, "format", overlay_format);
    overlay_shot = get_bool(kf, kOverlay, "shot", overlay_shot);
    overlay_location = get_bool(kf, kOverlay, "location", overlay_location);
    overlay_view = get_bool(kf, kOverlay, "view", overlay_view);
    overlay_windowed = get_bool(kf, kOverlay, "windowed", overlay_windowed);
    overlay_color = get_string(kf, kOverlay, "color", overlay_color);
    overlay_font_pt = std::clamp(get_int(kf, kOverlay, "font_pt", overlay_font_pt), 6, 40);
    map_url = get_string(kf, kMaps, "url", map_url);
    geocoder_url = get_string(kf, kMaps, "geocoder_url", geocoder_url);
    if (!geocoder_url.empty() && (geocoder_url.find("{lat}") == std::string::npos || geocoder_url.find("{lon}") == std::string::npos))
        geocoder_url.clear();
    if (map_url.find("{lat}") == std::string::npos || map_url.find("{lon}") == std::string::npos)
        map_url = "https://www.google.com/maps?q={lat},{lon}";
    for (const ControlInfo& c : kControls)
    {
        Action& a = controls[size_t(c.id)];
        a = action_from_key(get_string(kf, kControlsGroup, c.key, action_info(a).key), a);
        if (!action_fits(a, c.kind))
            a = Action::None;
        Action& f = fullscreen_controls[size_t(c.id)];
        f = action_from_key(get_string(kf, kFullscreenControlsGroup, c.key, action_info(f).key), f);
        if (!action_fits(f, c.kind))
            f = Action::None;
    }
    g_key_file_free(kf);
}

bool Config::save() const
{
    GKeyFile* kf = g_key_file_new();
    std::vector<const gchar*> exts;
    for (const std::string& e : extensions)
        exts.push_back(e.c_str());
    g_key_file_set_string_list(kf, kFiles, "extensions", exts.data(), exts.size());
    g_key_file_set_boolean(kf, kFiles, "wrap_around", wrap_around);
    g_key_file_set_boolean(kf, kFiles, "show_hidden", show_hidden);
    g_key_file_set_integer(kf, kFiles, "prefetch_ahead", prefetch_ahead);
    g_key_file_set_integer(kf, kFiles, "prefetch_behind", prefetch_behind);
    g_key_file_set_integer(kf, kFiles, "cache_megabytes", cache_megabytes);
    g_key_file_set_string(kf, kFiles, "raw_mode", raw_mode.c_str());
    g_key_file_set_boolean(kf, kFiles, "apply_xmp_edits", apply_xmp_edits);
    g_key_file_set_boolean(kf, kFiles, "raw_camera_crop", raw_camera_crop);

    g_key_file_set_boolean(kf, kWindow, "fit_to_image", fit_window_to_image);
    g_key_file_set_integer(kf, kWindow, "min_width", min_window_width);
    g_key_file_set_integer(kf, kWindow, "min_height", min_window_height);

    g_key_file_set_boolean(kf, kView, "smooth_upscale", smooth_upscale);
    g_key_file_set_boolean(kf, kView, "smooth_downscale", smooth_downscale);
    g_key_file_set_boolean(kf, kView, "fullscreen_upscale", fullscreen_upscale);
    g_key_file_set_boolean(kf, kView, "checkerboard", checkerboard);
    g_key_file_set_boolean(kf, kView, "animate", animate);
    g_key_file_set_string(kf, kView, "background", background.c_str());
    g_key_file_set_string(kf, kView, "fullscreen_background", fullscreen_background.c_str());
    for (const ControlInfo& c : kControls)
    {
        g_key_file_set_string(kf, kControlsGroup, c.key, action_info(controls[size_t(c.id)]).key);
        g_key_file_set_string(kf, kFullscreenControlsGroup, c.key, action_info(fullscreen_controls[size_t(c.id)]).key);
    }
    g_key_file_set_string(kf, kMaps, "url", map_url.c_str());
    g_key_file_set_string(kf, kMaps, "geocoder_url", geocoder_url.c_str());
    g_key_file_set_boolean(kf, kOverlay, "file", overlay_file);
    g_key_file_set_boolean(kf, kOverlay, "format", overlay_format);
    g_key_file_set_boolean(kf, kOverlay, "shot", overlay_shot);
    g_key_file_set_boolean(kf, kOverlay, "location", overlay_location);
    g_key_file_set_boolean(kf, kOverlay, "view", overlay_view);
    g_key_file_set_boolean(kf, kOverlay, "windowed", overlay_windowed);
    g_key_file_set_string(kf, kOverlay, "color", overlay_color.c_str());
    g_key_file_set_integer(kf, kOverlay, "font_pt", overlay_font_pt);

    const std::string p = path();
    gchar* dir = g_path_get_dirname(p.c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    GError* err = nullptr;
    const bool ok = g_key_file_save_to_file(kf, p.c_str(), &err);
    if (err)
    {
        g_warning("could not save %s: %s", p.c_str(), err->message);
        g_error_free(err);
    }
    g_key_file_free(kf);
    return ok;
}
