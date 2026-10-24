#include "settings.h"

#include "config.h"
#include "controls.h"
#include "decoders/decoder.h"
#include "geocode.h"
#include "i18n.h"
#include "integration.h"
#include "platform.h"
#include "plugins.h"

#include <gio/gio.h>
#include <rapport/sys.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
        out.push_back(tr_ifrit(action_info(a).label));
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
    rap::drop_down *raw_mode = nullptr, *map_provider = nullptr, *geo_provider = nullptr, *language = nullptr;
    std::vector<std::string> language_tags;  // the language choice's, "" for the system's
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
        if (const int i = language->current(); i >= 0 && size_t(i) < language_tags.size())
            cfg.language = language_tags[size_t(i)];
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
    maps.push_back(_("Custom"));

    std::vector<std::string> geos{_("Off")};
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
    geos.push_back(_("Custom"));
    st.geo_urls.push_back("");
    if (geo_index < 0)
        geo_index = int(geos.size() - 1);

    int raw_index = 0;
    for (int i = 0; i < 3; ++i)
        if (cfg.raw_mode == kRawModes[i])
            raw_index = i;

    // The translations there are, by their own names; English is the source.
    std::vector<std::string> languages{_("Automatic (the system's)"), "English"};
    st.language_tags = {"", "en"};
    if (const std::filesystem::path dir = translations_folder("ifritview"); !dir.empty())
        for (const rap::i18n::language& l : rap::i18n::available("ifritview", dir))
            if (l.tag != "en")
            {
                languages.push_back(l.name);
                st.language_tags.push_back(l.tag);
            }
    int language_index = 0;
    for (size_t i = 0; i < st.language_tags.size(); ++i)
        if (st.language_tags[i] == cfg.language)
            language_index = int(i);

    State* s = &st;
    return L::scroll{L::vbox{
        L::group{_("Language"),
                 L::form{
                     {_("Language"), L::drop_down{languages, language_index}.bind(st.language)},
                     {"", L::label{_("Takes effect when IfritView next starts.")}},
                 }},
        L::group{_("Window"),
                 L::form{
                     {"", L::check{_("Resize the window to fit each image"), cfg.fit_window_to_image}.bind(st.fit_window)},
                     {_("Min window width"), L::spin{200, 8192, double(cfg.min_window_width), 10}.bind(st.min_width)},
                     {_("Min window height"), L::spin{150, 8192, double(cfg.min_window_height), 10}.bind(st.min_height)},
                 }},
        L::group{_("Browsing"),
                 L::form{
                     {"", L::check{_("Wrap around at the end of the folder"), cfg.wrap_around}.bind(st.wrap_around)},
                     {"", L::check{_("Include hidden files"), cfg.show_hidden}.bind(st.show_hidden)},
                     {_("Preload images ahead"), L::spin{0, 16, double(cfg.prefetch_ahead)}.bind(st.prefetch_ahead)},
                     {_("Preload images behind"), L::spin{0, 16, double(cfg.prefetch_behind)}.bind(st.prefetch_behind)},
                     {_("Image cache (MB, 0 = auto)"),
                      L::spin{0, 65536, double(cfg.cache_megabytes), 256}.tooltip(_("0 = automatic: a quarter of the memory free at startup")).bind(st.cache_mb)},
                 }},
        L::group{_("Camera RAW"),
                 L::form{
                     {_("Decode"), L::drop_down{{_("Full resolution"), _("Half size (fast, bins 2×2 photosites)"), _("Embedded JPEG only (never develop)")}, raw_index}.bind(
                                    st.raw_mode)},
                     {"", L::check{_("Frame as the camera did (crop the sensor margins the camera never shows)"), cfg.raw_camera_crop}.bind(st.raw_crop)},
                     {"", L::check{_("Apply edits from XMP sidecars (exposure, crop, rotation)"), cfg.apply_xmp_edits}.bind(st.apply_xmp)},
                 }},
        L::group{_("Maps"),
                 L::form{
                     {_("Open locations in"), L::drop_down{maps, map_index}
                                               .on_changed(
                                                   [s](int i)
                                                   {
                                                       const bool custom = i < 0 || i >= int(std::size(kMapProviders));
                                                       if (!custom)
                                                           s->map_url->set_text(kMapProviders[i].url);
                                                       s->map_url->set_enabled(custom);
                                                   })
                                               .bind(st.map_provider)},
                     {_("URL template"),
                      L::entry{cfg.map_url}
                          .tooltip(_("URL template; {lat} and {lon} are replaced with the coordinates"))
                          .enabled(map_index >= int(std::size(kMapProviders)))
                          .bind(st.map_url)
                          .width(chars(44))},
                     {_("Look up place names with"), L::drop_down{geos, geo_index}
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
                     {_("Lookup URL template"), L::entry{cfg.geocoder_url}
                                                 .tooltip(_("Reverse-geocoding URL template returning JSON; {lat} and {lon} are replaced"))
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
        L::group{_("Scaling"),
                 L::form{
                     {"", L::check{_("Smooth when zoomed in"), cfg.smooth_upscale}.bind(st.smooth_upscale)},
                     {"", L::check{_("Smooth when zoomed out"), cfg.smooth_downscale}.bind(st.smooth_downscale)},
                     {"", L::check{_("In fullscreen, enlarge small images to fill the screen"), cfg.fullscreen_upscale}.bind(st.fullscreen_upscale)},
                 }},
        L::group{_("Background"),
                 L::form{
                     {_("Window"), L::color_button{parse_colour(cfg.background), _("Window background")}.bind(st.background)},
                     {_("Fullscreen"), L::color_button{parse_colour(cfg.fullscreen_background), _("Fullscreen background")}.bind(st.fullscreen_background)},
                     {"", L::check{_("Show transparency as a checkerboard"), cfg.checkerboard}.bind(st.checkerboard)},
                     {"", L::check{_("Play animated GIF and WebP (A toggles)"), cfg.animate}.bind(st.animate)},
                 }},
        L::group{_("Video"),
                 L::form{
                     {"", L::check{_("Play videos as soon as they are shown (Space toggles)"), cfg.video_autoplay}.bind(st.video_autoplay)},
                     {"", L::check{_("Start muted (M toggles)"), cfg.video_muted}.bind(st.video_muted)},
                     {"", L::check{_("Loop"), cfg.video_loop}.bind(st.video_loop)},
                 }},
        L::group{_("Slideshow"),
                 L::form{
                     {_("Seconds per image"), L::spin{0.5, 600, cfg.slideshow_seconds, 0.5, 1}.bind(st.ss_seconds)},
                     {"", L::check{_("Gentle pan and zoom on each image"), cfg.slideshow_motion}.bind(st.ss_motion)},
                     {_("Motion amount (%)"), L::spin{0, 50, cfg.slideshow_zoom * 100}.bind(st.ss_zoom)},
                     {"", L::check{_("Shuffle"), cfg.slideshow_shuffle}.bind(st.ss_shuffle)},
                     {_("Cross-fade (ms, 0 = cut)"), L::spin{0, 5000, double(cfg.slideshow_crossfade_ms), 100}.bind(st.ss_fade)},
                 }},
        L::group{_("Fullscreen info overlay"),
                 L::vbox{
                     L::check{_("Show the overlay (I toggles it in fullscreen)"), cfg.overlay_enabled}
                         .on_toggled([s](bool on) { s->overlay_details->set_enabled(on); })
                         .bind(st.ov_enabled),
                     L::form{
                         {"", L::check{_("File: name, position in folder, size, date"), cfg.overlay_file}.bind(st.ov_file)},
                         {"", L::check{_("Format: dimensions, depth, codec details, colour profile"), cfg.overlay_format}.bind(st.ov_format)},
                         {"", L::check{_("Shot: camera, lens, aperture, shutter, ISO, focal length"), cfg.overlay_shot}.bind(st.ov_shot)},
                         {"", L::check{_("Location"), cfg.overlay_location}.bind(st.ov_location)},
                         {"", L::check{_("View: zoom, raw state, exposure"), cfg.overlay_view}.bind(st.ov_view)},
                         {"", L::check{_("Also show in the window, not only in fullscreen"), cfg.overlay_windowed}.bind(st.ov_windowed)},
                         {_("Text colour"), L::color_button{parse_colour(cfg.overlay_color), _("Overlay text")}.bind(st.ov_color)},
                         {_("Text size (pt)"), L::spin{6, 40, double(cfg.overlay_font_pt)}.bind(st.ov_font)},
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
    rows.push_back(L::hbox{L::label{_("Windowed")}.width(chars(column)), L::label{_("Fullscreen")}});
    for (const ControlInfo& c : kControls)
    {
        const std::vector<Action> acts = actions_for(c.kind);
        const std::vector<std::string> items = labels(acts);
        form_labels.push_back(tr_ifrit(c.label));
        rows.push_back(L::hbox{
            L::drop_down{items, index_of(acts, cfg.control(c.id, false))}.bind(st.controls[size_t(c.id)]).width(chars(column)),
            L::drop_down{items, index_of(acts, cfg.control(c.id, true))}.bind(st.fullscreen_controls[size_t(c.id)]).width(chars(column)),
        });
    }
    rap::spec::form form{form_labels, rows};
    State* s = &st;
    return L::scroll{L::vbox{
        rap::node{form, {}},
        L::label{_("While selecting a region, hold Shift for a square or Ctrl for the image's own shape.")},
        L::hbox{
            L::spacer{},
            L::button{_("Reset to defaults")}.on_click(
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
        return _("Format plugins: still loading.");
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
        text = _("none");
    if (failed)
        text += "; " + rap::i18n::format(P_("{0} not loaded (see ifritview --plugins)", "{0} not loaded (see ifritview --plugins)", failed), failed);
    return rap::i18n::format(_("Format plugins: {0}."), text);
}

rap::node file_types_page(const Config& cfg, State& st)
{
    State* s = &st;
    return L::vbox{
        L::label{_("File extensions to include when browsing a folder, separated by spaces:")},
        L::text_view{join_extensions(cfg.extensions)}.monospace().bind(st.extensions).stretch(),
        L::hbox{
            L::button{_("Make IfritView the default viewer for these types")}.on_click(
                [s]
                {
                    std::vector<std::string> skipped;
                    std::string error;
                    const bool videos = s->default_videos->checked(), documents = s->default_documents->checked();
                    const int n = set_default_for_extensions(parse_extensions(s->extensions->text()), videos, documents, skipped, error);
                    std::string msg = n ? rap::i18n::format(P_("IfritView is now the default for {0} file type.",
                                                                 "IfritView is now the default for {0} file types.", n), n)
                                        : std::string(_("No file types were set."));
                    if (!videos)
                        msg += std::string(" ") + _("Videos were left alone.");
                    if (!skipped.empty())
                    {
                        std::string names;
                        for (size_t i = 0; i < skipped.size(); ++i)
                            names += (i ? ", " : "") + skipped[i];
                        msg += " " + rap::i18n::format(_("Unknown to the system: {0}."), names);
                    }
                    if (!error.empty())
                        msg += " " + rap::i18n::format(_("Error: {0}"), error);
                    s->default_result->set_text(msg);
                }),
            L::spacer{},
            L::button{_("Reset to all supported formats")}.on_click([s] { s->extensions->set_text(join_extensions(default_extensions())); }),
        },
        L::hbox{
            L::check{_("Also for videos")}.tooltip(_("Leave unticked to keep your current video player for mp4, mov, mkv…")).bind(st.default_videos),
            L::check{_("Also for PDFs")}.tooltip(_("Leave unticked to keep your current PDF reader")).bind(st.default_documents),
        },
        L::label{""}.bind(st.default_result),
        L::label{plugin_list()},
    };
}

// The licence notices of the code built in: beside the exe on Windows (and
// in an unpacked archive), the package's doc folder on Linux, the source's
// own in a build tree. Empty when none is there.
std::string notices_path()
{
    const std::filesystem::path exe = rap::sys::executable_path().parent_path();
    std::vector<std::filesystem::path> candidates = {exe / "THIRD-PARTY-NOTICES.txt"};
#ifdef IFRIT_DOC_DIR
    candidates.push_back(std::filesystem::path(IFRIT_DOC_DIR) / "THIRD-PARTY-NOTICES.txt");
#endif
#ifdef IFRIT_SOURCE_DIR
    candidates.push_back(std::filesystem::path(IFRIT_SOURCE_DIR) / "third_party" / "NOTICES.txt");
#endif
    std::error_code ec;
    for (const std::filesystem::path& p : candidates)
        if (std::filesystem::is_regular_file(p, ec))
            return rap::sys::path_to_utf8(p);
    return {};
}

const char* const kProjectUrl = "https://github.com/TurkeyMan/ifritview";

// GitHub's mark, from Octicons (mark-github-16, MIT): its outline on a 16 px
// grid, flattened to points (it's one shape, the cat its negative space).
rap::icon github_icon()
{
    return rap::icon::drawn(
        [](rap::painter& p, rap::size_f s)
        {
            static const rap::point_f outline[] = {
        {6.77f, 11.33f}, {6.02f, 11.18f}, {5.35f, 10.92f}, {4.75f, 10.57f}, {4.23f, 10.13f}, {3.82f, 9.61f},
        {3.51f, 9.02f}, {3.32f, 8.37f}, {3.25f, 7.67f}, {3.26f, 7.38f}, {3.30f, 7.08f}, {3.36f, 6.78f},
        {3.45f, 6.50f}, {3.56f, 6.22f}, {3.68f, 5.95f}, {3.83f, 5.71f}, {4.00f, 5.48f}, {3.93f, 5.27f},
        {3.89f, 5.01f}, {3.87f, 4.72f}, {3.87f, 4.43f}, {3.89f, 4.14f}, {3.93f, 3.86f}, {3.98f, 3.62f},
        {4.06f, 3.42f}, {4.31f, 3.41f}, {4.56f, 3.44f}, {4.83f, 3.49f}, {5.09f, 3.57f}, {5.35f, 3.68f},
        {5.60f, 3.81f}, {5.83f, 3.96f}, {6.03f, 4.12f}, {6.26f, 4.06f}, {6.48f, 4.00f}, {6.72f, 3.95f},
        {6.96f, 3.91f}, {7.21f, 3.88f}, {7.47f, 3.86f}, {7.74f, 3.85f}, {8.02f, 3.84f}, {8.30f, 3.85f},
        {8.56f, 3.86f}, {8.82f, 3.88f}, {9.07f, 3.91f}, {9.31f, 3.95f}, {9.53f, 4.00f}, {9.75f, 4.05f},
        {9.97f, 4.11f}, {10.17f, 3.95f}, {10.39f, 3.81f}, {10.64f, 3.68f}, {10.90f, 3.57f}, {11.17f, 3.49f},
        {11.43f, 3.43f}, {11.69f, 3.41f}, {11.94f, 3.42f}, {12.01f, 3.61f}, {12.07f, 3.85f}, {12.10f, 4.11f},
        {12.12f, 4.40f}, {12.12f, 4.70f}, {12.09f, 4.98f}, {12.05f, 5.25f}, {11.98f, 5.47f}, {12.16f, 5.70f},
        {12.31f, 5.95f}, {12.45f, 6.21f}, {12.55f, 6.49f}, {12.64f, 6.77f}, {12.70f, 7.07f}, {12.74f, 7.37f},
        {12.75f, 7.67f}, {12.68f, 8.37f}, {12.49f, 9.01f}, {12.18f, 9.60f}, {11.76f, 10.11f}, {11.25f, 10.55f},
        {10.64f, 10.90f}, {9.96f, 11.16f}, {9.20f, 11.31f}, {9.39f, 11.46f}, {9.57f, 11.64f}, {9.72f, 11.85f},
        {9.85f, 12.10f}, {9.95f, 12.36f}, {10.03f, 12.65f}, {10.08f, 12.95f}, {10.09f, 13.27f}, {10.09f, 14.89f},
        {10.11f, 15.06f}, {10.16f, 15.20f}, {10.24f, 15.32f}, {10.35f, 15.41f}, {10.47f, 15.47f}, {10.62f, 15.50f},
        {10.78f, 15.49f}, {10.95f, 15.44f}, {11.98f, 14.95f}, {12.93f, 14.32f}, {13.79f, 13.54f}, {14.54f, 12.64f},
        {15.15f, 11.63f}, {15.61f, 10.51f}, {15.90f, 9.31f}, {16.00f, 8.03f}, {15.84f, 6.42f}, {15.37f, 4.91f},
        {14.63f, 3.55f}, {13.65f, 2.36f}, {12.46f, 1.38f}, {11.10f, 0.63f}, {9.60f, 0.16f}, {7.98f, 0.00f},
        {6.37f, 0.16f}, {4.87f, 0.63f}, {3.51f, 1.38f}, {2.33f, 2.36f}, {1.36f, 3.55f}, {0.63f, 4.91f},
        {0.16f, 6.42f}, {0.00f, 8.03f}, {0.02f, 8.63f}, {0.09f, 9.23f}, {0.20f, 9.82f}, {0.36f, 10.40f},
        {0.56f, 10.97f}, {0.80f, 11.52f}, {1.09f, 12.05f}, {1.41f, 12.56f}, {1.78f, 13.04f}, {2.18f, 13.49f},
        {2.61f, 13.91f}, {3.07f, 14.29f}, {3.56f, 14.64f}, {4.08f, 14.95f}, {4.61f, 15.22f}, {5.17f, 15.45f},
        {5.33f, 15.49f}, {5.48f, 15.49f}, {5.62f, 15.46f}, {5.74f, 15.40f}, {5.85f, 15.31f}, {5.93f, 15.19f},
        {5.98f, 15.06f}, {6.00f, 14.91f}, {6.00f, 13.66f}, {5.92f, 13.69f}, {5.83f, 13.72f}, {5.73f, 13.75f},
        {5.64f, 13.77f}, {5.54f, 13.79f}, {5.44f, 13.80f}, {5.35f, 13.81f}, {5.25f, 13.81f}, {4.88f, 13.79f},
        {4.55f, 13.71f}, {4.25f, 13.58f}, {3.99f, 13.40f}, {3.75f, 13.17f}, {3.54f, 12.90f}, {3.34f, 12.57f},
        {3.17f, 12.20f}, {3.11f, 12.05f}, {3.04f, 11.92f}, {2.96f, 11.80f}, {2.88f, 11.70f}, {2.79f, 11.62f},
        {2.69f, 11.56f}, {2.58f, 11.51f}, {2.45f, 11.48f}, {2.34f, 11.46f}, {2.26f, 11.42f}, {2.21f, 11.33f},
        {2.22f, 11.23f}, {2.32f, 11.11f}, {2.40f, 11.06f}, {2.49f, 11.02f}, {2.60f, 10.99f}, {2.71f, 10.98f},
        {2.83f, 10.97f}, {3.00f, 10.98f}, {3.16f, 11.02f}, {3.32f, 11.09f}, {3.47f, 11.18f}, {3.62f, 11.30f},
        {3.78f, 11.45f}, {3.93f, 11.63f}, {4.08f, 11.83f}, {4.20f, 11.99f}, {4.32f, 12.12f}, {4.44f, 12.23f},
        {4.56f, 12.33f}, {4.69f, 12.40f}, {4.83f, 12.45f}, {4.97f, 12.47f}, {5.11f, 12.48f}, {5.25f, 12.48f},
        {5.38f, 12.46f}, {5.50f, 12.42f}, {5.62f, 12.37f}, {5.74f, 12.30f}, {5.86f, 12.21f}, {5.98f, 12.11f},
        {6.11f, 11.98f}, {6.21f, 11.89f}, {6.30f, 11.79f}, {6.38f, 11.70f}, {6.47f, 11.62f}, {6.55f, 11.53f},
        {6.62f, 11.46f}, {6.69f, 11.39f}, {6.77f, 11.33f}
            };
            const float u = s.w / 16;
            std::vector<rap::point_f> points;
            for (const rap::point_f& o : outline)
                points.push_back({o.x * u, o.y * u});
            p.polygon(points, rap::paint{rap::theme_color::window_text}, std::nullopt);
        });
}

rap::node about_page()
{
    rap::nodes items = {
        L::label{std::string("IfritView ") + IFRIT_VERSION},
        L::label{_("A fast image viewer, for photos and camera raws, and the texture formats of games.")},
        L::label{_("Copyright © 2026 Manu Evans. Free software, under the GNU General Public License, version 3 or later.")},
        L::label{_("Project page and source")}.icon(github_icon()).link([] { platform::open_uri(kProjectUrl); }),
        L::label{_("The licence (GPL 3.0)")}.link([] { platform::open_uri("https://www.gnu.org/licenses/gpl-3.0.html"); }),
    };
    // The code it carries from elsewhere, and the licences that come with it.
    if (const std::string notices = notices_path(); !notices.empty())
        items.push_back(L::label{_("Third-party notices")}.link(
            [notices]
            {
                if (gchar* uri = g_filename_to_uri(notices.c_str(), nullptr, nullptr))
                {
                    platform::open_uri(uri);
                    g_free(uri);
                }
            }));
    return rap::node{rap::spec::box{rap::orientation::vertical, items, false, {}}, {}};
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
    st->window = ui.create(L::dialog{_("Settings"),
                                     {L::vbox{
                                         L::tabs{
                                             {_("General"), L::lazy{[&cfg, s] { return general_page(cfg, *s); }}},
                                             {_("Viewing"), L::lazy{[&cfg, s] { return viewing_page(cfg, *s); }}},
                                             {_("Controls"), L::lazy{[&cfg, s] { return controls_page(cfg, *s); }}},
                                             {_("File Types"), L::lazy{[&cfg, s] { return file_types_page(cfg, *s); }}},
                                             {_("About"), L::lazy{[] { return about_page(); }}},
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
