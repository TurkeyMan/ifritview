#include "i18n.h"
#include "app.h"

#include "../white_balance.h"
#include "app_util.h"

#include "platform.h"
#include "plugins.h"
#include "save.h"
#include "settings.h"

#include "decoders/decoder.h"
#include "first_image.h"
#include "integration.h"
#include "jpeg_lossless.h"
#include "encoders/encoder.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>

using APixelFormat = rap::pixel_format;


// The app while it exists, for work posted from other threads that may land
// after it's gone (both sides on the main thread).
static App* s_app = nullptr;

std::shared_future<Config> App::read_settings()
{
    return std::async(std::launch::async,
                      []
                      {
                          Config cfg;
                          cfg.load();
                          set_decode_options(decode_options_for(cfg));
                          platform::trace("settings read");
                          return cfg;
                      })
        .share();
}

App::App(rap::ui& ui, std::shared_future<Config> settings) : ui_(ui), loader_([this](Loader::Result& r) { on_loaded(r); }, [&ui](std::function<void()> fn, int) { ui.post(std::move(fn)); }), cache_(0)
{
    platform::trace("app starting");
    cfg_ = settings.get();
    cache_.set_budget(size_t(cfg_.cache_megabytes) << 20);
    geocoder_.set_provider(cfg_.geocoder_url);
    build_ui();
    // Format plugins load on a thread of their own, the first image never
    // waiting for them; once they're in, their formats join the folder.
    s_app = this;
    plugins::load_async([] { platform::post_main([] { if (s_app) s_app->plugins_loaded(); }); });
}

void App::plugins_loaded()
{
    // The folder was listed without them (even where the settings already
    // name their extensions), so it's listed again whenever there are any.
    const std::vector<std::string> extensions = plugins::extensions();
    if (extensions.empty())
        return;
    cfg_.add_new_extensions(extensions);
    // A file opened by name stays even if its type isn't listed; scan() keeps it.
    const std::string current = folder_.current();
    const bool was_empty = folder_.empty();
    if (!current.empty())
        folder_.scan(current, cfg_);
    else
        folder_.rescan(cfg_);
    if (was_empty && !folder_.empty())
        load_current();  // a folder of nothing but plugin formats
    update_title();
    update_status();
}

App::~App()
{
    s_app = nullptr;
    watch_.reset();  // its thread joined before anything it reports to goes
    stop_video();
    outgoing_video_.stop();
    // A save preview still encoding uses the encoders' and decoders' statics,
    // which go when the process does: let it finish (or give up) first.
    ++preview_->generation;
    for (int i = 0; i < 600 && preview_->running.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

namespace
{

// Icons for comparing, which no icon theme has: drawn in the theme's text colour.
const rap::paint ink{rap::theme_color::window_text};

// A and B as strokes, not text: a font's size means something different to
// each toolkit, and the letters must sit with the other icons.
rap::icon a_icon()
{
    return rap::icon::drawn(
        [](rap::painter& p, rap::size_f s)
        {
            const float u = s.w / 16, w = 1.8f * u;
            p.line({4 * u, 13.5f * u}, {8 * u, 2.5f * u}, ink, w, rap::line_cap::round);
            p.line({8 * u, 2.5f * u}, {12 * u, 13.5f * u}, ink, w, rap::line_cap::round);
            p.line({5.6f * u, 9.5f * u}, {10.4f * u, 9.5f * u}, ink, w);
        });
}

rap::icon b_icon()
{
    return rap::icon::drawn(
        [](rap::painter& p, rap::size_f s)
        {
            const float u = s.w / 16, w = 1.8f * u, half = 1.5707963f;
            p.line({5 * u, 2.1f * u}, {5 * u, 13.9f * u}, ink, w);  // past the bars, for square corners
            p.line({5 * u, 3 * u}, {8.5f * u, 3 * u}, ink, w);
            p.arc({8.5f * u, 5.5f * u}, 2.5f * u, -half, half, ink, w);
            p.line({5 * u, 8 * u}, {9 * u, 8 * u}, ink, w);
            p.arc({9 * u, 10.5f * u}, 2.5f * u, -half, half, ink, w);
            p.line({5 * u, 13 * u}, {9 * u, 13 * u}, ink, w);
        });
}

// A histogram: bars on a baseline, as no icon theme has one either.
rap::icon histogram_icon()
{
    return rap::icon::drawn(
        [](rap::painter& p, rap::size_f s)
        {
            const float u = s.w / 16;
            const float heights[] = {3, 6, 10, 12, 8, 5, 7, 4};
            for (int i = 0; i < 8; ++i)
                p.rect({(1.5f + i * 1.65f) * u, (13.5f - heights[i]) * u, 1.25f * u, heights[i] * u}, 0, ink, std::nullopt);
            p.line({1 * u, 14 * u}, {15 * u, 14 * u}, ink, 1.2f * u);
        });
}

rap::icon swipe_icon()
{
    return rap::icon::drawn(
        [](rap::painter& p, rap::size_f s)
        {
            const float u = s.w / 16;
            p.rect({2 * u, 3 * u, 12 * u, 10 * u}, u, std::nullopt, ink);
            p.line({8 * u, 1 * u}, {8 * u, 15 * u}, ink, 1.5f * u);
            p.polygon({{3.5f * u, 8 * u}, {6 * u, 6 * u}, {6 * u, 10 * u}}, ink, std::nullopt);
            p.polygon({{12.5f * u, 8 * u}, {10 * u, 6 * u}, {10 * u, 10 * u}}, ink, std::nullopt);
        });
}

rap::icon difference_icon()
{
    return rap::icon::drawn(
        [](rap::painter& p, rap::size_f s)
        {
            const float u = s.w / 16;
            p.rect({2 * u, 2 * u, 8 * u, 8 * u}, u, std::nullopt, ink);
            p.rect({6 * u, 6 * u, 8 * u, 8 * u}, u, std::nullopt, ink);
            p.rect({6 * u, 6 * u, 4 * u, 4 * u}, 0, ink, std::nullopt);  // where they overlap
        });
}

rap::icon side_by_side_icon()
{
    return rap::icon::drawn(
        [](rap::painter& p, rap::size_f s)
        {
            const float u = s.w / 16;
            p.rect({1.5f * u, 3.5f * u, 6 * u, 9 * u}, u, std::nullopt, ink);
            p.rect({8.5f * u, 3.5f * u, 6 * u, 9 * u}, u, ink, ink);
        });
}

rap::icon swap_icon()
{
    return rap::icon::drawn(
        [](rap::painter& p, rap::size_f s)
        {
            const float u = s.w / 16, w = 1.5f * u;
            p.line({3 * u, 5.5f * u}, {12 * u, 5.5f * u}, ink, w);
            p.polygon({{14 * u, 5.5f * u}, {11 * u, 3 * u}, {11 * u, 8 * u}}, ink, std::nullopt);
            p.line({4 * u, 10.5f * u}, {13 * u, 10.5f * u}, ink, w);
            p.polygon({{2 * u, 10.5f * u}, {5 * u, 8 * u}, {5 * u, 13 * u}}, ink, std::nullopt);
        });
}

}  // namespace

rap::node App::tool_button(const char* icon, const char* tooltip, void (App::*action)())
{
    return rap::layout::tool_button{rap::icon::named(icon), tooltip}.on_click([this, action] { (this->*action)(); });
}

rap::node App::compare_button(rap::icon icon, const char* tooltip, View::Compare mode)
{
    const size_t i = size_t(mode);
    return rap::layout::tool_button{std::move(icon), tooltip}
        .toggle(false)
        .on_toggled([this, mode](bool) { set_compare(mode); })
        .bind(compare_buttons_[i])
        .handle(compare_widgets_[i]);
}

void App::update_toolbar()
{
    if (!toolbar_)
        return;
    const bool comparing = view_->comparing();
    for (rap::widget* w : browse_widgets_)
        if (w)
            w->set_visible(!comparing);
    for (rap::widget* w : diff_widgets_)
        if (w)
            w->set_visible(comparing);
    for (size_t i = 0; i < compare_widgets_.size(); ++i)
    {
        if (compare_widgets_[i])
            compare_widgets_[i]->set_visible(comparing);
        if (compare_buttons_[i])
            compare_buttons_[i]->set_checked(comparing && size_t(view_->compare_mode()) == i);
    }
    exposure_box_->set_visible(!comparing && exposure_applies());
    if (gain_)
        gain_->set_enabled(comparing && view_->compare_mode() == View::Compare::Difference);
    if (compare_widgets_[size_t(View::Compare::Difference)])
        compare_widgets_[size_t(View::Compare::Difference)]->set_enabled(view_->compare_difference() != nullptr);
    const bool browse = !navigation_locked();
    for (size_t i : {size_t(0), size_t(1), size_t(3)})  // previous, next, slideshow
        if (browse_widgets_[i])
            browse_widgets_[i]->set_enabled(browse);
}

void App::build_ui()
{
    platform::trace("ui: building");
    view_ = std::make_unique<View>(ui_, cfg_);
    side_view_ = std::make_unique<View>(ui_, cfg_);
    namespace L = rap::layout;
    using L::hbox;
    using L::horizontal;
    using L::separator;
    using L::spacer;
    using L::vbox;

    // The develop controls, under the histogram: exposure in stops round the
    // camera's compensation (raw and HDR images), and a raw's white balance
    // as a temperature and tint (raws the GPU develops).
    rap::node develop =
        vbox{
            L::form{
                {_("Exposure"), L::slider{-3.0, 3.0, 0.0}
                                    .step(0.1, 1.0)
                                    .show_value(2)
                                    .mark(0.0)
                                    .on_changed([this](double) { retone(); })
                                    .bind(exposure_)
                                    .tooltip(_("Exposure (EV). Rests at the camera's compensation, i.e. as shot.\n"
                                               "Greyed while the embedded preview stands in for the raw"))},
            },
            vbox{
                L::form{
                    {_("Temperature"), L::slider{2000.0, 12000.0, 5500.0}
                                           .step(50, 500)
                                           .show_value(0)
                                           .on_changed([this](double) { white_balance_changed(); })
                                           .bind(wb_kelvin_)
                                           .tooltip(_("White balance: the colour temperature of the light (kelvin)"))},
                    {_("Tint"), L::slider{-150.0, 150.0, 0.0}
                                    .step(1, 10)
                                    .show_value(0)
                                    .mark(0.0)
                                    .on_changed([this](double) { white_balance_changed(); })
                                    .bind(wb_tint_)
                                    .tooltip(_("White balance: towards green (-) or magenta (+)"))},
                },
            }
                .handle(wb_box_),
            L::form{{"", L::button{_("As shot")}
                             .on_click([this] { white_balance_as_shot(); })
                             .tooltip(_("Back to the camera's exposure and white balance"))}},
        }
            .handle(exposure_box_);

    platform::trace("ui: creating the window");
    window_ = ui_.create(L::window{
        "IfritView",
        {vbox{
            L::toolbar{
                // Always: open and save, zoom, fullscreen.
                tool_button("document-open-symbolic", _("Open… (Ctrl+O)"), &App::open_dialog),
                tool_button("media-floppy-symbolic", _("Save as… (Ctrl+S); comparing, saves the difference shown"), &App::save_as),
                tool_button("document-print-symbolic", _("Print… (Ctrl+P); a selection prints alone"), &App::print_image),
                separator{},
                tool_button("zoom-out-symbolic", _("Zoom out (-, Ctrl+wheel)"), &App::zoom_out),
                tool_button("zoom-in-symbolic", _("Zoom in (+, Ctrl+wheel)"), &App::zoom_in),
                tool_button("zoom-original-symbolic", _("Actual pixels (1)"), &App::zoom_original),
                tool_button("zoom-fit-best-symbolic", _("Fit to window (F)"), &App::zoom_fit),
                L::tool_button{rap::icon::named("view-grid-symbolic"), _("Smooth when zoomed in (off shows crisp pixels)")}
                    .toggle(cfg_.smooth_upscale())
                    .on_toggled(
                        [this](bool on)
                        {
                            // Back on: the smooth filter it had, else Lanczos-3.
                            if (cfg_.smooth_upscale())
                                smooth_enlarge_ = cfg_.enlarge_filter;
                            cfg_.enlarge_filter = on ? smooth_enlarge_ : ScaleFilter::Nearest;
                            cfg_.save();
                            view_->config_changed();
                        })
                    .bind(smooth_toggle_),
                tool_button("view-fullscreen-symbolic", _("Fullscreen (Enter, double-click)"), &App::toggle_fullscreen),
                separator{},
                // Browsing: through the folder, and what's done to an image.
                L::tool_button{rap::icon::named("go-previous-symbolic"), _("Previous image (Left, wheel up)")}
                    .on_click([this] { navigate(-1); })
                    .handle(browse_widgets_[0]),
                L::tool_button{rap::icon::named("go-next-symbolic"), _("Next image (Right, wheel down)")}
                    .on_click([this] { navigate(+1); })
                    .handle(browse_widgets_[1]),
                separator{}.handle(browse_widgets_[2]),
                L::tool_button{rap::icon::named("media-playback-start-symbolic"), _("Slideshow (S): fullscreen, timed, with gentle motion")}
                    .on_click([this] { toggle_slideshow(); })
                    .handle(browse_widgets_[3]),
                L::tool_button{rap::icon::named("edit-cut-symbolic"), _("Crop to selection (Ctrl+Y)")}
                    .on_click([this] { crop_to_selection(); })
                    .bind(crop_button_)
                    .handle(browse_widgets_[4]),
                L::tool_button{rap::icon::named("document-edit-symbolic"), _("Apply edits from XMP sidecars (exposure, crop, rotation)")}
                    .toggle(cfg_.apply_xmp_edits)
                    .on_toggled([this](bool) { toggle_xmp(); })
                    .bind(xmp_toggle_)
                    .handle(browse_widgets_[5]),
                // Comparing two images, in place of browsing.
                compare_button(a_icon(), _("Show A (Tab flips A and B)"), View::Compare::A),
                compare_button(b_icon(), _("Show B (Tab flips A and B)"), View::Compare::B),
                compare_button(swipe_icon(), _("Swipe: A left of the divider, B right of it (drag the divider)"), View::Compare::Swipe),
                compare_button(difference_icon(), _("Difference: where A and B differ, amplified"), View::Compare::Difference),
                L::tool_button{swap_icon(), _("Swap A and B")}.on_click([this] { swap_compare(); }).handle(diff_widgets_[0]),
                L::tool_button{side_by_side_icon(), _("Side by side: B in a second pane, beside A")}
                    .toggle(false)
                    .on_toggled([this](bool on) { set_side_by_side(on); })
                    .bind(side_button_)
                    .handle(diff_widgets_[1]),
                spacer{},
                hbox{
                    L::label{_("Gain")},
                    L::slider{1.0, 32.0, 8.0}
                        .step(1.0, 4.0)
                        // No value shown: the view's tag has it ("Difference ×8"), and
                        // GTK would put it above the track, making the toolbar taller.
                        .on_changed([this](double v) { view_->set_compare_gain(int(v + 0.5)); })
                        .bind(gain_)
                        .width(rap::extent{14, rap::extent::unit::chars})
                        .tooltip(_("How much the difference is amplified")),
                }
                    .handle(diff_widgets_[2]),
                L::tool_button{histogram_icon(), _("Histogram and levels (G)")}.on_click([this] { toggle_histogram(); }),
                tool_button("dialog-information-symbolic", _("Metadata / EXIF (I)"), &App::toggle_metadata),
                tool_button("preferences-system-symbolic", _("Settings (Ctrl+,)"), &App::open_settings),
            }
                .handle(toolbar_),
            L::splitter{
                horizontal,
                hbox{
                    view_->node(),
                    vbox{side_view_->node()}.stretch().handle(side_pane_),
                }
                    .stretch(),
                vbox{
                // Hidden at first, so made once the window is up and idle.
                L::deferred{[this]
                            {
                                return L::list{{{_("Tag"), rap::extent{14, rap::extent::unit::chars}}, {_("Value"), {}, true}},
                                               &metadata_model_}
                                    .selection(rap::selection_mode::single)
                                    .on_activate(
                                        [this](size_t row)
                                        {
                                            const std::string url = metadata_model_.url(row);
                                            if (!url.empty())
                                                open_url(url);
                                        })
                                    .bind(metadata_list_)
                                    .stretch();
                            }}
                    .stretch()  // the list's own stretch is inside the builder: the slot needs it too
                    .handle(info_box_),
                // The histogram of what's shown (drawn on the GPU), and what's measured.
                vbox{
                    L::canvas{}
                        .on_paint(
                            [this](rap::painter& p, rap::size_f size)
                            {
                                if (!histogram_canvas_->gpu_device())
                                {
                                    rap::text_style style;
                                    style.fg = rap::theme_color::placeholder_text;
                                    p.text({8, 8}, _("The histogram needs a GPU."), style);
                                    return;
                                }
                                paint_histogram_legend(p, size);
                            })
                        .on_pointer([this](const rap::pointer_event& e) { return histogram_legend_click(e); })
                        .bind(histogram_canvas_)
                        .height(rap::extent{13, rap::extent::unit::lines}),

                    develop,
                    L::form{
                        {_("Peak"), L::label{""}.bind(hist_peak_)},
                        {_("Brightest"), L::label{""}.bind(hist_max_)},
                        {_("Darkest"), L::label{""}.bind(hist_min_)},
                        {_("Average (APL)"), L::label{""}.bind(hist_average_)},
                        {_("Clipped"), L::label{""}.bind(hist_clipped_)},
                    },
                    L::spacer{},
                }
                    .margins()
                    .stretch()
                    .handle(histogram_box_),
                }
                    .handle(metadata_pane_),
            }
                .bind(splitter_)
                .stretch(),
            hbox{
                // Long names and details give way, so they never hold the window wide.
                L::label{""}.ellipsize().bind(status_left_).stretch(),
                L::label{""}.ellipsize().bind(status_center_),
                L::button{""}.flat().on_click([this] { open_url(location_url_); }).bind(status_location_),
                L::label{""}.bind(status_right_),
            }
                .margins()
                .handle(footer_),
        }}}.size(rap::extent{float(cfg_.min_window_width), rap::extent::unit::px}, rap::extent{float(cfg_.min_window_height), rap::extent::unit::px})
                             .margins(false));
    platform::trace("ui: window created");
    view_->attach(view_->canvas());
    view_->on_needs_pixels = [this] { pixels_for_cpu(); };
    histogram_box_->set_visible(false);
    // The docked histogram: drawn again when its canvas changes size, and
    // its numbers when the view has counted.
    histogram_canvas_->on_frame_target([this](const rap::frame_target&) { view_->histogram_panel_changed(); });
#ifdef HAVE_GPU
    view_->on_histogram_details = [this](const GpuView::Details& d)
    {
        // Luminance in nits (SDR white at BT.2408's 203 until the display's is
        // known) and EV from white; or, set so, as a share of white.
        const auto level = [&](float v) -> std::string
        {
            char buf[64];
            if (cfg_.levels_in_nits)
                std::snprintf(buf, sizeof buf, "%.*f nits (%+.1f EV)", v * 203 < 10 ? 1 : 0, double(v) * 203,
                              v > 0 ? std::log2(double(v)) : -99.0);
            else
                std::snprintf(buf, sizeof buf, "%.1f%%", double(v) * 100);
            return buf;
        };
        if (std::getenv("IFRIT_TRACE"))
            std::fprintf(stderr, "histogram: known %d scene %d min %g max %g peak %g average %g black %g white %g\n", int(d.known),
                         int(d.scene), double(d.minimum), double(d.maximum), double(d.peak), double(d.average), double(d.black),
                         double(d.white));
        if (!d.known)
        {
            for (rap::label* l : {hist_peak_, hist_max_, hist_min_, hist_average_, hist_clipped_})
                l->set_text("–");
            return;
        }
        hist_peak_->set_text(level(d.peak));
        hist_max_->set_text(level(d.maximum));
        hist_min_->set_text(level(d.minimum));
        hist_average_->set_text(level(d.average));
        char clipped[96];
        std::snprintf(clipped, sizeof clipped, _("%.1f%% black, %.1f%% white"), double(d.black) * 100, double(d.white) * 100);
        hist_clipped_->set_text(clipped);
    };
#endif
    side_view_->attach(side_view_->canvas());
    side_pane_->set_visible(false);
    side_view_->set_divided(true);
    // Side by side, either view's zoom and pan carry over to the other.
    const auto follow = [this](View& from, View& to)
    {
        if (!side_by_side_ || syncing_framing_)
            return;
        syncing_framing_ = true;
        to.set_framing(from.framing());
        syncing_framing_ = false;
    };
    view_->on_framing_changed = [this, follow] { follow(*view_, *side_view_); };
    side_view_->on_framing_changed = [this, follow] { follow(*side_view_, *view_); };
    window_->on_key([this](const rap::key_event& e) { return on_key(e); });
    // A dropped file opens with its folder, as if opened from the menu.
    window_->on_files_dropped(
        [this](const std::vector<std::string>& paths)
        {
            if (paths.size() == 2)
                open_diff(paths[0], paths[1]);
            else if (!paths.empty())
                open_path(paths.front());
        });
    // A width the user drags the info pane to is kept for next time.
    splitter_->on_moved([this] { remember_pane_width(); });
    window_->on_fullscreen_changed([this](bool fs) { on_fullscreen_changed(fs); });
    window_->on_close_request(
        [this]
        {
            quit();
            return true;
        });
    window_->set_icon(rap::icon::named("ifritview"));
    // Where the user drags it, as where the window fits an image: never below the minimum.
    window_->set_min_size({float(cfg_.min_window_width), float(cfg_.min_window_height)});
    metadata_pane_->set_visible(false);
    exposure_box_->set_visible(false);
    crop_button_->set_enabled(false);
    update_toolbar();
    status_location_->set_visible(false);
    status_location_->set_tooltip(_("Open in maps"));

    view_->on_action = [this](Action a)
    {
        switch (a)
        {
            case Action::NextImage:
                navigate(+1);
                break;
            case Action::PrevImage:
                navigate(-1);
                break;
            case Action::ToggleFullscreen:
                toggle_fullscreen();
                break;
            case Action::OpenFile:
                open_dialog();
                break;
            case Action::Settings:
                open_settings();
                break;
            case Action::Quit:
                quit();
                break;
            default:
                break;
        }
    };
    side_view_->on_action = view_->on_action;
    // A notch up brightens by a third of a stop, the cameras' own step,
    // where the slider applies (raws, HDR).
    view_->on_exposure_step = [this](int direction)
    {
        if (image_ && exposure_applies() && !image_->is_preview())
            set_exposure(std::clamp(exposure_->value() - direction / 3.0, exposure_range_[0], exposure_range_[1]));
    };
    view_->on_zoom_changed = [this] { update_status(); };
    view_->on_presented = [this] { image_presented(); };
    view_->on_resized = [this]
    {
        if (refit_after_layout_)
            refit_after_pane_toggle();
        else if (refit_when_laid_out_)
            fit_window_to_image();  // measured now: clears the flag
        else if (fitted_scale_ > 0 && view_->scale_factor() != fitted_scale_)
        {
            // Fitted at another scale (Qt on Wayland learns its fractional one
            // just after showing; a move to another monitor): again at this
            // one. Posted, as this can run inside a paint.
            fitted_scale_ = view_->scale_factor();
            ui_.post([this] { fit_window_to_image(); });
        }
        settle_side_by_side();
    };
    view_->on_video_toggle = [this] { toggle_video(); };
    view_->on_video_mute = [this] { toggle_mute(); };
    view_->on_video_volume = [this](double v)
    {
        cfg_.video_volume = v;
        video_.set_volume(v);
        volume_dirty_ = true;  // saved by the progress timer, not on every pixel of a drag
    };
    view_->set_video_volume(cfg_.video_volume);
    view_->set_video_muted(cfg_.video_muted);
    view_->on_video_seek = [this](double f, bool final)
    {
        // Holding the knob pauses; letting go resumes only if it was playing.
        if (!scrubbing_)
        {
            scrubbing_ = true;
            resume_after_scrub_ = video_.playing();
            video_.set_playing(false);
        }
        video_.seek(f, final);
        if (final)
        {
            scrubbing_ = false;
            video_.set_playing(resume_after_scrub_);
        }
    };
    video_.on_frame = [this]
    {
        if (!image_)
            return;
        const int w = image_->width, h = image_->height;
        view_->image_changed();
        if (video_.duration() > 0)
            view_->set_video_progress(video_.position() / video_.duration(), video_.playing());
        // A rotated stream can change shape after the poster.
        if (w != video_shape_[0] || h != video_shape_[1])
            fit_window_to_image();
        video_shape_ = {w, h};
    };
    view_->on_selection_changed = [this]
    {
        int x, y, w, h;
        crop_button_->set_enabled(view_->selection(x, y, w, h));
        update_status();
    };
    update_status();
}

void App::show_window()
{
    platform::trace("showing the window");
    window_->show();
    platform::trace("window shown");
    view_->canvas()->focus();
}

void App::open_url(const std::string& url)
{
    if (!url.empty())
        platform::open_uri(url);
}

// Ctrl+C: the selection if there is one, otherwise the whole image. The
// selection is dropped afterwards as the visible sign that it was copied.
void App::pixels_for_cpu()
{
    if (!image_ || !image_->pixels_stale)
        return;
    if (image_->mosaic)
    {
        // Developed on the GPU: rendered there and read back.
        if (view_->develop_pixels())
            image_->pixels_stale = false;
        else
            status_message_ = _("This raw needs the GPU to develop it");
        return;
    }
    image_->pixels_stale = false;
    image_->retone(image_->exposure_edit);
    view_->retoned();
}

void App::copy_to_clipboard()
{
    if (!image_)
        return;
    pixels_for_cpu();
    int x, y, w, h;
    const Image region = view_->selection(x, y, w, h) ? image_->crop(x, y, w, h) : *image_;
    ui_.set_clipboard_image(region.width, region.height, APixelFormat::bgra8_premultiplied, region.pixels.data(), region.stride);
    view_->clear_selection();
    status_message_ = rap::i18n::format(_("Copied {0} × {1} to clipboard"), region.width, region.height);
    update_status();
}

void App::transform(int exif_orientation)
{
    if (!image_)
        return;
    if (!image_->mosaic)
        pixels_for_cpu();  // (a raw the GPU develops turns as it develops)
    image_->apply_orientation(exif_orientation);  // a preview remembers it for its develop
    view_->image_changed();
    fit_window_to_image();
    // Wayland ignores a resize while the previous one is still being
    // configured; a quick second rotation needs this follow-up.
    refit_timer_ = ui_.timer(120, false, [this] { fit_window_to_image(); });
    update_status();
    refresh_metadata();
}

void App::script_toggle_xmp()
{
    xmp_toggle_->set_checked(!xmp_toggle_->checked());  // set from code: no callback
    toggle_xmp();
}

// Scripts end the dialog in front: Save As (only cancelled: accepting it
// opens the file chooser), else Settings.
void App::script_end_dialog(bool accept)
{
    if (save_dialog_ && !accept)
        save_dialog_->close();
    else if (end_settings_)
    {
        const auto end = end_settings_;  // ending it clears end_settings_
        end(accept);
    }
}

void App::script_compare(const std::string& how)
{
    if (how == "swap")
        swap_compare();
    else if (how == "side")
        set_side_by_side(!side_by_side_);
    else if (how.rfind("gain:", 0) == 0)
    {
        gain_->set_value(std::atoi(how.c_str() + 5));
        view_->set_compare_gain(std::atoi(how.c_str() + 5));
    }
    else
        set_compare(how == "a" ? View::Compare::A : how == "swipe" ? View::Compare::Swipe : how == "diff" ? View::Compare::Difference : View::Compare::B);
}

void App::toggle_xmp()
{
    cfg_.apply_xmp_edits = xmp_toggle_->checked();
    cfg_.save();
    // Re-read the file with or without its edits; the cached copy embodies
    // the old choice, so it has to go.
    if (image_ && image_->xmp.present)
    {
        cache_.erase(image_path_);
        image_.reset();
        load_current();
    }
}

int App::run()
{
    ui_.post([] { platform::trace("event loop running"); });
    return ui_.run();
}

void App::quit()
{
    ui_.quit();
}

// -- navigation ---------------------------------------------------------------
void App::open_path(const std::string& path, FirstImage* first)
{
    gchar* canonical = g_canonicalize_filename(path.c_str(), nullptr);
    const std::string file = canonical;
    g_free(canonical);
    if (!first)
    {
        platform::trace("reading the folder");
        folder_.scan(file, cfg_);
        platform::trace("folder read: %d files", folder_.count());
        load_current();
        return;
    }
    // The image is being decoded already: the window opens with it, and the
    // folder is listed once it's painted (it isn't needed to show it).
    folder_.set_single(file);
    deferred_scan_ = file;
    const uint64_t generation = ++load_generation_;
    // A moment's wait for it, so a big one doesn't hold the window back:
    // past that, it arrives in the open window.
    if (first->wait_for(std::chrono::milliseconds(250)))
        take_first_image(*first, file, generation);
    else
        first->on_done([this, first, file, generation] { ui_.post([this, first, file, generation] { take_first_image(*first, file, generation); }); });
}

void App::take_first_image(FirstImage& first, const std::string& file, uint64_t generation)
{
    if (generation != load_generation_)
        return;  // something else was opened meanwhile
    Loader::Result r{generation, file, first.image(), first.error(), first.preview()};
    on_loaded(r);
    if (first.preview())
        loader_.request_develop(generation, file);  // the stand-in's real decode
}

// The folder shown, watched: files added, removed, renamed or rewritten
// there change the list (and the image, if it's the one shown) as they
// happen. Started once the first image is up, never in its way.
void App::watch_folder()
{
    const std::string dir = folder_.directory();
    if (!first_presented_ || dir == watched_dir_)
        return;
    watch_.reset();
    watched_dir_ = dir;
    if (dir.empty())
        return;
    rap::sys::watch_options options;
    options.settle = std::chrono::milliseconds(300);  // a file being written arrives once, done
    auto w = rap::sys::directory_watch::start(rap::sys::path_from_utf8(dir), options,
                                              [](rap::sys::watch_batch batch)
                                              {
                                                  // On the watch's thread: over to the UI.
                                                  platform::post_main([batch = std::move(batch)] { if (s_app) s_app->folder_changed(batch); });
                                              });
    if (w)
        watch_ = std::move(*w);
    else
        platform::trace("not watching %s: %s", dir.c_str(), w.error().c_str());
}

void App::folder_changed(const rap::sys::watch_batch& batch)
{
    if (folder_.directory() != watched_dir_ || diff_session_)
        return;  // a different folder by now, or comparing two files
    const std::filesystem::path dir = rap::sys::path_from_utf8(watched_dir_).lexically_normal();
    bool listed = batch.overflow, reload_shown = false;
    if (batch.overflow)
        folder_.rescan(cfg_);  // events were lost: what's there now
    for (const rap::sys::file_event& e : batch.events)
    {
        if (batch.overflow || e.path.parent_path().lexically_normal() != dir)
            continue;
        const std::string name = rap::sys::path_to_utf8(e.path.filename());
        const std::string full = rap::sys::path_to_utf8(e.path);
        using K = rap::sys::file_event::kind;
        switch (e.what)
        {
            case K::removed: listed |= folder_.remove(name); break;
            // Added under the shown file's name: a fresh temporary renamed
            // over it within the settle time comes as that, the watch not
            // knowing the name was there.
            case K::added:
            case K::modified:
                listed |= folder_.add(name, cfg_);  // one that wasn't readable before, say
                reload_shown |= name == folder_.current_name();
                break;
            case K::renamed:
            {
                const std::string from = rap::sys::path_to_utf8(e.old_path.filename());
                const bool shown = from == folder_.current_name();
                // Renamed over the shown file: how most programs save (a
                // temporary written, then renamed into place). Read it again.
                reload_shown |= !shown && name == folder_.current_name();
                listed |= folder_.rename(from, name, cfg_);
                cache_.erase(rap::sys::path_to_utf8(e.old_path));
                if (shown)
                    image_path_ = folder_.current();  // the same image, by its new name
                break;
            }
        }
        cache_.erase(full);  // what was cached of it is out of date
    }
    if (reload_shown)
    {
        // Rewritten: read it again. Not via the cache, which leaving it would
        // fill with what's shown now, dated as the new file.
        image_path_.clear();
        load_current();
    }
    platform::trace("folder changed: %zu events%s, %d files", batch.events.size(), batch.overflow ? " (overflow)" : "", folder_.count());
    if (listed || reload_shown)
    {
        update_title();
        update_status();
        schedule_prefetch();
    }
}

void App::load_current(const std::string& file)
{
    if (diff_session_)
        end_comparing();  // a file opened: back to browsing
    watch_folder();
    const std::string path = file.empty() ? folder_.current() : file;
    if (path != part_file_)
    {
        part_ = {};  // another file opens at its first image
        part_file_ = path;
    }
    const std::string key = part_key(path, part_);
    if (save_dialog_ && key != image_key_)
        save_dialog_->close();  // it was saving (and previewing) the image going away
    if (path.empty())
    {
        status_message_ = folder_.directory().empty() ? std::string(_("No image open")) : rap::i18n::format(_("No images in {0}"), folder_.directory());
        update_status();
        update_title();
        return;
    }
    stop_video();
    leave_image();
    ++load_generation_;
    ImageCache::Entry hit;
    if (cache_.get(key, hit))
    {
        Loader::Result r{load_generation_, key, hit.image, {}, hit.image->is_preview(), false, true};
        on_loaded(r);
        return;
    }
    status_message_ = busy_message_ = rap::i18n::format(_("Loading {0}…"), std::filesystem::path(path).filename().string());
    update_status();
    int vw = 0, vh = 0;
    view_->view_size(vw, vh);
    loader_.set_stand_in_fit(vw, vh);
    loader_.request(load_generation_, key);
}

void App::step_page(int delta)
{
    if (!image_ || image_->page_count <= 1 || navigation_locked())
        return;
    const int page = std::clamp(part_.page + delta, 0, image_->page_count - 1);
    if (page == part_.page)
        return;
    part_ = {page, 0};
    load_current(image_path_);
}

void App::step_level(int delta)
{
    if (!image_ || image_->level_count <= 1 || navigation_locked())
        return;
    const int level = std::clamp(part_.level + delta, 0, image_->level_count - 1);
    if (level == part_.level)
        return;
    part_.level = level;
    load_current(image_path_);
}

void App::navigate(int delta)
{
    if (navigation_locked())
        return;
    if (awaiting_present_)
    {
        pending_steps_ = std::clamp(pending_steps_ + delta, -8, 8);
        return;
    }
    if (tracing())
        nav_stamp_us_ = now_us();
    const int64_t now = g_get_monotonic_time();
    nav_steps_.push_back({now, delta > 0 ? 1 : -1});
    while (nav_steps_.size() > 64 || now - nav_steps_.front().us > 1'000'000)
        nav_steps_.pop_front();
    if (folder_.step(delta, cfg_.wrap_around))
    {
        awaiting_present_ = true;
        load_current();
    }
}

// The current image is on screen: play out one queued step, if any.
void App::image_presented()
{
    if (!first_presented_)
    {
        first_presented_ = true;
        platform::trace("first image painted");
        std::thread([] { refresh_registration(); }).detach();  // after an upgrade that moved the executable
        if (!deferred_scan_.empty())
            ui_.post(
                [this]
                {
                    // Now the rest of the folder, keeping to the image shown.
                    const std::string file = std::move(deferred_scan_);
                    deferred_scan_.clear();
                    if (folder_.empty() || folder_.current() != file)
                        return;  // something else was opened meanwhile
                    folder_.scan(file, cfg_);
                    platform::trace("folder read: %d files", folder_.count());
                    watch_folder();
                    update_title();
                    update_status();
                    schedule_prefetch();
                });
    }
    awaiting_present_ = false;
    preload_optional_libraries();  // the first image is up; warm video and PDF support
    if (pending_steps_ == 0)
        return;
    const int d = pending_steps_ > 0 ? 1 : -1;
    pending_steps_ -= d;
    navigate(d);
}

void App::go_first()
{
    if (navigation_locked())
        return;
    if (folder_.go(0))
        load_current();
}

void App::go_last()
{
    if (navigation_locked())
        return;
    if (folder_.go(folder_.count() - 1))
        load_current();
}

void App::reload()
{
    folder_.rescan(cfg_);
    load_current();
}

void App::on_loaded(Loader::Result& r)
{
    if (r.image)
        request_place(r.image);  // prefetched neighbours get their place name looked up ahead of time
    if (r.prefetch && r.image && !r.preview)
    {
        // The whole image, for one shown or cached as its preview: adopted
        // into it, as its develop would be.
        if (image_ && image_key_ == r.path && image_->is_preview())
            r.prefetch = false;  // on screen: below
        else if (ImageCache::Entry old; cache_.peek(r.path, old) && old.image->is_preview())
        {
            old.image->adopt(std::move(*r.image));
            return;
        }
    }
    if (r.prefetch)
    {
        // Never replace what is already cached: it may carry the user's edits.
        if (r.image && r.path != image_key_ && !cache_.contains(r.path))
            cache_.put(r.path, {r.image, ImageCache::file_mtime(key_file(r.path))});
        if (r.image && r.path == current_key() && (!image_ || image_key_ != r.path))
        {  // the user moved onto it while it was decoding: it's here before the stand-in
            r.generation = load_generation_;
            r.prefetch = false;
            if (!r.image->is_preview())
                loader_.cancel_develop(r.path);
        }
        else
        {
            return;
        }
    }
    // A stand-in for what's already shown in full (its prefetch got there first).
    if (r.preview && image_ && image_key_ == r.path && !image_->is_preview())
    {
        loader_.cancel_develop(r.path);
        return;
    }
    // A develop for the image on screen is adopted in place, even one
    // started before the user left it and came back (only one runs).
    if (!r.preview && !r.cached && r.image && image_ && image_->is_preview() && image_key_ == r.path)
    {
        image_->adopt(std::move(*r.image));
        if (tracing())
            std::fprintf(stderr, "developed %s %dx%d: decoded in %.0f ms, %.0f ms to reach the UI\n", key_text(r.path).c_str(), image_->width,
                         image_->height, (r.decoded_us - r.picked_us) / 1000.0, (g_get_monotonic_time() - r.decoded_us) / 1000.0);
        status_message_.clear();
        view_->image_changed();
        fit_window_to_image();
        sync_exposure_slider();
        update_status();
        refresh_metadata();
        return;
    }
    if (r.generation != load_generation_)
    {
        // Too late to show, but a develop still completes its cached preview.
        if (r.image && !r.preview && !r.cached)
        {
            ImageCache::Entry old;
            if (cache_.get(r.path, old) && old.image->is_preview())
                old.image->adopt(std::move(*r.image));
        }
        return;
    }
    if (nav_stamp_us_)
    {
        const double base = nav_stamp_us_ / 1000.0;
        if (r.cached)
            std::fprintf(stderr, "timeline: cache hit at %.1f ms\n", now_us() / 1000.0 - base);
        else
            std::fprintf(stderr, "timeline: picked %.1f  decoded %.1f  main %.1f ms\n", r.picked_us / 1000.0 - base, r.decoded_us / 1000.0 - base,
                         now_us() / 1000.0 - base);
    }
    const bool from_cache = r.cached;
    if (r.picked_us)
        platform::trace("UI has the loader's result: started %.1f ms ago, decoded in %.1f ms, waited %.1f ms to reach the UI",
                        (g_get_monotonic_time() - r.picked_us) / 1000.0, (r.decoded_us - r.picked_us) / 1000.0, (g_get_monotonic_time() - r.decoded_us) / 1000.0);
    else
        platform::trace("UI has the image (%s)", r.cached ? "from the cache" : "decoded ahead");
    if (tracing() && r.image)
        std::fprintf(stderr, "loaded %s %dx%d preview=%d cache=%zuMB\n", key_text(r.path).c_str(), r.image->width, r.image->height, int(r.image->is_preview()),
                     cache_.bytes() >> 20);
    if (!r.image)
    {
        image_.reset();
        image_path_ = key_file(r.path);
        image_key_ = r.path;
        part_file_ = image_path_;
        part_ = key_part(r.path);
        schedule_prefetch();
        view_->set_image(nullptr);
        status_message_ = rap::i18n::format(_("Cannot load {0}: {1}"), folder_.current_name(), r.error);
        update_status();
        update_title();
        image_presented();
        return;
    }
    stop_animation();
    image_ = r.image;
    image_path_ = key_file(r.path);
    image_key_ = r.path;
    part_file_ = image_path_;  // however it came (the first image doesn't come through load_current)
    part_ = key_part(r.path);
    schedule_prefetch();  // its next page among them, now it's known
    std::error_code ec;
    file_size_ = std::filesystem::file_size(image_path_, ec);
    if (ec)
        file_size_ = 0;
    status_message_ = busy_message_ = image_->is_preview() && image_->is_raw ? _("Developing…") : "";
    if (!from_cache && cfg_.apply_xmp_edits && image_->xmp.present)
        apply_xmp(true);
    // Come back to, it's as it was opened: an exposure set before leaving
    // goes back to as shot (or its sidecar's).
    if (from_cache)
    {
        const double opened = cfg_.apply_xmp_edits && image_->xmp.present ? image_->xmp.exposure : 0.0;
        if (image_->exposure_edit != opened)
        {
            image_->exposure_edit = opened;
            if (image_->tonable())
                image_->retone(opened);
        }
    }
    if (image_->is_preview() && from_cache)  // its develop never finished (or was skipped when the user moved on)
        ensure_developed();
    view_->set_image(image_);
    if (nav_stamp_us_)
        view_->stamp_present(nav_stamp_us_, image_.get());
    nav_stamp_us_ = 0;
    fit_window_to_image();
    update_title();
    sync_exposure_slider();
    update_status();
    refresh_metadata();
    if (cfg_.animate && image_->animation)
        start_animation();
    view_->set_video(image_->is_video, image_->has_audio);
    if (image_->is_video && (cfg_.video_autoplay || slideshow_))
        play_video();
    if (slideshow_)
        slideshow_start_slide();
}

// The slider reads in the shooter's units: it rests at the camera's exposure
// compensation (already in the pixels) and the render uses the difference.
// Greyed while the embedded JPEG stands in for the raw.
void App::sync_exposure_slider()
{
    const bool shown = exposure_applies();
    if (!fullscreen_)
        exposure_box_->set_visible(shown);
    if (!shown)
        return;
    const double bias = image_->exposure_bias;
    // An HDR image may need taking down further than a raw to see its brightest.
    const double down = image_->hdr() ? std::max(3.0, std::ceil(std::log2(double(image_->source->peak)))) : 3.0;
    exposure_range_[0] = bias - down;
    exposure_range_[1] = bias + 3.0;
    exposure_->set_range(exposure_range_[0], exposure_range_[1]);
    exposure_->set_value(bias + image_->exposure_edit);
    exposure_->set_enabled(!image_->is_preview());
    sync_white_balance();
}

void App::sync_white_balance()
{
    const bool raw = image_ && image_->mosaic && !image_->is_preview();
    wb_box_->set_visible(raw);
    if (!raw)
        return;
    double kelvin = image_->wb_kelvin, tint = image_->wb_tint;
    if (kelvin <= 0)
        wb_as_shot(*image_->mosaic, kelvin, tint);
    syncing_wb_ = true;
    wb_kelvin_->set_value(kelvin);
    wb_tint_->set_value(tint);
    syncing_wb_ = false;
}

void App::white_balance_changed()
{
    if (syncing_wb_ || !image_ || !image_->mosaic)
        return;
    image_->wb_kelvin = wb_kelvin_->value();
    image_->wb_tint = wb_tint_->value();
    view_->redraw();
}

// Back to how the camera took it: its white balance and its exposure.
void App::white_balance_as_shot()
{
    if (!image_)
        return;
    image_->wb_kelvin = image_->wb_tint = 0;
    sync_white_balance();
    if (exposure_applies())
        set_exposure(image_->exposure_bias);  // the slider rests at the camera's compensation
    view_->redraw();
}

void App::set_exposure(double ev)
{
    exposure_->set_value(ev);
    retone();
}

// Replaces the displayed image with the selected region. Nothing is written
// to disk; F5 reloads the original.
// Leaving an image: kept for coming back, unless it was cropped or turned:
// then it's read afresh, as the file is.
void App::leave_image()
{
    if (!image_ || image_path_.empty())
        return;
    if (image_->edited)
        cache_.erase(image_key_);
    else
        cache_.put(image_key_, {image_, ImageCache::file_mtime(image_path_)}, !flicking());  // flicking on: what's behind goes first
}

void App::crop_to_selection()
{
    int x, y, w, h;
    if (!image_ || !view_->selection(x, y, w, h))
        return;
    pixels_for_cpu();
    *image_ = image_->crop(x, y, w, h);  // same object: the view and pane keep their pointer
    if (image_->tonable())
        image_->retone(image_->exposure_edit);
    image_->info = (image_->info.empty() ? "" : image_->info + "  ·  ") + _("cropped");
    view_->clear_selection();  // it was the image's own shape a moment ago
    view_->image_changed();
    fit_window_to_image();
    update_status();
    refresh_metadata();
}

// Cached as a visit would leave it: a preview the GPU would develop doesn't count.
bool App::cached_in_full(const std::string& key)
{
    ImageCache::Entry e;
    if (!cache_.contains(key))
        return false;
    return !decode_options().gpu_develop || (cache_.peek(key, e) && !e.image->is_preview());
}

// Neighbours in the browsing direction first, then behind; skips what the
// cache already has.
void App::schedule_prefetch()
{
    std::vector<std::string> paths;
    // The file's next page first, where it has pages: paging is browsing too.
    if (image_ && part_.page + 1 < image_->page_count)
        if (const std::string next = part_key(image_path_, {part_.page + 1, 0}); !cached_in_full(next))
            paths.push_back(next);
    const int n = folder_.count();
    if (n <= 1)
    {
        loader_.prefetch(std::move(paths));
        return;
    }
    auto add = [&](int offset)
    {
        int i = folder_.index() + offset;
        if (cfg_.wrap_around)
            i = ((i % n) + n) % n;
        else if (i < 0 || i >= n)
            return;
        if (i == folder_.index())
            return;
        const std::string path = folder_.path_at(i);
        if (!cached_in_full(path) && std::find(paths.begin(), paths.end(), path) == paths.end())
            paths.push_back(path);
    };
    // Flicking through (the wheel spun): far ahead the way it's going, a
    // good margin past a second at that pace (speeding up a little doesn't
    // catch it up), and nothing behind. Browsing slowly:
    // both ways alike, for flipping back and forth to compare.
    int direction = 1, ahead = cfg_.prefetch_ahead, behind = std::max(cfg_.prefetch_ahead, cfg_.prefetch_behind);
    if (flicking(&direction))
    {
        // Within half the cache, at what the image on screen takes.
        const size_t each = std::max<size_t>(image_ ? ImageCache::image_bytes(*image_) : 0, 32 << 20);
        const int room = int(cache_.budget() / 2 / each);
        // And at least one for every worker, so they're all busy from the start.
        const int pace = int(nav_steps_.size());  // steps in the last second
        const int wanted = std::max({pace * 3 / 2 + 2, int(loader_.workers()), cfg_.prefetch_ahead});
        ahead = std::clamp(wanted, cfg_.prefetch_ahead, std::max(cfg_.prefetch_ahead, std::min(32, room)));
        behind = 0;
    }
    else if (!nav_steps_.empty())
        direction = nav_steps_.back().direction;
    for (int k = 1; k <= ahead || k <= behind; ++k)
    {
        if (k <= ahead)
            add(k * direction);
        if (k <= behind)
            add(-k * direction);
    }
    if (tracing())
        std::fprintf(stderr, "prefetch: %d %s, %d behind%s\n", ahead, direction > 0 ? "ahead" : "back", behind, flicking() ? " (flicking)" : "");
    loader_.prefetch(std::move(paths));
}

// Three or more steps the same way in the last second.
bool App::flicking(int* direction) const
{
    const int64_t now = g_get_monotonic_time();
    int net = 0, n = 0;
    for (const NavStep& s : nav_steps_)
        if (now - s.us <= 1'000'000)
            net += s.direction, ++n;
    if (direction && net != 0)
        *direction = net > 0 ? 1 : -1;
    return n >= 3 && std::abs(net) == n;
}

void App::request_place(const std::shared_ptr<Image>& image)
{
    if (!image || !image->has_location || !image->place.empty())
        return;
    std::weak_ptr<Image> weak = image;
    geocoder_.lookup(image->latitude, image->longitude,
                     [this, weak](const std::string& place)
                     {
                         auto img = weak.lock();
                         if (!img)
                             return;
                         img->place = place.empty() ? " " : place;  // a space marks "looked up, nothing found"
                         if (img == image_)
                         {
                             update_status();
                             refresh_metadata();
                         }
                     });
}

void App::ensure_developed()
{
    if (!image_ || !image_->is_preview())
        return;
    if (image_->is_raw)
    {
        status_message_ = busy_message_ = _("Developing…");
        update_status();
    }
    loader_.request_develop(load_generation_, current_key());
}

void App::retone()
{
    if (!image_ || !image_->tonable())
        return;
    image_->exposure_edit = exposure_->value() - image_->exposure_bias;
    if (view_->drawn_on_gpu())
    {
        // Drawn from the image's own data: the view shows the new exposure,
        // and nothing reaches the CPU. Its 8-bit pixels are made again only
        // if something needs them (pixels_for_cpu).
        image_->pixels_stale = true;
        PixelBuffer().swap(image_->pixels);  // tens of megabytes it doesn't need
        image_->mips.clear();
        view_->redraw();
        return;
    }
    request_retone();
}

// Render the current exposure on a thread into a fresh buffer; swap it in on
// the UI thread. If the slider moved meanwhile, go again.
void App::request_retone()
{
    if (retone_busy_ || !image_ || !image_->tonable())
        return;
    if (image_->mosaic)
    {
        // Developed on the GPU: its pixels are made there, when asked for.
        image_->pixels_stale = true;
        PixelBuffer().swap(image_->pixels);
        view_->redraw();
        return;
    }
    retone_busy_ = true;
    auto image = image_;
    auto linear = image_->linear;
    auto source = image_->source;
    const double ev = image_->exposure_edit;
    std::thread(
        [this, image, linear, source, ev]
        {
            auto result = std::make_shared<PixelBuffer>(image->retone_buffer(ev));
            ui_.post(
                [this, image, linear, source, ev, result]
                {
                    retone_busy_ = false;
                    const bool current = image->linear == linear && image->source == source &&
                                         result->size() == size_t(image->width) * image->height * 4;
                    if (current)
                    {
                        image->pixels = std::move(*result);
                        if (ev != 0)
                            image->break_transform(_("the exposure was changed"));
                        image->mips.clear();
                        if (image == image_)
                            view_->retoned();
                    }
                    if (image == image_ && image_->tonable() && std::fabs(image_->exposure_edit - ev) > 1e-9)
                    {
                        if (view_->drawn_on_gpu())
                        {
                            image_->pixels_stale = true;  // the GPU shows it; made again when needed
                            PixelBuffer().swap(image_->pixels);
                        }
                        else
                            request_retone();  // the slider moved on while we rendered
                    }
                });
        })
        .detach();
}

// Orientation and crop change the pixels; exposure just moves the slider.
bool App::apply_xmp_geometry(Image& image)
{
    const XmpDevelop d = image.xmp;
    if (d.has_crop)
    {
        const int x = int(d.crop_left * image.width), y = int(d.crop_top * image.height);
        const int w = int((d.crop_right - d.crop_left) * image.width), h = int((d.crop_bottom - d.crop_top) * image.height);
        if (w > 1 && h > 1)
            image = image.crop(x, y, w, h);
    }
    if (d.orientation != 1)
        image.apply_orientation(d.orientation);
    return d.has_crop || d.orientation != 1;
}

void App::apply_xmp(bool fresh)
{
    if (!image_ || !image_->xmp.present)
        return;
    if (fresh)
        apply_xmp_geometry(*image_);
    image_->exposure_edit = image_->xmp.exposure;
    if (image_->tonable())
        image_->retone(image_->exposure_edit);
    status_message_ = "XMP edits applied";
}

// -- window -------------------------------------------------------------------
// Size the window so the image shows 1:1 when it fits on the monitor,
// but never smaller than the configured minimum and never larger than
// the work area. Where the platform lets us, keep it on screen.
void App::fit_window_to_image()
{
    // A maximized or snapped window has the size the user gave it.
    if (!image_ || fullscreen_ || !cfg_.fit_window_to_image || !window_ || window_->maximized())
        return;
    rap::rect_f work = window_->work_area();
    if (work.w <= 0 || work.h <= 0)
        work = {0, 0, 1920, 1080};
    // Chrome: what the window holds besides the image area.
    int vw = 0, vh = 0;
    view_->view_size(vw, vh);
    const double scale = view_->scale_factor();
    const rap::size_f cur = window_->size();
    const float pane_w = metadata_visible_ ? float(cfg_.metadata_pane_width) : 0.f;
    const int images = side_by_side_ ? 2 : 1;  // side by side, B's pane is as wide as A's
    const float chrome_w = vw > 0 ? std::max(0.f, cur.w - images * float(vw / scale) - pane_w) : 0.f;
    const float chrome_h = vh > 0 ? std::max(0.f, cur.h - float(vh / scale)) : 80.f;
    // Not laid out yet (GTK sizes nothing before the window shows): the
    // chrome was guessed, so fit again once the view has a size.
    refit_when_laid_out_ = vw <= 0 || vh <= 0;
    fitted_scale_ = scale;

    // Leave room for the title bar and a little air around the window.
    const float max_w = work.w - 24, max_h = work.h - 72;
    const bool stand_in = image_->final_width > 0 && image_->final_height > 0;
    const double iw = (stand_in ? image_->final_width : image_->width) / scale;  // logical pixels at 1:1
    const double ih = (stand_in ? image_->final_height : image_->height) / scale;
    // A page that renders at any size fills the screen its way up; pixels
    // stop at 1:1.
    const double cap = image_->vector ? HUGE_VAL : 1.0;
    const double s = std::min({cap, (max_w - chrome_w - pane_w) / (images * iw), (max_h - chrome_h) / ih});
    const float cw = images * float(std::ceil(iw * s)), ch = float(std::ceil(ih * s));
    const float w = std::clamp(cw + chrome_w + pane_w, float(cfg_.min_window_width), max_w);
    const float h = std::clamp(ch + chrome_h, float(cfg_.min_window_height), max_h);
    if (std::fabs(cur.w - w) < 1 && std::fabs(cur.h - h) < 1)
        return;
    if (auto pos = window_->position())
    {
        // Growth keeps the top-left corner; an edge past the work area pushes back.
        float x = pos->x, y = pos->y;
        if (x + w > work.x + work.w)
            x = work.x + work.w - w;
        if (y + h > work.y + work.h)
            y = work.y + work.h - h;
        x = std::max(x, work.x);
        y = std::max(y, work.y);
        if (x != pos->x || y != pos->y)
            window_->move({x, y});
    }
    window_->resize({w, h});
}

void App::toggle_fullscreen()
{
    window_->set_fullscreen(!fullscreen_);
}

void App::on_fullscreen_changed(bool fs)
{
    if (fs == fullscreen_)
        return;
    fullscreen_ = fs;
    if (!fs && slideshow_)
        stop_slideshow();
    toolbar_->set_visible(!fs);
    footer_->set_visible(!fs);
    // The info pane gives way to the fullscreen overlay, and comes back after.
    if (fs)
    {
        pane_before_fullscreen_ = metadata_visible_;
        metadata_pane_->set_visible(false);
    }
    else
    {
        if (pane_before_fullscreen_)
            metadata_pane_->set_visible(true);
        update_title();
        exposure_box_->set_visible(exposure_applies());
    }
    update_histogram();
    view_->set_fullscreen(fs);
    if (fs && slideshow_)
        ui_.post([this] { slideshow_start_slide(); });  // after the fullscreen layout, so the motion sees the real size
}

void App::zoom_in()
{
    view_->zoom_step(+1);
}
void App::zoom_out()
{
    view_->zoom_step(-1);
}
void App::zoom_original()
{
    view_->zoom_original();
}
void App::zoom_fit()
{
    view_->zoom_fit();
}

void App::open_dialog()
{
    rap::file_dialog_spec spec;
    spec.mode = rap::file_dialog_spec::mode::open;
    spec.title = _("Open Image");
    spec.initial_path = folder_.directory();
    std::string patterns;
    for (const std::string& e : cfg_.extensions)
        patterns += (patterns.empty() ? "*." : ";*.") + e;
    spec.filters = {{_("Images"), patterns}, {_("All files"), "*"}};
    ui_.choose_files(window_.get(), spec,
                    [this](std::vector<std::string> files)
                    {
                        if (!files.empty())
                            open_path(files.front());
                    });
}

void App::open_settings_page(int page)
{
    const std::string old_raw_mode = cfg_.raw_mode;
    const bool old_apply_xmp = cfg_.apply_xmp_edits;
    const bool old_raw_crop = cfg_.raw_camera_crop;
    end_settings_ = open_settings_dialog(
        ui_, window_.get(), cfg_,
        [this, old_raw_mode, old_apply_xmp, old_raw_crop](bool ok)
        {
            end_settings_ = nullptr;
            if (!ok)
                return;
            cfg_.save();
            set_decode_options(decode_options_for(cfg_));
            cache_.set_budget(size_t(cfg_.cache_megabytes) << 20);
            geocoder_.set_provider(cfg_.geocoder_url);
            if (image_)
                request_place(image_);
            video_.set_loop(cfg_.video_loop);
            video_.set_muted(cfg_.video_muted);
            view_->set_video_muted(cfg_.video_muted);
            folder_.rescan(cfg_);
            smooth_toggle_->set_checked(cfg_.smooth_upscale());
            xmp_toggle_->set_checked(cfg_.apply_xmp_edits);
            window_->set_min_size({float(cfg_.min_window_width), float(cfg_.min_window_height)});
            view_->config_changed();
            refresh_metadata();
            const bool reload =
                ((cfg_.raw_mode != old_raw_mode || cfg_.raw_camera_crop != old_raw_crop) && image_ && image_->is_raw) ||
                (cfg_.apply_xmp_edits != old_apply_xmp && image_ && image_->xmp.present);
            if (reload)
            {
                cache_.erase(image_path_);
                image_.reset();
                load_current();
            }
            update_status();
        },
        page);
}

// -- status -------------------------------------------------------------------
void App::update_title()
{
    // In fullscreen the title is invisible, and under some compositors a
    // title change re-activates the window, stealing focus.
    if (fullscreen_ || !window_)
        return;
    std::string title = "IfritView";
    if (diff_session_)
        title = diff_name_ + " – IfritView";
    else if (!folder_.empty())
        title = folder_.current_name() + " (" + std::to_string(folder_.index() + 1) + "/" + std::to_string(folder_.count()) + ") – IfritView";
    window_->set_title(title);
}

void App::update_status()
{
    if (!status_left_)
        return;
    std::string left, center, right;
    if (!folder_.empty())
        left = folder_.current_name() + "  (" + std::to_string(folder_.index() + 1) + " / " + std::to_string(folder_.count()) + ")";
    if (!status_message_.empty())
        left = status_message_;
    if (image_)
    {
        center = std::to_string(image_->width) + " × " + std::to_string(image_->height) + " × " + (image_->has_alpha ? "32" : "24") + " bpp";
        if (image_->bits_per_sample != 8)
            center += " (" + std::to_string(image_->bits_per_sample) + "-bit)";
        if (!image_->format.empty())
            center += "  ·  " + image_->format;
        if (file_size_)
            center += "  ·  " + human_size(file_size_);
        if (!image_->info.empty() && !image_->is_video)
            center += "  ·  " + image_->info;
        // Which of the file's images: its page (and name), its mip level.
        if (image_->page_count > 1)
        {
            center += "  ·  " + rap::i18n::format(_("page {0} / {1}"), image_->part.page + 1, image_->page_count);
            if (!image_->page_label.empty())
                center += " " + image_->page_label;
        }
        if (image_->level_count > 1)
            center += "  ·  " + rap::i18n::format(_("mip {0} / {1}"), image_->part.level, image_->level_count - 1);
        if (image_->is_preview() && status_message_.empty())
            center += std::string("  ·  ") + (image_->is_raw ? _("embedded preview") : _("preview"));
        // Against 1:1, which for a page is its actual size.
        const double shown = (image_->final_width > 0 ? view_->zoom() * image_->width / image_->final_width : view_->zoom()) / view_->natural_zoom();
        right = std::to_string(int(std::lround(shown * 100.0))) + "%";
        if (image_->is_video && video_.active() && video_.duration() > 0)
        {
            auto clock = [](double t)
            {
                const int v = int(t + 0.5);
                char buf[32];
                std::snprintf(buf, sizeof buf, "%d:%02d", v / 60, v % 60);
                return std::string(buf);
            };
            right = clock(video_.position()) + " / " + clock(video_.duration()) + (cfg_.video_muted && image_->has_audio ? std::string("  ·  ") + _("muted") : std::string()) + "  ·  " + right;
        }
        if (view_->zoom_mode() == View::ZoomMode::Fit && view_->zoom() < 1.0)
            right += std::string(" ") + _("(fit)");
        if (int x, y, w, h; view_->selection(x, y, w, h))
            right = rap::i18n::format(_("selection {0} × {1} at {2}, {3}"), w, h, x, y) + "  ·  " + right;
        if (const std::shared_ptr<Image> b = view_->compare_image(true); b && (b->width != image_->width || b->height != image_->height))
            center += "  ·  " + rap::i18n::format(_("B {0} × {1}, scaled to match"), b->width, b->height);
        if (const ImageDifference* d = view_->compare_difference())
        {
            std::string text;
            if (!d->summary.empty())
                text = d->summary;
            else if (d->max_error == 0)
                text = _("identical");
            else
            {
                char psnr[32], changed[32];
                std::snprintf(psnr, sizeof psnr, "%.1f", d->psnr);
                std::snprintf(changed, sizeof changed, "%.1f", d->changed * 100.0);
                // xgettext:no-c-format
                text = rap::i18n::format(_("PSNR {0} dB  ·  largest error {1}  ·  {2}% changed"), psnr, d->max_error, changed);
            }
            center += "  ·  " + text;
        }
    }
    if (image_ && image_->has_location)
    {
        const std::string place = image_->place == " " ? "" : image_->place;
        location_url_ = cfg_.map_link(image_->latitude, image_->longitude);
        status_location_->set_text(image_->location_text() + (place.empty() ? "" : "  ·  " + place));
        status_location_->set_visible(true);
    }
    else
    {
        status_location_->set_visible(false);
    }
    const bool busy = !busy_message_.empty() && status_message_ == busy_message_;
    view_->set_busy(busy);
    view_->set_overlay(overlay_sections());
    status_left_->set_text(left);
    status_center_->set_text(center);
    status_right_->set_text(right);
}

