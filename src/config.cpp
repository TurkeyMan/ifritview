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
const char* kSave = "save";

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

namespace
{
constexpr std::pair<ScaleFilter, const char*> kScaleFilters[] = {
    {ScaleFilter::Nearest, "nearest"},   {ScaleFilter::Area, "area"},         {ScaleFilter::Mitchell, "mitchell"},
    {ScaleFilter::CatmullRom, "catmull-rom"}, {ScaleFilter::Lanczos3, "lanczos3"},
};
}  // namespace

const char* scale_filter_name(ScaleFilter f)
{
    for (auto [filter, name] : kScaleFilters)
        if (filter == f)
            return name;
    return "lanczos3";
}

ScaleFilter scale_filter_named(const std::string& name, ScaleFilter otherwise)
{
    for (auto [filter, n] : kScaleFilters)
        if (name == n)
            return filter;
    return otherwise;
}

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

bool Config::add_new_extensions(const std::vector<std::string>& supported)
{
    bool added = false;
    for (const std::string& e : supported)
        if (!accepts_extension(e) && std::find(known_extensions.begin(), known_extensions.end(), e) == known_extensions.end())
        {
            extensions.push_back(e);
            added = true;
        }
    return added;
}

void Config::load()
{
    reset_extensions();
    known_extensions.clear();
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
            const std::string e = normalise_extension(list[i]);
            if (!e.empty() && e != "gz" && e != "svggz" && !accepts_extension(e))  // gz came from the SVGZ loader by mistake
                extensions.push_back(e);
        }
        g_strfreev(list);
        // Formats added since the list was saved join it; ones the user took
        // out stay out. `known_extensions` records what was supported at the
        // last save. Older files predate it: everything missing from them is
        // new except PDF, which may have been left out on purpose.
        std::vector<std::string>& known = known_extensions;
        gsize k = 0;
        if (gchar** kl = g_key_file_get_string_list(kf, kFiles, "known_extensions", &k, nullptr))
        {
            known.assign(kl, kl + k);
            g_strfreev(kl);
        }
        else
        {
            known = extensions;
            known.push_back("pdf");
        }
        add_new_extensions(default_extensions());
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
    metadata_pane_width = std::clamp(get_int(kf, kWindow, "metadata_pane_width", metadata_pane_width), 240, 1600);

    // The filters by name; settings from before them said only smooth or not.
    if (!get_bool(kf, kView, "smooth_upscale", true))
        enlarge_filter = ScaleFilter::Nearest;
    if (!get_bool(kf, kView, "smooth_downscale", true))
        shrink_filter = ScaleFilter::Nearest;
    enlarge_filter = scale_filter_named(get_string(kf, kView, "enlarge_filter", scale_filter_name(enlarge_filter)), enlarge_filter);
    shrink_filter = scale_filter_named(get_string(kf, kView, "shrink_filter", scale_filter_name(shrink_filter)), shrink_filter);
    fullscreen_upscale = get_bool(kf, kView, "fullscreen_upscale", fullscreen_upscale);
    checkerboard = get_bool(kf, kView, "checkerboard", checkerboard);
    histogram = get_bool(kf, kView, "histogram", histogram);
    histogram_channels = get_int(kf, kView, "histogram_channels", histogram_channels) & 15;
    levels_in_nits = get_bool(kf, kView, "levels_in_nits", levels_in_nits);
    animate = get_bool(kf, kView, "animate", animate);
    video_autoplay = get_bool(kf, kView, "video_autoplay", video_autoplay);
    video_muted = get_bool(kf, kView, "video_muted", video_muted);
    video_loop = get_bool(kf, kView, "video_loop", video_loop);
    if (g_key_file_has_key(kf, kView, "video_volume", nullptr))
        video_volume = std::clamp(g_key_file_get_double(kf, kView, "video_volume", nullptr), 0.0, 1.0);
    slideshow_seconds = std::clamp(g_key_file_get_double(kf, kView, "slideshow_seconds", nullptr), 0.5, 600.0);
    if (!g_key_file_has_key(kf, kView, "slideshow_seconds", nullptr))
        slideshow_seconds = 5.0;
    slideshow_motion = get_bool(kf, kView, "slideshow_motion", slideshow_motion);
    slideshow_zoom = std::clamp(
        g_key_file_has_key(kf, kView, "slideshow_zoom", nullptr) ? g_key_file_get_double(kf, kView, "slideshow_zoom", nullptr) : slideshow_zoom, 0.0, 0.5);
    slideshow_shuffle = get_bool(kf, kView, "slideshow_shuffle", slideshow_shuffle);
    slideshow_crossfade_ms = std::clamp(get_int(kf, kView, "slideshow_crossfade_ms", slideshow_crossfade_ms), 0, 5000);
    background = get_string(kf, kView, "background", background);
    fullscreen_background = get_string(kf, kView, "fullscreen_background", fullscreen_background);
    overlay_enabled = get_bool(kf, kOverlay, "enabled", overlay_enabled);
    overlay_file = get_bool(kf, kOverlay, "file", overlay_file);
    overlay_format = get_bool(kf, kOverlay, "format", overlay_format);
    overlay_shot = get_bool(kf, kOverlay, "shot", overlay_shot);
    overlay_location = get_bool(kf, kOverlay, "location", overlay_location);
    overlay_view = get_bool(kf, kOverlay, "view", overlay_view);
    overlay_windowed = get_bool(kf, kOverlay, "windowed", overlay_windowed);
    overlay_color = get_string(kf, kOverlay, "color", overlay_color);
    overlay_font_pt = std::clamp(get_int(kf, kOverlay, "font_pt", overlay_font_pt), 6, 40);
    save_metadata = get_string(kf, kSave, "metadata", save_metadata);
    map_url = get_string(kf, kMaps, "url", map_url);
    language = get_string(kf, kInterfaceGroup, "language", language);
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
    const std::vector<std::string> supported = default_extensions();
    std::vector<const gchar*> known;
    for (const std::string& e : supported)
        known.push_back(e.c_str());
    g_key_file_set_string_list(kf, kFiles, "known_extensions", known.data(), known.size());
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
    g_key_file_set_integer(kf, kWindow, "metadata_pane_width", metadata_pane_width);

    g_key_file_set_string(kf, kView, "enlarge_filter", scale_filter_name(enlarge_filter));
    g_key_file_set_string(kf, kView, "shrink_filter", scale_filter_name(shrink_filter));
    g_key_file_remove_key(kf, kView, "smooth_upscale", nullptr);
    g_key_file_remove_key(kf, kView, "smooth_downscale", nullptr);
    g_key_file_set_boolean(kf, kView, "fullscreen_upscale", fullscreen_upscale);
    g_key_file_set_boolean(kf, kView, "checkerboard", checkerboard);
    g_key_file_set_boolean(kf, kView, "histogram", histogram);
    g_key_file_set_integer(kf, kView, "histogram_channels", histogram_channels);
    g_key_file_set_boolean(kf, kView, "levels_in_nits", levels_in_nits);
    g_key_file_set_boolean(kf, kView, "animate", animate);
    g_key_file_set_boolean(kf, kView, "video_autoplay", video_autoplay);
    g_key_file_set_boolean(kf, kView, "video_muted", video_muted);
    g_key_file_set_boolean(kf, kView, "video_loop", video_loop);
    g_key_file_set_double(kf, kView, "video_volume", video_volume);
    g_key_file_set_double(kf, kView, "slideshow_seconds", slideshow_seconds);
    g_key_file_set_boolean(kf, kView, "slideshow_motion", slideshow_motion);
    g_key_file_set_double(kf, kView, "slideshow_zoom", slideshow_zoom);
    g_key_file_set_boolean(kf, kView, "slideshow_shuffle", slideshow_shuffle);
    g_key_file_set_integer(kf, kView, "slideshow_crossfade_ms", slideshow_crossfade_ms);
    g_key_file_set_string(kf, kView, "background", background.c_str());
    g_key_file_set_string(kf, kView, "fullscreen_background", fullscreen_background.c_str());
    for (const ControlInfo& c : kControls)
    {
        g_key_file_set_string(kf, kControlsGroup, c.key, action_info(controls[size_t(c.id)]).key);
        g_key_file_set_string(kf, kFullscreenControlsGroup, c.key, action_info(fullscreen_controls[size_t(c.id)]).key);
    }
    g_key_file_set_string(kf, kSave, "metadata", save_metadata.c_str());
    g_key_file_set_string(kf, kMaps, "url", map_url.c_str());
    g_key_file_set_string(kf, kInterfaceGroup, "language", language.c_str());
    g_key_file_set_string(kf, kMaps, "geocoder_url", geocoder_url.c_str());
    g_key_file_set_boolean(kf, kOverlay, "enabled", overlay_enabled);
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
