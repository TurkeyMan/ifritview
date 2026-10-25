#pragma once

#include "controls.h"

#include <string>
#include <vector>

// User settings, stored as a GKeyFile at $XDG_CONFIG_HOME/ifritview/ifritview.conf.
// How the view resamples a zoomed image (on the GPU; the CPU's drawing has
// only smooth or not, which is anything but Nearest).
enum class ScaleFilter
{
    Nearest,
    Area,
    Mitchell,
    CatmullRom,
    Lanczos3,
};
const char* scale_filter_name(ScaleFilter);  // as the settings file has it: "lanczos3"
ScaleFilter scale_filter_named(const std::string& name, ScaleFilter otherwise);

struct Config
{
    // Files
    std::vector<std::string> extensions;  // lowercase, no dot; which files count when cycling a folder
    bool wrap_around = true;              // next after the last image goes to the first
    bool show_hidden = false;
    bool recursive = false;               // the folders inside too, in order of their paths
    bool interleave_folders = false;      // and the folders inside among a folder's images, by name (else after them)
    int prefetch_ahead = 3;  // images decoded in advance in the browsing direction
    int prefetch_behind = 1;
    int cache_megabytes = 0;        // decoded image cache; 0 = a share of free memory
    std::string raw_mode = "full";  // full | half | preview; half bins 2x2 (fast, no demosaic)
    bool raw_camera_crop = true;    // crop the sensor margins to the camera's own framing
    bool apply_xmp_edits = true;    // honour exposure/crop/orientation from XMP

    // Window
    bool fit_window_to_image = true;
    int min_window_width = 640;  // logical pixels; the window never shrinks below this
    int min_window_height = 480;
    int metadata_pane_width = 420;  // the docked info pane, logical pixels; the divider is draggable

    // Viewing
    ScaleFilter enlarge_filter = ScaleFilter::Lanczos3;
    ScaleFilter shrink_filter = ScaleFilter::Lanczos3;
    // What the CPU's drawing can do of those: smooth, or nearest.
    bool smooth_upscale() const { return enlarge_filter != ScaleFilter::Nearest; }
    bool smooth_downscale() const { return shrink_filter != ScaleFilter::Nearest; }
    bool fullscreen_upscale = false;  // scale small images up to fill the screen in fullscreen
    bool checkerboard = true;         // show transparency over a checkerboard
    bool histogram = false;           // the histogram over the image in fullscreen (G toggles)
    int histogram_channels = 15;      // shown in it: bits 1 R, 2 G, 4 B, 8 luminance
    bool levels_in_nits = true;       // the histogram's numbers in nits (and EV from white), else as a share of white
    bool animate = true;
    bool video_autoplay = true;
    bool video_muted = true;
    double video_volume = 0.7;  // 0..1, perceptual (cubic)
    bool video_loop = false;    // play animated GIF/WebP

    // Slideshow
    double slideshow_seconds = 5.0;
    bool slideshow_motion = true;  // gentle pan/zoom on each slide
    double slideshow_zoom = 0.08;  // how far the motion zooms, as a fraction
    bool slideshow_shuffle = false;
    int slideshow_crossfade_ms = 700;  // 0 = cut
    std::string background = "#262626";
    std::string fullscreen_background = "#000000";

    // Fullscreen info overlay: categories of metadata drawn over the image.
    bool overlay_enabled = true;  // master switch; I toggles it in fullscreen
    bool overlay_file = true;
    bool overlay_format = false;
    bool overlay_shot = true;
    bool overlay_location = false;
    bool overlay_view = false;
    bool overlay_windowed = false;  // also show it in the window, not just fullscreen
    std::string overlay_color = "#a6d96a";
    int overlay_font_pt = 10;

    // Save As: the metadata kept, as parse_metadata_choice reads it.
    std::string save_metadata = "all";

    // Maps: where a GPS location link goes; {lat} and {lon} are substituted.
    std::string map_url = "https://www.google.com/maps?q={lat},{lon}";
    std::string map_link(double latitude, double longitude) const;
    // Reverse geocoding provider, {lat}/{lon} template; empty = off.
    // The interface's language, a tag such as "de"; empty for the system's.
    // Takes effect when IfritView next starts.
    std::string language;
    std::string geocoder_url = "https://nominatim.openstreetmap.org/reverse?lat={lat}&lon={lon}&format=jsonv2&zoom=14";

    // Mouse: one binding set for windowed mode, another for fullscreen.
    Bindings controls = default_bindings();
    Bindings fullscreen_controls = default_fullscreen_bindings();
    Action control(Control c, bool fullscreen = false) const { return (fullscreen ? fullscreen_controls : controls)[size_t(c)]; }

    static std::string path();
    // The settings file's group with the interface's language (read alone at
    // startup, before the rest: saved_language() in i18n.cpp).
    static constexpr const char* kInterfaceGroup = "window";
    void load();  // missing file or keys leave the defaults
    bool save() const;
    void reset_extensions();
    bool accepts_extension(const std::string& ext) const;  // ext lowercase, no dot
    // Supported extensions the list hasn't met before (not in the saved
    // known_extensions) join it, as when a plugin brings new formats; ones
    // the user took out stay out. Whether any were added.
    bool add_new_extensions(const std::vector<std::string>& supported);
    std::vector<std::string> known_extensions;  // as of the last save; empty without a settings file
};
