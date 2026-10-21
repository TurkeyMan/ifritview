#pragma once

#include "controls.h"

#include <string>
#include <vector>

// User settings, stored as a GKeyFile at $XDG_CONFIG_HOME/ifritview/ifritview.conf.
struct Config {
    // Files
    std::vector<std::string> extensions;   // lowercase, no dot; which files count when cycling a folder
    bool wrap_around = true;               // next after the last image goes to the first
    bool show_hidden = false;
    std::string raw_mode = "half";         // full | half | preview
    bool apply_xmp_edits = true;           // honour exposure/crop/orientation from XMP

    // Window
    bool fit_window_to_image = true;
    int min_window_width = 640;            // logical pixels; the window never shrinks below this
    int min_window_height = 480;

    // Viewing
    bool smooth_upscale = true;
    bool smooth_downscale = true;
    bool fullscreen_upscale = false;       // scale small images up to fill the screen in fullscreen
    bool checkerboard = true;              // show transparency over a checkerboard
    std::string background = "#262626";
    std::string fullscreen_background = "#000000";

    // Maps: where a GPS location link goes; {lat} and {lon} are substituted.
    std::string map_url = "https://www.google.com/maps?q={lat},{lon}";
    std::string map_link(double latitude, double longitude) const;

    // Mouse
    Bindings controls = default_bindings();
    Action control(Control c) const { return controls[size_t(c)]; }

    static std::string path();
    void load();                           // missing file or keys leave the defaults
    bool save() const;
    void reset_extensions();
    bool accepts_extension(const std::string& ext) const;   // ext lowercase, no dot
};
