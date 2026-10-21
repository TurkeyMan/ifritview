#include "settings.h"

#include "config.h"
#include "controls.h"
#include "decoders/decoder.h"
#include "geocode.h"
#include "integration.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace L = anyui::layout;
using anyui::Color;
using anyui::Extent;

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

Extent chars(float n)
{
    return Extent{n, Extent::Unit::Chars};
}

Color parse_colour(const std::string& hex)
{
    unsigned v = 0;
    const char* p = hex.c_str();
    if (*p == '#')
        ++p;
    if (std::sscanf(p, "%6x", &v) != 1)
        return Color{0, 0, 0, 255};
    return Color{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), 255};
}

std::string colour_hex(Color c)
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
    std::unique_ptr<anyui::Window> window;

    anyui::Check *fit_window = nullptr, *wrap_around = nullptr, *show_hidden = nullptr, *raw_crop = nullptr, *apply_xmp = nullptr;
    anyui::Spin *min_width = nullptr, *min_height = nullptr, *prefetch_ahead = nullptr, *prefetch_behind = nullptr, *cache_mb = nullptr;
    anyui::Select *raw_mode = nullptr, *map_provider = nullptr, *geo_provider = nullptr;
    anyui::Entry *map_url = nullptr, *geo_url = nullptr;

    anyui::Check *smooth_upscale = nullptr, *smooth_downscale = nullptr, *fullscreen_upscale = nullptr, *checkerboard = nullptr, *animate = nullptr;
    anyui::Check *video_autoplay = nullptr, *video_muted = nullptr, *video_loop = nullptr;
    anyui::Spin *ss_seconds = nullptr, *ss_zoom = nullptr, *ss_fade = nullptr;
    anyui::Check *ss_motion = nullptr, *ss_shuffle = nullptr;
    anyui::ColorButton *background = nullptr, *fullscreen_background = nullptr;
    anyui::Check *ov_enabled = nullptr, *ov_file = nullptr, *ov_format = nullptr, *ov_shot = nullptr, *ov_location = nullptr, *ov_view = nullptr,
                 *ov_windowed = nullptr;
    anyui::ColorButton* ov_color = nullptr;
    anyui::Spin* ov_font = nullptr;
    anyui::Widget* overlay_details = nullptr;

    std::array<anyui::Select*, size_t(Control::Count)> controls{};
    std::array<anyui::Select*, size_t(Control::Count)> fullscreen_controls{};

    anyui::TextView* extensions = nullptr;
    anyui::Check *default_videos = nullptr, *default_documents = nullptr;
    anyui::Label* default_result = nullptr;
    anyui::Tabs* tabs = nullptr;

    std::vector<std::string> geo_urls;  // per geo_provider item: "off", provider urls, "" = custom

    void apply()
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
        cfg.extensions = parse_extensions(extensions->text());
        const std::string url = map_url->text();
        if (url.find("{lat}") != std::string::npos && url.find("{lon}") != std::string::npos)
            cfg.map_url = url;
        const int geo = geo_provider->current();
        const std::string geo_text = geo_url->text();
        if (geo >= 0 && geo_urls[size_t(geo)] == "off")
            cfg.geocoder_url.clear();
        else if (geo_text.find("{lat}") != std::string::npos && geo_text.find("{lon}") != std::string::npos)
            cfg.geocoder_url = geo_text;
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

anyui::Node general_page(const Config& cfg, State& st)
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
    return L::Scroll{L::VBox{
        L::Group{"Window",
                 L::Form{
                     {"", L::Check{"Resize the window to fit each image", cfg.fit_window_to_image}.bind(st.fit_window)},
                     {"Min window width", L::Spin{200, 8192, double(cfg.min_window_width), 10}.bind(st.min_width)},
                     {"Min window height", L::Spin{150, 8192, double(cfg.min_window_height), 10}.bind(st.min_height)},
                 }},
        L::Group{"Browsing",
                 L::Form{
                     {"", L::Check{"Wrap around at the end of the folder", cfg.wrap_around}.bind(st.wrap_around)},
                     {"", L::Check{"Include hidden files", cfg.show_hidden}.bind(st.show_hidden)},
                     {"Preload images ahead", L::Spin{0, 16, double(cfg.prefetch_ahead)}.bind(st.prefetch_ahead)},
                     {"Preload images behind", L::Spin{0, 16, double(cfg.prefetch_behind)}.bind(st.prefetch_behind)},
                     {"Image cache (MB, 0 = auto)",
                      L::Spin{0, 65536, double(cfg.cache_megabytes), 256}.tooltip("0 = automatic: a quarter of the memory free at startup").bind(st.cache_mb)},
                 }},
        L::Group{"Camera RAW",
                 L::Form{
                     {"Decode", L::Select{{"Full resolution", "Half size (fast, bins 2×2 photosites)", "Embedded JPEG only (never develop)"}, raw_index}.bind(
                                    st.raw_mode)},
                     {"", L::Check{"Frame as the camera did (crop the sensor margins the camera never shows)", cfg.raw_camera_crop}.bind(st.raw_crop)},
                     {"", L::Check{"Apply edits from XMP sidecars (exposure, crop, rotation)", cfg.apply_xmp_edits}.bind(st.apply_xmp)},
                 }},
        L::Group{"Maps",
                 L::Form{
                     {"Open locations in", L::Select{maps, map_index}
                                               .onChanged(
                                                   [s](int i)
                                                   {
                                                       const bool custom = i < 0 || i >= int(std::size(kMapProviders));
                                                       if (!custom)
                                                           s->map_url->setText(kMapProviders[i].url);
                                                       s->map_url->setEnabled(custom);
                                                   })
                                               .bind(st.map_provider)},
                     {"URL template",
                      L::Entry{cfg.map_url}.tooltip("URL template; {lat} and {lon} are replaced with the coordinates").bind(st.map_url).width(chars(44))},
                     {"Look up place names with", L::Select{geos, geo_index}
                                                      .onChanged(
                                                          [s](int i)
                                                          {
                                                              if (i < 0)
                                                                  return;
                                                              const std::string& url = s->geo_urls[size_t(i)];
                                                              if (url == "off")
                                                                  s->geo_url->setText("");
                                                              else if (!url.empty())
                                                                  s->geo_url->setText(url);
                                                              s->geo_url->setEnabled(url.empty());
                                                          })
                                                      .bind(st.geo_provider)},
                     {"Lookup URL template", L::Entry{cfg.geocoder_url}
                                                 .tooltip("Reverse-geocoding URL template returning JSON; {lat} and {lon} are replaced")
                                                 .bind(st.geo_url)
                                                 .width(chars(44))},
                 }},
    }};
}

anyui::Node viewing_page(const Config& cfg, State& st)
{
    State* s = &st;
    return L::Scroll{L::VBox{
        L::Group{"Scaling",
                 L::Form{
                     {"", L::Check{"Smooth when zoomed in", cfg.smooth_upscale}.bind(st.smooth_upscale)},
                     {"", L::Check{"Smooth when zoomed out", cfg.smooth_downscale}.bind(st.smooth_downscale)},
                     {"", L::Check{"In fullscreen, enlarge small images to fill the screen", cfg.fullscreen_upscale}.bind(st.fullscreen_upscale)},
                 }},
        L::Group{"Background",
                 L::Form{
                     {"Window", L::ColorButton{parse_colour(cfg.background), "Window background"}.bind(st.background)},
                     {"Fullscreen", L::ColorButton{parse_colour(cfg.fullscreen_background), "Fullscreen background"}.bind(st.fullscreen_background)},
                     {"", L::Check{"Show transparency as a checkerboard", cfg.checkerboard}.bind(st.checkerboard)},
                     {"", L::Check{"Play animated GIF and WebP (A toggles)", cfg.animate}.bind(st.animate)},
                 }},
        L::Group{"Video",
                 L::Form{
                     {"", L::Check{"Play videos as soon as they are shown (Space toggles)", cfg.video_autoplay}.bind(st.video_autoplay)},
                     {"", L::Check{"Start muted (M toggles)", cfg.video_muted}.bind(st.video_muted)},
                     {"", L::Check{"Loop", cfg.video_loop}.bind(st.video_loop)},
                 }},
        L::Group{"Slideshow",
                 L::Form{
                     {"Seconds per image", L::Spin{0.5, 600, cfg.slideshow_seconds, 0.5, 1}.bind(st.ss_seconds)},
                     {"", L::Check{"Gentle pan and zoom on each image", cfg.slideshow_motion}.bind(st.ss_motion)},
                     {"Motion amount (%)", L::Spin{0, 50, cfg.slideshow_zoom * 100}.bind(st.ss_zoom)},
                     {"", L::Check{"Shuffle", cfg.slideshow_shuffle}.bind(st.ss_shuffle)},
                     {"Cross-fade (ms, 0 = cut)", L::Spin{0, 5000, double(cfg.slideshow_crossfade_ms), 100}.bind(st.ss_fade)},
                 }},
        L::Group{"Fullscreen info overlay",
                 L::VBox{
                     L::Check{"Show the overlay (I toggles it in fullscreen)", cfg.overlay_enabled}
                         .onToggled([s](bool on) { s->overlay_details->setEnabled(on); })
                         .bind(st.ov_enabled),
                     L::Form{
                         {"", L::Check{"File: name, position in folder, size, date", cfg.overlay_file}.bind(st.ov_file)},
                         {"", L::Check{"Format: dimensions, depth, codec details, colour profile", cfg.overlay_format}.bind(st.ov_format)},
                         {"", L::Check{"Shot: camera, lens, aperture, shutter, ISO, focal length", cfg.overlay_shot}.bind(st.ov_shot)},
                         {"", L::Check{"Location", cfg.overlay_location}.bind(st.ov_location)},
                         {"", L::Check{"View: zoom, raw state, exposure", cfg.overlay_view}.bind(st.ov_view)},
                         {"", L::Check{"Also show in the window, not only in fullscreen", cfg.overlay_windowed}.bind(st.ov_windowed)},
                         {"Text colour", L::ColorButton{parse_colour(cfg.overlay_color), "Overlay text"}.bind(st.ov_color)},
                         {"Text size (pt)", L::Spin{6, 40, double(cfg.overlay_font_pt)}.bind(st.ov_font)},
                     }
                         .handle(st.overlay_details),
                 }},
    }};
}

anyui::Node controls_page(const Config& cfg, State& st)
{
    // One row per control: windowed and fullscreen actions side by side.
    anyui::Nodes rows;
    std::vector<std::string> form_labels;
    form_labels.push_back("");
    rows.push_back(L::HBox{L::Label{"Windowed"}.width(chars(30)), L::Label{"Fullscreen"}});
    for (const ControlInfo& c : kControls)
    {
        const std::vector<Action> acts = actions_for(c.kind);
        const std::vector<std::string> items = labels(acts);
        form_labels.push_back(c.label);
        rows.push_back(L::HBox{
            L::Select{items, index_of(acts, cfg.control(c.id, false))}.bind(st.controls[size_t(c.id)]).width(chars(30)),
            L::Select{items, index_of(acts, cfg.control(c.id, true))}.bind(st.fullscreen_controls[size_t(c.id)]).width(chars(30)),
        });
    }
    anyui::spec::Form form{form_labels, rows};
    State* s = &st;
    return L::Scroll{L::VBox{
        anyui::Node{form, {}},
        L::HBox{
            L::Spacer{},
            L::Button{"Reset to defaults"}.onClick(
                [s]
                {
                    const Bindings d = default_bindings(), f = default_fullscreen_bindings();
                    for (const ControlInfo& c : kControls)
                    {
                        const std::vector<Action> acts = actions_for(c.kind);
                        s->controls[size_t(c.id)]->setCurrent(index_of(acts, d[size_t(c.id)]));
                        s->fullscreen_controls[size_t(c.id)]->setCurrent(index_of(acts, f[size_t(c.id)]));
                    }
                }),
        },
    }};
}

anyui::Node file_types_page(const Config& cfg, State& st)
{
    State* s = &st;
    return L::VBox{
        L::Label{"File extensions to include when browsing a folder, separated by spaces:"},
        L::TextView{join_extensions(cfg.extensions)}.monospace().bind(st.extensions).stretch(),
        L::HBox{
            L::Button{"Make IfritView the default viewer for these types"}.onClick(
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
                    s->default_result->setText(msg);
                }),
            L::Spacer{},
            L::Button{"Reset to all supported formats"}.onClick([s] { s->extensions->setText(join_extensions(default_extensions())); }),
        },
        L::HBox{
            L::Check{"Also for videos"}.tooltip("Leave unticked to keep your current video player for mp4, mov, mkv…").bind(st.default_videos),
            L::Check{"Also for PDFs"}.tooltip("Leave unticked to keep your current PDF reader").bind(st.default_documents),
        },
        L::Label{""}.bind(st.default_result),
    };
}

}  // namespace

void open_settings_dialog(anyui::Ui& ui, anyui::Window* parent, Config& cfg, std::function<void(bool)> done, int page)
{
    auto st = std::make_shared<State>(cfg, std::move(done));
    st->window = ui.create(L::Dialog{"Settings",
                                     {L::VBox{
                                         L::Tabs{
                                             {"General", general_page(cfg, *st)},
                                             {"Viewing", viewing_page(cfg, *st)},
                                             {"Controls", controls_page(cfg, *st)},
                                             {"File Types", file_types_page(cfg, *st)},
                                         }
                                             .bind(st->tabs)
                                             .stretch(),
                                         L::Buttons{L::Cancel, L::Ok},
                                     }}}
                               .size(chars(90), Extent{40, Extent::Unit::Lines}),
                           parent);
    // Initial enabled states that follow other fields.
    st->map_url->setEnabled(st->map_provider->current() >= int(std::size(kMapProviders)));
    st->geo_url->setEnabled(st->geo_urls[size_t(std::max(0, st->geo_provider->current()))].empty());
    st->overlay_details->setEnabled(cfg.overlay_enabled);
    st->tabs->setCurrent(page);
    st->window->onFinished(
        [st, &ui](anyui::DialogRole role)
        {
            const bool ok = role == anyui::DialogRole::Ok;
            if (ok)
                st->apply();
            if (st->done)
                st->done(ok);
            // Not from inside the window's own callback: destroy it afterwards.
            ui.post([st] { st->window.reset(); });
        });
    st->window->show();
}
