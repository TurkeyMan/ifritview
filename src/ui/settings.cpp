#include "settings.h"

#include "config.h"
#include "controls.h"
#include "decoders/decoder.h"
#include "geocode.h"
#include "integration.h"
#include "plugins.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace L = rap::layout;

namespace
{

const struct
{
    const char* name;
    const char* url;
} kMapProviders[] = {
    {"Google Maps", "https://www.google.com/maps?q={lat},{lon}"},
    {"OpenStreetMap", "https://www.openstreetmap.org/?mlat={lat}&mlon={lon}#map=16/{lat}/{lon}"},
    {"Apple Maps", "https://maps.apple.com/?ll={lat},{lon}&q={lat},{lon}"},
    {"Bing Maps", "https://www.bing.com/maps?cp={lat}~{lon}&lvl=16&sp=point.{lat}_{lon}_Photo"},
};

const char* const kRawModes[] = {"full", "half", "preview"};

rap::extent chars(float n)
{
    return rap::extent{n, rap::extent::unit::chars};
}

rap::color parse_colour(const std::string& hex)
{
    unsigned v = 0;
    const char* p = hex.c_str();
    if (*p == '#')
        ++p;
    if (std::sscanf(p, "%6x", &v) != 1)
        return rap::color{0, 0, 0, 255};
    return rap::color{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), 255};
}

std::string colour_hex(rap::color c)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string join_extensions(const std::vector<std::string>& exts)
{
    std::string s;
    for (const std::string& e : exts)
        s += (s.empty() ? "" : " ") + e;
    return s;
}

std::vector<std::string> parse_extensions(const std::string& text)
{
    std::vector<std::string> out;
    std::string token;
    auto flush = [&]
    {
        if (!token.empty() && std::find(out.begin(), out.end(), token) == out.end())
            out.push_back(token);
        token.clear();
    };
    for (unsigned char c : text)
    {
        if (std::isspace(c) || c == ',' || c == ';' || c == '.' || c == '*')
            flush();
        else
            token += char(std::tolower(c));
    }
    flush();
    return out;
}

// The actions a control of `kind` can take, in table order.
std::vector<Action> actions_for(Kind kind)
{
    std::vector<Action> out;
    for (const ActionInfo& a : kActions)
        if (action_fits(a.id, kind))
            out.push_back(a.id);
    return out;
}

std::vector<std::string> labels(const std::vector<Action>& actions)
{
    std::vector<std::string> out;
    for (Action a : actions)
        out.push_back(action_info(a).label);
    return out;
}

int index_of(const std::vector<Action>& actions, Action a)
{
    auto it = std::find(actions.begin(), actions.end(), a);
    return it == actions.end() ? 0 : int(it - actions.begin());
}

// Everything the dialog reads back on OK; lives as long as the dialog.
struct State
{
    State(Config& c, std::function<void(bool)> d) : cfg(c), done(std::move(d)) {}
    Config& cfg;
    std::function<void(bool)> done;
    std::unique_ptr<rap::window> window;
    bool finished = false;

    rap::check *fit_window = nullptr, *wrap_around = nullptr, *show_hidden = nullptr, *raw_crop = nullptr, *apply_xmp = nullptr;
    rap::spin *min_width = nullptr, *min_height = nullptr, *prefetch_ahead = nullptr, *prefetch_behind = nullptr, *cache_mb = nullptr;
    rap::drop_down *raw_mode = nullptr, *map_provider = nullptr, *geo_provider = nullptr;
    rap::entry *map_url = nullptr, *geo_url = nullptr;

    rap::check *smooth_upscale = nullptr, *smooth_downscale = nullptr, *fullscreen_upscale = nullptr, *checkerboard = nullptr, *animate = nullptr;
    rap::check *video_autoplay = nullptr, *video_muted = nullptr, *video_loop = nullptr;
    rap::spin *ss_seconds = nullptr, *ss_zoom = nullptr, *ss_fade = nullptr;
    rap::check *ss_motion = nullptr, *ss_shuffle = nullptr;
    rap::color_button *background = nullptr, *fullscreen_background = nullptr;
    rap::check *ov_enabled = nullptr, *ov_file = nullptr, *ov_format = nullptr, *ov_shot = nullptr, *ov_location = nullptr, *ov_view = nullptr,
                 *ov_windowed = nullptr;
    rap::color_button* ov_color = nullptr;
    rap::spin* ov_font = nullptr;
    rap::widget* overlay_details = nullptr;

    std::array<rap::drop_down*, size_t(Control::Count)> controls{};
    std::array<rap::drop_down*, size_t(Control::Count)> fullscreen_controls{};

    rap::text_view* extensions = nullptr;
    rap::check *default_videos = nullptr, *default_documents = nullptr;
    rap::label* default_result = nullptr;
    rap::tabs* tabs = nullptr;

    std::vector<std::string> geo_urls;  // per geo_provider item: "off", provider urls, "" = custom

    // Pages are built when first shown: only those can have changed.
    void apply()
    {
        if (fit_window)
            apply_general();
        if (smooth_upscale)
            apply_viewing();
        if (controls[0])
            apply_controls();
        if (extensions)
            cfg.extensions = parse_extensions(extensions->text());
    }

    void apply_general()
    {
        cfg.fit_window_to_image = fit_window->checked();
        cfg.min_window_width = int(min_width->value());
        cfg.min_window_height = int(min_height->value());
        cfg.wrap_around = wrap_around->checked();
        cfg.show_hidden = show_hidden->checked();
        cfg.prefetch_ahead = int(prefetch_ahead->value());
        cfg.prefetch_behind = int(prefetch_behind->value());
        cfg.cache_megabytes = int(cache_mb->value());
        if (raw_mode->current() >= 0)
            cfg.raw_mode = kRawModes[raw_mode->current()];
        cfg.raw_camera_crop = raw_crop->checked();
        cfg.apply_xmp_edits = apply_xmp->checked();
        const std::string url = map_url->text();
        if (url.find("{lat}") != std::string::npos && url.find("{lon}") != std::string::npos)
            cfg.map_url = url;
        const int geo = geo_provider->current();
        const std::string geo_text = geo_url->text();
        if (geo >= 0 && geo_urls[size_t(geo)] == "off")
            cfg.geocoder_url.clear();
        else if (geo_text.find("{lat}") != std::string::npos && geo_text.find("{lon}") != std::string::npos)
            cfg.geocoder_url = geo_text;
    }

    void apply_viewing()
    {
        cfg.smooth_upscale = smooth_upscale->checked();
        cfg.smooth_downscale = smooth_downscale->checked();
        cfg.fullscreen_upscale = fullscreen_upscale->checked();
        cfg.checkerboard = checkerboard->checked();
        cfg.animate = animate->checked();
        cfg.video_autoplay = video_autoplay->checked();
        cfg.video_muted = video_muted->checked();
        cfg.video_loop = video_loop->checked();
        cfg.slideshow_seconds = ss_seconds->value();
        cfg.slideshow_motion = ss_motion->checked();
        cfg.slideshow_zoom = ss_zoom->value() / 100.0;
        cfg.slideshow_shuffle = ss_shuffle->checked();
        cfg.slideshow_crossfade_ms = int(ss_fade->value());
        cfg.background = colour_hex(background->color());
        cfg.fullscreen_background = colour_hex(fullscreen_background->color());
        cfg.overlay_enabled = ov_enabled->checked();
        cfg.overlay_file = ov_file->checked();
        cfg.overlay_format = ov_format->checked();
        cfg.overlay_shot = ov_shot->checked();
        cfg.overlay_location = ov_location->checked();
        cfg.overlay_view = ov_view->checked();
        cfg.overlay_windowed = ov_windowed->checked();
        cfg.overlay_color = colour_hex(ov_color->color());
        cfg.overlay_font_pt = int(ov_font->value());
    }

    void apply_controls()
    {
        for (const ControlInfo& c : kControls)
        {
            const std::vector<Action> acts = actions_for(c.kind);
            const int w = controls[size_t(c.id)]->current(), f = fullscreen_controls[size_t(c.id)]->current();
            if (w >= 0)
                cfg.controls[size_t(c.id)] = acts[size_t(w)];
            if (f >= 0)
                cfg.fullscreen_controls[size_t(c.id)] = acts[size_t(f)];
        }
    }
};

rap::node general_page(const Config& cfg, State& st)
{
    std::vector<std::string> maps;
    int map_index = int(std::size(kMapProviders));  // Custom
    for (size_t i = 0; i < std::size(kMapProviders); ++i)
    {
        maps.push_back(kMapProviders[i].name);
        if (cfg.map_url == kMapProviders[i].url)
            map_index = int(i);
    }
    maps.push_back("Custom");

    std::vector<std::string> geos{"Off"};
    st.geo_urls = {"off"};
    size_t n = 0;
    const Geocoder::Provider* providers = Geocoder::providers(n);
    int geo_index = cfg.geocoder_url.empty() ? 0 : -1;
    for (size_t i = 0; i < n; ++i)
    {
        geos.push_back(providers[i].name);
        st.geo_urls.push_back(providers[i].url);
        if (cfg.geocoder_url == providers[i].url)
            geo_index = int(i + 1);
    }
    geos.push_back("Custom");
    st.geo_urls.push_back("");
    if (geo_index < 0)
        geo_index = int(geos.size() - 1);

    int raw_index = 0;
    for (int i = 0; i < 3; ++i)
        if (cfg.raw_mode == kRawModes[i])
            raw_index = i;

    State* s = &st;
    return L::scroll{L::vbox{
        L::group{"Window",
                 L::form{
                     {"", L::check{"Resize the window to fit each image", cfg.fit_window_to_image}.bind(st.fit_window)},
                     {"Min window width", L::spin{200, 8192, double(cfg.min_window_width), 10}.bind(st.min_width)},
                     {"Min window height", L::spin{150, 8192, double(cfg.min_window_height), 10}.bind(st.min_height)},
                 }},
        L::group{"Browsing",
                 L::form{
                     {"", L::check{"Wrap around at the end of the folder", cfg.wrap_around}.bind(st.wrap_around)},
                     {"", L::check{"Include hidden files", cfg.show_hidden}.bind(st.show_hidden)},
                     {"Preload images ahead", L::spin{0, 16, double(cfg.prefetch_ahead)}.bind(st.prefetch_ahead)},
                     {"Preload images behind", L::spin{0, 16, double(cfg.prefetch_behind)}.bind(st.prefetch_behind)},
                     {"Image cache (MB, 0 = auto)",
                      L::spin{0, 65536, double(cfg.cache_megabytes), 256}.tooltip("0 = automatic: a quarter of the memory free at startup").bind(st.cache_mb)},
                 }},
        L::group{"Camera RAW",
                 L::form{
                     {"Decode", L::drop_down{{"Full resolution", "Half size (fast, bins 2×2 photosites)", "Embedded JPEG only (never develop)"}, raw_index}.bind(
                                    st.raw_mode)},
                     {"", L::check{"Frame as the camera did (crop the sensor margins the camera never shows)", cfg.raw_camera_crop}.bind(st.raw_crop)},
                     {"", L::check{"Apply edits from XMP sidecars (exposure, crop, rotation)", cfg.apply_xmp_edits}.bind(st.apply_xmp)},
                 }},
        L::group{"Maps",
                 L::form{
                     {"Open locations in", L::drop_down{maps, map_index}
                                               .on_changed(
                                                   [s](int i)
                                                   {
                                                       const bool custom = i < 0 || i >= int(std::size(kMapProviders));
                                                       if (!custom)
                                                           s->map_url->set_text(kMapProviders[i].url);
                                                       s->map_url->set_enabled(custom);
                                                   })
                                               .bind(st.map_provider)},
                     {"URL template",
                      L::entry{cfg.map_url}
                          .tooltip("URL template; {lat} and {lon} are replaced with the coordinates")
                          .enabled(map_index >= int(std::size(kMapProviders)))
                          .bind(st.map_url)
                          .width(chars(44))},
                     {"Look up place names with", L::drop_down{geos, geo_index}
                                                      .on_changed(
                                                          [s](int i)
                                                          {
                                                              if (i < 0)
                                                                  return;
                                                              const std::string& url = s->geo_urls[size_t(i)];
                                                              if (url == "off")
                                                                  s->geo_url->set_text("");
                                                              else if (!url.empty())
                                                                  s->geo_url->set_text(url);
                                                              s->geo_url->set_enabled(url.empty());
                                                          })
                                                      .bind(st.geo_provider)},
                     {"Lookup URL template", L::entry{cfg.geocoder_url}
                                                 .tooltip("Reverse-geocoding URL template returning JSON; {lat} and {lon} are replaced")
                                                 .enabled(st.geo_urls[size_t(geo_index)].empty())
                                                 .bind(st.geo_url)
                                                 .width(chars(44))},
                 }},
    }};
}

rap::node viewing_page(const Config& cfg, State& st)
{
    State* s = &st;
    return L::scroll{L::vbox{
        L::group{"Scaling",
                 L::form{
                     {"", L::check{"Smooth when zoomed in", cfg.smooth_upscale}.bind(st.smooth_upscale)},
                     {"", L::check{"Smooth when zoomed out", cfg.smooth_downscale}.bind(st.smooth_downscale)},
                     {"", L::check{"In fullscreen, enlarge small images to fill the screen", cfg.fullscreen_upscale}.bind(st.fullscreen_upscale)},
                 }},
        L::group{"Background",
                 L::form{
                     {"Window", L::color_button{parse_colour(cfg.background), "Window background"}.bind(st.background)},
                     {"Fullscreen", L::color_button{parse_colour(cfg.fullscreen_background), "Fullscreen background"}.bind(st.fullscreen_background)},
                     {"", L::check{"Show transparency as a checkerboard", cfg.checkerboard}.bind(st.checkerboard)},
                     {"", L::check{"Play animated GIF and WebP (A toggles)", cfg.animate}.bind(st.animate)},
                 }},
        L::group{"Video",
                 L::form{
                     {"", L::check{"Play videos as soon as they are shown (Space toggles)", cfg.video_autoplay}.bind(st.video_autoplay)},
                     {"", L::check{"Start muted (M toggles)", cfg.video_muted}.bind(st.video_muted)},
                     {"", L::check{"Loop", cfg.video_loop}.bind(st.video_loop)},
                 }},
        L::group{"Slideshow",
                 L::form{
                     {"Seconds per image", L::spin{0.5, 600, cfg.slideshow_seconds, 0.5, 1}.bind(st.ss_seconds)},
                     {"", L::check{"Gentle pan and zoom on each image", cfg.slideshow_motion}.bind(st.ss_motion)},
                     {"Motion amount (%)", L::spin{0, 50, cfg.slideshow_zoom * 100}.bind(st.ss_zoom)},
                     {"", L::check{"Shuffle", cfg.slideshow_shuffle}.bind(st.ss_shuffle)},
                     {"Cross-fade (ms, 0 = cut)", L::spin{0, 5000, double(cfg.slideshow_crossfade_ms), 100}.bind(st.ss_fade)},
                 }},
        L::group{"Fullscreen info overlay",
                 L::vbox{
                     L::check{"Show the overlay (I toggles it in fullscreen)", cfg.overlay_enabled}
                         .on_toggled([s](bool on) { s->overlay_details->set_enabled(on); })
                         .bind(st.ov_enabled),
                     L::form{
                         {"", L::check{"File: name, position in folder, size, date", cfg.overlay_file}.bind(st.ov_file)},
                         {"", L::check{"Format: dimensions, depth, codec details, colour profile", cfg.overlay_format}.bind(st.ov_format)},
                         {"", L::check{"Shot: camera, lens, aperture, shutter, ISO, focal length", cfg.overlay_shot}.bind(st.ov_shot)},
                         {"", L::check{"Location", cfg.overlay_location}.bind(st.ov_location)},
                         {"", L::check{"View: zoom, raw state, exposure", cfg.overlay_view}.bind(st.ov_view)},
                         {"", L::check{"Also show in the window, not only in fullscreen", cfg.overlay_windowed}.bind(st.ov_windowed)},
                         {"Text colour", L::color_button{parse_colour(cfg.overlay_color), "Overlay text"}.bind(st.ov_color)},
                         {"Text size (pt)", L::spin{6, 40, double(cfg.overlay_font_pt)}.bind(st.ov_font)},
                     }
                         .enabled(cfg.overlay_enabled)
                         .handle(st.overlay_details),
                 }},
    }};
}

rap::node controls_page(const Config& cfg, State& st)
{
    // One row per control: windowed and fullscreen actions side by side.
    rap::nodes rows;
    std::vector<std::string> form_labels;
    // A blank label, not an empty one (which would span both columns), so
    // the headings sit over the fields.
    form_labels.push_back(" ");
    // Wide enough for the longest action: GTK widens a drop-down to fit its
    // choices, so a narrower one would push the second column out of line.
    constexpr float column = 36;
    rows.push_back(L::hbox{L::label{"Windowed"}.width(chars(column)), L::label{"Fullscreen"}});
    for (const ControlInfo& c : kControls)
    {
        const std::vector<Action> acts = actions_for(c.kind);
        const std::vector<std::string> items = labels(acts);
        form_labels.push_back(c.label);
        rows.push_back(L::hbox{
            L::drop_down{items, index_of(acts, cfg.control(c.id, false))}.bind(st.controls[size_t(c.id)]).width(chars(column)),
            L::drop_down{items, index_of(acts, cfg.control(c.id, true))}.bind(st.fullscreen_controls[size_t(c.id)]).width(chars(column)),
        });
    }
    rap::spec::form form{form_labels, rows};
    State* s = &st;
    return L::scroll{L::vbox{
        rap::node{form, {}},
        L::label{"While selecting a region, hold Shift for a square or Ctrl for the image's own shape."},
        L::hbox{
            L::spacer{},
            L::button{"Reset to defaults"}.on_click(
                [s]
                {
                    const Bindings d = default_bindings(), f = default_fullscreen_bindings();
                    for (const ControlInfo& c : kControls)
                    {
                        const std::vector<Action> acts = actions_for(c.kind);
                        s->controls[size_t(c.id)]->set_current(index_of(acts, d[size_t(c.id)]));
                        s->fullscreen_controls[size_t(c.id)]->set_current(index_of(acts, f[size_t(c.id)]));
                    }
                }),
        },
    }};
}

// The format plugins in use, a line for the File Types page.
std::string plugin_list()
{
    if (!plugins::ready())
        return "Format plugins: still loading.";
    std::string text;
    for (const plugins::PluginSummary& p : plugins::loaded())
    {
        std::string exts;
        for (const plugins::FormatSummary& f : p.formats)
            for (const std::string& e : f.extensions)
                exts += (exts.empty() ? "" : " ") + e;
        text += (text.empty() ? "" : "; ") + p.name + (p.version.empty() ? "" : " " + p.version) + " (" + exts + ")";
    }
    const size_t failed = plugins::failures().size();
    if (text.empty())
        text = "none";
    if (failed)
        text += "; " + std::to_string(failed) + " not loaded (see ifritview --plugins)";
    return "Format plugins: " + text + ".";
}

rap::node file_types_page(const Config& cfg, State& st)
{
    State* s = &st;
    return L::vbox{
        L::label{"File extensions to include when browsing a folder, separated by spaces:"},
        L::text_view{join_extensions(cfg.extensions)}.monospace().bind(st.extensions).stretch(),
        L::hbox{
            L::button{"Make IfritView the default viewer for these types"}.on_click(
                [s]
                {
                    std::vector<std::string> skipped;
                    std::string error;
                    const bool videos = s->default_videos->checked(), documents = s->default_documents->checked();
                    const int n = set_default_for_extensions(parse_extensions(s->extensions->text()), videos, documents, skipped, error);
                    std::string msg = n ? "IfritView is now the default for " + std::to_string(n) + " file types." : "No file types were set.";
                    if (!videos)
                        msg += " Videos were left alone.";
                    if (!skipped.empty())
                    {
                        msg += " Unknown to the system: ";
                        for (size_t i = 0; i < skipped.size(); ++i)
                            msg += (i ? ", " : "") + skipped[i];
                        msg += ".";
                    }
                    if (!error.empty())
                        msg += " Error: " + error;
                    s->default_result->set_text(msg);
                }),
            L::spacer{},
            L::button{"Reset to all supported formats"}.on_click([s] { s->extensions->set_text(join_extensions(default_extensions())); }),
        },
        L::hbox{
            L::check{"Also for videos"}.tooltip("Leave unticked to keep your current video player for mp4, mov, mkv…").bind(st.default_videos),
            L::check{"Also for PDFs"}.tooltip("Leave unticked to keep your current PDF reader").bind(st.default_documents),
        },
        L::label{""}.bind(st.default_result),
        L::label{plugin_list()},
    };
}

}  // namespace

std::function<void(bool)> open_settings_dialog(rap::ui& ui, rap::window* parent, Config& cfg, std::function<void(bool)> done, int page)
{
    const bool trace = std::getenv("IFRIT_TRACE");
    const auto t0 = std::chrono::steady_clock::now();
    auto t = t0;
    const auto lap = [&](const char* what)
    {
        const auto now = std::chrono::steady_clock::now();
        if (trace)
            std::fprintf(stderr, "settings: %-8s %6.1f ms\n", what, std::chrono::duration<double, std::milli>(now - t).count());
        t = now;
    };
    auto st = std::make_shared<State>(cfg, std::move(done));
    // Each page is built the first time it's shown (the window owns them and
    // goes before the state does).
    State* s = st.get();
    st->window = ui.create(L::dialog{"Settings",
                                     {L::vbox{
                                         L::tabs{
                                             {"General", L::lazy{[&cfg, s] { return general_page(cfg, *s); }}},
                                             {"Viewing", L::lazy{[&cfg, s] { return viewing_page(cfg, *s); }}},
                                             {"Controls", L::lazy{[&cfg, s] { return controls_page(cfg, *s); }}},
                                             {"File Types", L::lazy{[&cfg, s] { return file_types_page(cfg, *s); }}},
                                         }
                                             .sidebar()
                                             .bind(st->tabs)
                                             .stretch(),
                                         L::buttons{L::cancel, L::ok},
                                     }}}
                               .size(chars(116), rap::extent{40, rap::extent::unit::lines}),
                           parent);
    lap("create");
    st->tabs->set_current(page);
    // Once, whichever way it ends.
    auto finish = [st, &ui](bool ok)
    {
        if (!st->window || st->finished)
            return;
        st->finished = true;
        if (ok)
            st->apply();
        if (st->done)
            st->done(ok);
        st->window->close();
        // Not from inside the window's own callback: destroy it afterwards.
        ui.post([st] { st->window.reset(); });
    };
    st->window->on_finished([finish](rap::dialog_role role) { finish(role == rap::dialog_role::ok); });
    st->window->show();
    lap("show");
    if (trace)  // the first paint has happened by the time the loop gets here
        ui.post(
            [t0]
            {
                std::fprintf(stderr, "settings: on screen %6.1f ms after the click\n",
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            });
    return finish;
}
