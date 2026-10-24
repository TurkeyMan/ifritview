#include "app.h"

#include "platform.h"
#include "save.h"
#include "settings.h"

#include "decoders/decoder.h"
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

namespace
{

int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool tracing()
{
    return g_getenv("IFRIT_TRACE") != nullptr;
}

DecodeOptions decode_options_for(const Config& cfg)
{
    DecodeOptions o;
    using RawMode = DecodeOptions::RawMode;
    o.raw_mode = cfg.raw_mode == "full" ? RawMode::Full : cfg.raw_mode == "preview" ? RawMode::Preview : RawMode::Half;
    o.raw_camera_crop = cfg.raw_camera_crop;
    return o;
}

std::string human_size(uint64_t bytes)
{
    gchar* s = g_format_size(bytes);
    std::string r = s;
    g_free(s);
    return r;
}

std::string file_time(const std::string& path, const char* format)
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

std::mt19937& rng()
{
    static std::mt19937 r{std::random_device{}()};
    return r;
}

double random01()
{
    return std::uniform_real_distribution<double>(0, 1)(rng());
}

}  // namespace

App::App(rap::ui& ui) : ui_(ui), loader_([this](Loader::Result& r) { on_loaded(r); }, [&ui](std::function<void()> fn, int) { ui.post(std::move(fn)); }), cache_(0)
{
    cfg_.load();
    cache_.set_budget(size_t(cfg_.cache_megabytes) << 20);
    geocoder_.set_provider(cfg_.geocoder_url);
    set_decode_options(decode_options_for(cfg_));
    build_ui();
}

App::~App()
{
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

void App::set_compare(View::Compare mode)
{
    if (mode == View::Compare::Difference && !view_->compare_difference())
        mode = view_->compare_mode();  // not for images of different sizes
    view_->set_compare(mode);
    update_toolbar();  // the buttons act as one choice: unchecking the shown one keeps it
    update_status();
}

void App::swap_compare()
{
    view_->swap_compare();
    show_side();
    update_toolbar();
    update_status();
}

// Back to browsing: the view shows the image again, alone.
void App::end_comparing()
{
    set_side_by_side(false);
    view_->end_compare();
    if (diff_session_)
    {
        diff_session_ = false;
        status_message_.clear();
    }
    update_toolbar();
    update_status();
    update_title();
}

void App::show_side()
{
    if (!side_by_side_)
        return;
    side_view_->set_image(view_->compare_image(true));
    side_view_->set_label(view_->compare_label(true));
    syncing_framing_ = true;
    side_view_->set_framing(view_->framing());
    syncing_framing_ = false;
}

// The side pane opens to the right of the image, the window growing by as
// much (as for the info pane), so the image keeps its size.
void App::set_side_by_side(bool on)
{
    if (on && !view_->comparing())
        on = false;
    if (side_button_)
        side_button_->set_checked(on);
    if (on == side_by_side_)
        return;
    int vw = 0, vh = 0;
    view_->view_size(vw, vh);
    const rap::size_f before = window_->size();
    const bool sized = !fullscreen_ && !window_->maximized();
    side_by_side_ = on;
    side_pane_->set_visible(on);
    side_settle_.reset();
    if (on)
    {
        if (view_->compare_mode() == View::Compare::B)
            view_->set_compare(View::Compare::A);  // B is beside it now
        show_side();
        if (sized)
        {
            // Wider by a pane (and the gap, a guess until measured below), and
            // back to exactly this width when it closes.
            side_restore_w_ = before.w;
            float w = before.w + float(vw / view_->scale_factor()) + 6;
            const rap::rect_f work = window_->work_area();
            if (work.w > 0)
                w = std::min(w, work.w - 24);
            window_->resize({w, before.h});
            side_settle_ = SideSettle{vw, w};
        }
    }
    else
    {
        side_view_->set_image(nullptr);
        side_view_->set_label({});
        if (sized && side_restore_w_ > 0)
            window_->resize({side_restore_w_, before.h});
        side_restore_w_ = 0;
    }
    update_toolbar();
    update_status();
}

// Once the side pane is laid out at the new width: each pane as wide as the
// image area was, correcting the guessed gap between them.
void App::settle_side_by_side()
{
    if (!side_settle_ || std::fabs(window_->size().w - side_settle_->window_w) >= 1)
        return;  // not yet at the width asked for
    const SideSettle settle = *side_settle_;
    side_settle_.reset();
    int vw = 0, vh = 0;
    view_->view_size(vw, vh);
    const float short_by = float((settle.view_w - vw) / view_->scale_factor());
    const rap::rect_f work = window_->work_area();
    const float w = settle.window_w + 2 * short_by;  // the panes share the width equally
    if (std::fabs(short_by) >= 1 && (work.w <= 0 || w <= work.w - 24))
        window_->resize({w, window_->size().h});
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
    view_ = std::make_unique<View>(ui_, cfg_);
    side_view_ = std::make_unique<View>(ui_, cfg_);
    namespace L = rap::layout;
    using L::hbox;
    using L::horizontal;
    using L::separator;
    using L::spacer;
    using L::vbox;

    // Exposure, in stops around the camera's compensation; raw only.
    rap::node exposure =
        hbox{
            L::label{"EV"},
            L::slider{-3.0, 3.0, 0.0}
                .step(0.1, 1.0)
                .show_value(2)
                .mark(0.0)
                .on_changed([this](double) { retone(); })
                .bind(exposure_)
                .width(rap::extent{24, rap::extent::unit::chars})
                .tooltip("Exposure (EV). Rests at the camera's compensation, i.e. as shot.\n"
                         "Greyed while the embedded preview stands in for the raw"),
        }
            .handle(exposure_box_);

    window_ = ui_.create(L::window{
        "IfritView",
        {vbox{
            L::toolbar{
                // Always: open and save, zoom, fullscreen.
                tool_button("document-open-symbolic", "Open… (Ctrl+O)", &App::open_dialog),
                tool_button("media-floppy-symbolic", "Save as… (Ctrl+S); comparing, saves the difference shown", &App::save_as),
                separator{},
                tool_button("zoom-out-symbolic", "Zoom out (-, Ctrl+wheel)", &App::zoom_out),
                tool_button("zoom-in-symbolic", "Zoom in (+, Ctrl+wheel)", &App::zoom_in),
                tool_button("zoom-original-symbolic", "Actual pixels (1)", &App::zoom_original),
                tool_button("zoom-fit-best-symbolic", "Fit to window (F)", &App::zoom_fit),
                L::tool_button{rap::icon::named("view-grid-symbolic"), "Smooth when zoomed in (off shows crisp pixels)"}
                    .toggle(cfg_.smooth_upscale)
                    .on_toggled(
                        [this](bool on)
                        {
                            cfg_.smooth_upscale = on;
                            cfg_.save();
                            view_->config_changed();
                        })
                    .bind(smooth_toggle_),
                tool_button("view-fullscreen-symbolic", "Fullscreen (Enter, double-click)", &App::toggle_fullscreen),
                separator{},
                // Browsing: through the folder, and what's done to an image.
                L::tool_button{rap::icon::named("go-previous-symbolic"), "Previous image (Left, wheel up)"}
                    .on_click([this] { navigate(-1); })
                    .handle(browse_widgets_[0]),
                L::tool_button{rap::icon::named("go-next-symbolic"), "Next image (Right, wheel down)"}
                    .on_click([this] { navigate(+1); })
                    .handle(browse_widgets_[1]),
                separator{}.handle(browse_widgets_[2]),
                L::tool_button{rap::icon::named("media-playback-start-symbolic"), "Slideshow (S): fullscreen, timed, with gentle motion"}
                    .on_click([this] { toggle_slideshow(); })
                    .handle(browse_widgets_[3]),
                L::tool_button{rap::icon::named("edit-cut-symbolic"), "Crop to selection (Ctrl+Y)"}
                    .on_click([this] { crop_to_selection(); })
                    .bind(crop_button_)
                    .handle(browse_widgets_[4]),
                L::tool_button{rap::icon::named("document-edit-symbolic"), "Apply edits from XMP sidecars (exposure, crop, rotation)"}
                    .toggle(cfg_.apply_xmp_edits)
                    .on_toggled([this](bool) { toggle_xmp(); })
                    .bind(xmp_toggle_)
                    .handle(browse_widgets_[5]),
                // Comparing two images, in place of browsing.
                compare_button(a_icon(), "Show A (Tab flips A and B)", View::Compare::A),
                compare_button(b_icon(), "Show B (Tab flips A and B)", View::Compare::B),
                compare_button(swipe_icon(), "Swipe: A left of the divider, B right of it (drag the divider)", View::Compare::Swipe),
                compare_button(difference_icon(), "Difference: where A and B differ, amplified", View::Compare::Difference),
                L::tool_button{swap_icon(), "Swap A and B"}.on_click([this] { swap_compare(); }).handle(diff_widgets_[0]),
                L::tool_button{side_by_side_icon(), "Side by side: B in a second pane, beside A"}
                    .toggle(false)
                    .on_toggled([this](bool on) { set_side_by_side(on); })
                    .bind(side_button_)
                    .handle(diff_widgets_[1]),
                spacer{},
                exposure,
                hbox{
                    L::label{"Gain"},
                    L::slider{1.0, 32.0, 8.0}
                        .step(1.0, 4.0)
                        // No value shown: the view's tag has it ("Difference ×8"), and
                        // GTK would put it above the track, making the toolbar taller.
                        .on_changed([this](double v) { view_->set_compare_gain(int(v + 0.5)); })
                        .bind(gain_)
                        .width(rap::extent{14, rap::extent::unit::chars})
                        .tooltip("How much the difference is amplified"),
                }
                    .handle(diff_widgets_[2]),
                tool_button("dialog-information-symbolic", "Metadata / EXIF (I)", &App::toggle_metadata),
                tool_button("preferences-system-symbolic", "Settings (Ctrl+P)", &App::open_settings),
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
                    L::list{{{"Tag", rap::extent{14, rap::extent::unit::chars}}, {"Value", {}, true}}, &metadata_model_}
                        .selection(rap::selection_mode::single)
                        .on_activate(
                            [this](size_t row)
                            {
                                const std::string url = metadata_model_.url(row);
                                if (!url.empty())
                                    open_url(url);
                            })
                        .bind(metadata_list_)
                        .stretch(),
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
    view_->attach(view_->canvas());
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
    status_location_->set_tooltip("Open in maps");

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
    view_->on_zoom_changed = [this] { update_status(); };
    view_->on_presented = [this] { image_presented(); };
    view_->on_resized = [this]
    {
        if (refit_after_layout_)
            refit_after_pane_toggle();
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
        static int last_w = 0, last_h = 0;  // a rotated stream can change shape after the poster
        if (w != last_w || h != last_h)
            fit_window_to_image();
        last_w = w;
        last_h = h;
    };
    view_->on_selection_changed = [this]
    {
        int x, y, w, h;
        crop_button_->set_enabled(view_->selection(x, y, w, h));
    };
    update_status();
    window_->show();
    view_->canvas()->focus();
}

void App::open_url(const std::string& url)
{
    if (!url.empty())
        g_app_info_launch_default_for_uri(url.c_str(), nullptr, nullptr);
}

// Ctrl+C: the selection if there is one, otherwise the whole image. The
// selection is dropped afterwards as the visible sign that it was copied.
void App::copy_to_clipboard()
{
    if (!image_)
        return;
    int x, y, w, h;
    const Image region = view_->selection(x, y, w, h) ? image_->crop(x, y, w, h) : *image_;
    ui_.set_clipboard_image(region.width, region.height, APixelFormat::bgra8_premultiplied, region.pixels.data(), region.stride);
    view_->clear_selection();
    status_message_ = "Copied " + std::to_string(region.width) + " × " + std::to_string(region.height) + " to clipboard";
    update_status();
}

void App::transform(int exif_orientation)
{
    if (!image_)
        return;
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

// -- slideshow ----------------------------------------------------------------
void App::toggle_slideshow()
{
    if (slideshow_)
    {
        stop_slideshow();
        return;
    }
    if (folder_.empty() || navigation_locked())
        return;
    slideshow_ = true;
    slideshow_order_.clear();
    if (cfg_.slideshow_shuffle)
    {
        for (int i = 0; i < folder_.count(); ++i)
            slideshow_order_.push_back(i);
        std::shuffle(slideshow_order_.begin(), slideshow_order_.end(), rng());
        slideshow_pos_ = 0;
        if (folder_.go(slideshow_order_[0]))
            load_current();
    }
    if (!fullscreen_)
        toggle_fullscreen();  // the first slide starts once fullscreen has landed
    else
        slideshow_start_slide();
}

void App::stop_slideshow()
{
    outgoing_video_.stop();
    audio_fade_timer_.reset();
    video_.set_volume(cfg_.video_volume);
    slideshow_ = false;
    slide_.active = false;
    slideshow_timer_.reset();
    view_->end_motion();
    view_->zoom_fit();
}

// One slide: a random gentle framing change over the slide's duration, then
// on to the next image.
void App::slideshow_start_slide()
{
    if (!slideshow_)
        return;
    int ms = int(cfg_.slideshow_seconds * 1000);
    if (image_ && image_->is_video && image_->duration > 0)
    {
        // A video plays to its end; the next slide fades in over its last moments.
        const double played = video_.active() ? video_.position() : 0.0;
        ms = std::max(500, int((image_->duration - played) * 1000) - cfg_.slideshow_crossfade_ms);
    }
    slide_.active = image_ && !image_->is_video && cfg_.slideshow_motion && cfg_.slideshow_zoom > 0;
    if (tracing())
        std::fprintf(stderr, "slide: %s for %d ms\n", folder_.current_name().c_str(), ms);
    slide_.start_us = now_us();
    slide_.duration_ms = ms;
    if (!slide_.active)
    {
        view_->end_motion();
        view_->zoom_fit();
    }
    else
    {
        const bool in = random01() < 0.5;
        const double lo = 1.0 + cfg_.slideshow_zoom * 0.15, hi = 1.0 + cfg_.slideshow_zoom;
        slide_.z0 = in ? lo : hi;
        slide_.z1 = in ? hi : lo;
        slide_.fx0 = random01();
        slide_.fy0 = random01();
        slide_.fx1 = random01();
        slide_.fy1 = random01();
        apply_slide_motion();
    }
    slideshow_timer_ = ui_.timer(ms, false, [this] { slideshow_advance(); });
}

// Translate the slide's relative motion into this image's coordinates. The
// viewport is kept inside the image.
void App::apply_slide_motion()
{
    if (!slide_.active || !image_)
        return;
    int vw, vh;
    view_->view_size(vw, vh);
    const double w = image_->width, h = image_->height;
    const double fit = std::min(vw / w, vh / h);
    auto centre = [&](double z, double fx, double fy, double& cx, double& cy)
    {
        const double half_w = vw / (2 * z), half_h = vh / (2 * z);
        const double lo_x = std::min(half_w, w - half_w), hi_x = std::max(half_w, w - half_w);
        const double lo_y = std::min(half_h, h - half_h), hi_y = std::max(half_h, h - half_h);
        cx = lo_x + (hi_x - lo_x) * fx;
        cy = lo_y + (hi_y - lo_y) * fy;
    };
    const double z0 = fit * slide_.z0, z1 = fit * slide_.z1;
    double cx0, cy0, cx1, cy1;
    centre(z0, slide_.fx0, slide_.fy0, cx0, cy0);
    centre(z1, slide_.fx1, slide_.fy1, cx1, cy1);
    view_->begin_motion(z0, z1, cx0, cy0, cx1, cy1, slide_.duration_ms, slide_.start_us);
}

void App::slideshow_advance()
{
    if (!slideshow_)
        return;
    if (tracing())
        std::fprintf(stderr, "slideshow: last frame cost %.1f ms\n", view_->frame_ms());
    view_->crossfade_next(cfg_.slideshow_crossfade_ms);
    if (cfg_.slideshow_crossfade_ms > 0 && image_ && image_->is_video && video_.active())
    {
        outgoing_video_.stop();
        outgoing_video_.swap(video_);  // it plays on under the fade
        view_->crossfade_live();
    }
    if (cfg_.slideshow_crossfade_ms > 0)
        start_audio_fade(cfg_.slideshow_crossfade_ms);
    if (!slideshow_order_.empty())
    {
        slideshow_pos_ = (slideshow_pos_ + 1) % slideshow_order_.size();
        if (folder_.go(slideshow_order_[slideshow_pos_]))
            load_current();
    }
    else
    {
        if (!folder_.step(+1, true))
            return;
        load_current();
    }
}

// -- video --------------------------------------------------------------------
void App::play_video()
{
    if (!image_ || !image_->is_video)
        return;
    video_.start(image_path_, image_, cfg_.video_muted, cfg_.video_loop && !slideshow_);
    video_.set_volume(audio_fade_timer_ ? 0.0 : cfg_.video_volume);  // during a cross-fade it fades in
    // Position for the knob and the status bar, a few times a second.
    video_timer_ = ui_.timer(250, true,
                             [this]
                             {
                                 if (!video_.active())
                                 {
                                     video_timer_.reset();
                                     return;
                                 }
                                 if (video_.duration() > 0)
                                     view_->set_video_progress(video_.position() / video_.duration(), video_.playing());
                                 if (volume_dirty_)
                                 {
                                     volume_dirty_ = false;
                                     cfg_.save();
                                 }
                                 update_status();
                             });
}

void App::toggle_video()
{
    if (!video_.active())
        play_video();
    else
        video_.toggle();
    if (video_.duration() > 0)
        view_->set_video_progress(video_.position() / video_.duration(), video_.playing());
    update_status();
}

// Over `ms`, the outgoing video's sound falls to silence and the incoming
// one's rises to the set volume; then the outgoing playback ends.
void App::start_audio_fade(int ms)
{
    outgoing_video_.on_frame = [this] { view_->outgoing_changed(); };
    audio_fade_start_ = now_us();
    audio_fade_ms_ = ms;
    if (audio_fade_timer_)
        return;
    audio_fade_timer_ = ui_.timer(16, true,
                                  [this]
                                  {
                                      const double t = std::clamp((now_us() - audio_fade_start_) / (audio_fade_ms_ * 1000.0), 0.0, 1.0);
                                      outgoing_video_.set_volume(cfg_.video_volume * (1 - t));
                                      video_.set_volume(cfg_.video_volume * t);
                                      if (t < 1.0)
                                          return;
                                      outgoing_video_.stop();
                                      audio_fade_timer_.reset();
                                  });
}

void App::toggle_mute()
{
    cfg_.video_muted = !cfg_.video_muted;
    cfg_.save();
    video_.set_muted(cfg_.video_muted);
    view_->set_video_muted(cfg_.video_muted);
    update_status();
}

void App::stop_video()
{
    video_.stop();
    video_timer_.reset();
}

// -- animation ----------------------------------------------------------------
// Frames are swapped into the image's pixel buffer in place, so everything
// else (zoom, selection, save, copy) sees the current frame.
void App::show_animation_frame(size_t index)
{
    if (!image_ || !image_->animation || image_->animation->frames.empty())
        return;
    const Animation::Frame& f = image_->animation->frames[index % image_->animation->frames.size()];
    if (f.pixels.size() == image_->pixels.size())
    {
        std::memcpy(image_->pixels.data(), f.pixels.data(), f.pixels.size());
        image_->mips.clear();
        view_->image_changed();
    }
}

void App::schedule_animation_frame()
{
    const int delay = std::max(10, image_->animation->frames[animation_frame_].delay_ms);
    animation_timer_ = ui_.timer(delay, false,
                                 [this]
                                 {
                                     if (!animation_playing_ || !image_ || !image_->animation)
                                         return;
                                     animation_frame_ = (animation_frame_ + 1) % image_->animation->frames.size();
                                     show_animation_frame(animation_frame_);
                                     schedule_animation_frame();
                                 });
}

void App::start_animation()
{
    stop_animation();
    if (!image_ || !image_->animation || image_->animation->frames.size() < 2)
        return;
    animation_playing_ = true;
    animation_frame_ = 0;
    show_animation_frame(0);
    schedule_animation_frame();
}

void App::stop_animation()
{
    animation_timer_.reset();
    animation_playing_ = false;
}

void App::toggle_animation()
{
    if (!image_ || !image_->animation)
        return;
    if (animation_playing_)
    {
        stop_animation();
        status_message_ = "Animation paused";
    }
    else
    {
        animation_playing_ = true;
        schedule_animation_frame();
        status_message_.clear();
    }
    update_status();
}

// -- metadata -----------------------------------------------------------------
void App::remember_pane_width()
{
    const std::vector<float> sizes = splitter_->sizes();
    if (!metadata_visible_ || sizes.size() < 2 || sizes[1] <= 0)
        return;
    const int width = std::max(240, int(std::lround(sizes[1])));
    if (width == cfg_.metadata_pane_width)
        return;
    if (pane_extra_ > 0)
        pane_extra_ += float(width - cfg_.metadata_pane_width);  // the divider's share is unchanged
    cfg_.metadata_pane_width = width;
    cfg_.save();
}

void App::toggle_metadata()
{
    const rap::size_f before = window_->size();
    metadata_visible_ = !metadata_visible_;
    metadata_pane_->set_visible(metadata_visible_);
    if (metadata_visible_)
    {
        splitter_->set_sizes({std::max(100.f, before.w), float(cfg_.metadata_pane_width)});
        refresh_metadata();
    }
    // The window takes (or gives back) what the pane adds in the same step,
    // so the first frame is laid out at the final width: laying the pane into
    // the old width first would show its divider mid-image for a frame.
    pane_toggled_from_ = 0;
    if (pane_extra_ <= 0)
        pane_extra_ = float(cfg_.metadata_pane_width) + 6;  // until measured: the pane and a divider
    if (image_ && !fullscreen_ && cfg_.fit_window_to_image)
    {
        float w = before.w + (metadata_visible_ ? pane_extra_ : -pane_extra_);
        const rap::rect_f work = window_->work_area();
        if (work.w > 0)
            w = std::min(w, work.w - 24);
        window_->resize({std::max(w, float(cfg_.min_window_width)), before.h});
        pane_toggled_from_ = before.w;
    }
    // Then the exact fit, measured once the toolkit has laid the pane in or
    // out (it does that later, not here), or shortly if the view's size
    // didn't change.
    refit_after_layout_ = true;
    refit_timer_ = ui_.timer(150, false,
                             [this]
                             {
                                 if (refit_after_layout_)
                                     refit_after_pane_toggle();
                             });
}

void App::refit_after_pane_toggle()
{
    refit_after_layout_ = false;
    fit_window_to_image();
    // Learn exactly what the pane adds (its width and the divider), for next time.
    const float now = window_->size().w;
    const rap::rect_f work = window_->work_area();
    const bool limited = work.w > 0 && now >= work.w - 25;
    if (pane_toggled_from_ > 0 && !limited)
    {
        const float extra = metadata_visible_ ? now - pane_toggled_from_ : pane_toggled_from_ - now;
        if (extra > 0)
            pane_extra_ = extra;
    }
    pane_toggled_from_ = 0;
}

std::vector<MetadataGroup> App::collect_metadata() const
{
    std::vector<MetadataGroup> groups;
    if (!image_)
        return groups;
    MetadataGroup file{"File", {}};
    file.items.push_back({"Name", folder_.current_name()});
    file.items.push_back({"Folder", folder_.directory()});
    if (file_size_)
        file.items.push_back({"Size", human_size(file_size_) + " (" + std::to_string(file_size_) + " bytes)"});
    if (const std::string t = file_time(image_path_, "%Y-%m-%d %H:%M:%S"); !t.empty())
        file.items.push_back({"Modified", t});
    groups.push_back(std::move(file));
    MetadataGroup img{"Image", {}};
    img.items.push_back({"Dimensions", std::to_string(image_->width) + " × " + std::to_string(image_->height)});
    img.items.push_back({"Format", image_->format});
    if (!image_->pixel_format.empty())
        img.items.push_back({"Pixel format", image_->pixel_format.describe(image_->width, image_->height)});
    if (!image_->color_space.empty())
        img.items.push_back({"Colour space", image_->color_space});
    img.items.push_back({"Source depth", std::to_string(image_->bits_per_sample) + " bits per sample"});
    img.items.push_back({"Alpha", image_->has_alpha ? "yes" : "no"});
    if (!image_->info.empty())
        img.items.push_back({"Notes", image_->info});
    groups.push_back(std::move(img));
    for (const MetadataGroup& g : image_->metadata)
        groups.push_back(g);
    if (image_->has_location)
    {
        MetadataGroup* gps = nullptr;
        for (MetadataGroup& g : groups)
            if (g.name == "GPS")
                gps = &g;
        if (!gps)
        {
            groups.push_back({"GPS", {}});
            gps = &groups.back();
        }
        if (!image_->place.empty() && image_->place != " ")
            gps->items.insert(gps->items.begin(), {"Place", image_->place});
    }
    return groups;
}

// The fullscreen HUD, assembled by category from what the decoders found.
View::OverlaySections App::overlay_sections() const
{
    View::OverlaySections sections;
    if (!image_)
        return sections;
    std::vector<std::string> lines;
    auto flush = [&]
    {
        if (!lines.empty())
            sections.push_back(std::move(lines));
        lines.clear();
    };
    auto find = [&](std::initializer_list<const char*> groups, std::initializer_list<const char*> keys) -> std::string
    {
        for (const MetadataGroup& g : image_->metadata)
            for (const char* gn : groups)
                if (g.name == gn)
                    for (const char* k : keys)
                        for (const MetadataItem& m : g.items)
                            if (m.key == k)
                                return m.value;
        return "";
    };
    if (cfg_.overlay_file)
    {
        lines.push_back(folder_.current_name() + "  (" + std::to_string(folder_.index() + 1) + "/" + std::to_string(folder_.count()) + ")");
        std::string line = file_size_ ? human_size(file_size_) : "";
        if (const std::string t = file_time(image_path_, "%Y-%m-%d %H:%M"); !t.empty())
            line += std::string(line.empty() ? "" : "  ·  ") + t;
        if (!line.empty())
            lines.push_back(line);
    }
    flush();
    if (cfg_.overlay_format)
    {
        lines.push_back(std::to_string(image_->width) + " × " + std::to_string(image_->height) + "  ·  " + image_->format);
        if (!image_->pixel_format.empty())
            lines.push_back(image_->pixel_format.describe(image_->width, image_->height) + (image_->color_space.empty() ? "" : "  ·  " + image_->color_space));
        const std::string compression = find({"TIFF"}, {"Compression"});
        if (!compression.empty())
            lines.push_back("compression " + compression);
    }
    flush();
    if (cfg_.overlay_shot)
    {
        const std::string make = find({"Camera", "EXIF"}, {"Make", "Manufacturer"});
        const std::string model = find({"Camera", "EXIF"}, {"Model"});
        std::string camera = model.rfind(make, 0) == 0 || make.empty() ? model : make + " " + model;
        const std::string lens = find({"Camera", "EXIF"}, {"Lens", "Lens Model"});
        if (!camera.empty())
            lines.push_back(camera + (lens.empty() ? "" : "  ·  " + lens));
        std::string exp;
        auto append = [&](const std::string& v)
        {
            if (!v.empty())
                exp += (exp.empty() ? "" : "  ·  ") + v;
        };
        append(find({"Camera", "EXIF"}, {"Focal length", "Focal Length"}));
        append(find({"Camera", "EXIF"}, {"F-Number", "Aperture"}));
        append(find({"Camera", "EXIF"}, {"Shutter", "Exposure Time"}));
        const std::string iso = find({"Camera", "EXIF"}, {"ISO", "ISO Speed Ratings"});
        if (!iso.empty())
            append("ISO " + iso);
        const std::string bias = find({"Camera", "EXIF"}, {"Exposure compensation", "Exposure Bias"});
        if (!bias.empty() && bias != "0.00 EV")
            append(bias + (bias.find("EV") == std::string::npos ? " EV" : ""));
        if (!exp.empty())
            lines.push_back(exp);
        std::string taken = find({"Camera", "EXIF"}, {"Taken", "Date and Time (Original)", "Date/time original"});
        if (taken.size() >= 10 && taken[4] == ':' && taken[7] == ':')  // EXIF writes the date with colons
            taken[4] = taken[7] = '-';
        if (!taken.empty())
            lines.push_back(taken);
    }
    flush();
    if (cfg_.overlay_location && image_->has_location)
    {
        lines.push_back(image_->location_text());
        if (!image_->place.empty() && image_->place != " ")
            lines.push_back(image_->place);
    }
    flush();
    if (cfg_.overlay_view)
    {
        const double shown = image_->final_width > 0 ? view_->zoom() * image_->width / image_->final_width : view_->zoom();
        std::string line = std::to_string(int(std::lround(shown * 100))) + "%";
        if (image_->is_preview())
            line += image_->is_raw ? "  ·  embedded preview" : "  ·  preview";
        else if (image_->tonable())
        {
            char ev[32];
            std::snprintf(ev, sizeof ev, "%+.2f EV", exposure_->value());
            line += std::string(image_->linear ? "  ·  developed  ·  " : "  ·  HDR  ·  ") + ev;
        }
        lines.push_back(line);
    }
    flush();
    return sections;
}

void App::refresh_metadata()
{
    if (!metadata_visible_)
        return;
    const std::string url = image_ && image_->has_location ? cfg_.map_link(image_->latitude, image_->longitude) : "";
    metadata_model_.set(collect_metadata(), image_.get(), url);
}

int App::run()
{
    return ui_.run();
}

void App::quit()
{
    ui_.quit();
}

// -- navigation ---------------------------------------------------------------
void App::open_path(const std::string& path)
{
    gchar* canonical = g_canonicalize_filename(path.c_str(), nullptr);
    folder_.scan(canonical, cfg_);
    g_free(canonical);
    load_current();
}

void App::open_diff(const std::string& a_path, const std::string& b_path)
{
    if (save_dialog_)
        save_dialog_->close();
    if (slideshow_)
        stop_slideshow();
    stop_video();
    status_message_ = "Comparing…";
    update_status();
    const uint64_t generation = ++load_generation_;
    ++preview_->running;  // waited for on exit, as a save preview is
    std::thread(
        [this, generation, a_path, b_path, state = preview_]
        {
            struct Done
            {
                PreviewState& s;
                ~Done() { --s.running; }
            } done{*state};
            auto a = std::make_shared<Image>(), b = std::make_shared<Image>();
            std::string error;
            const bool ok = decode_image(a_path, *a, error) && decode_image(b_path, *b, error);
            std::shared_ptr<const ImageDifference> difference;
            if (ok)
            {
                a->build_mips();
                b->build_mips();
                if (a->width == b->width && a->height == b->height)
                    difference = std::make_shared<ImageDifference>(compare_images(*a, *b));
            }
            platform::post_main(
                [this, generation, a, b, a_path, b_path, difference, ok, error]
                {
                    if (generation != load_generation_)
                        return;
                    if (!ok)
                    {
                        status_message_ = "Can't compare: " + error;
                        update_status();
                        return;
                    }
                    // Named by file, or by folder and file when the names are the same.
                    const std::filesystem::path pa(a_path), pb(b_path);
                    const bool same = pa.filename() == pb.filename();
                    const auto name = [same](const std::filesystem::path& p)
                    { return same ? (p.parent_path().filename() / p.filename()).string() : p.filename().string(); };
                    end_comparing();
                    if (image_ && !image_path_.empty())
                        cache_.put(image_path_, {image_, ImageCache::file_mtime(image_path_)});
                    image_ = a;
                    image_path_ = a_path;
                    std::error_code ec;
                    file_size_ = std::filesystem::file_size(a_path, ec);
                    diff_session_ = true;
                    diff_name_ = name(pa) + "  ↔  " + name(pb);
                    status_message_ = diff_name_;
                    view_->begin_compare(a, b, name(pa), name(pb), difference,
                                         difference ? View::Compare::Difference : View::Compare::Swipe);
                    view_->set_compare_gain(int(gain_->value() + 0.5));
                    update_toolbar();
                    update_title();
                    update_status();
                    fit_window_to_image();
                });
        })
        .detach();
}

void App::load_current()
{
    if (diff_session_)
        end_comparing();  // a file opened: back to browsing
    const std::string path = folder_.current();
    if (save_dialog_ && path != image_path_)
        save_dialog_->close();  // it was saving (and previewing) the image going away
    if (path.empty())
    {
        status_message_ = folder_.directory().empty() ? "No image open" : "No images in " + folder_.directory();
        update_status();
        update_title();
        return;
    }
    // Leaving an image: keep it (edits included) so coming back shows exactly
    // what was left, which is what flipping between the shots of a burst wants.
    stop_video();
    if (image_ && !image_path_.empty())
        cache_.put(image_path_, {image_, ImageCache::file_mtime(image_path_)});
    ++load_generation_;
    ImageCache::Entry hit;
    if (cache_.get(path, hit))
    {
        Loader::Result r{load_generation_, path, hit.image, {}, hit.image->is_preview(), false, true};
        on_loaded(r);
        return;
    }
    status_message_ = "Loading " + folder_.current_name() + "…";
    update_status();
    loader_.request(load_generation_, path);
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
    if (folder_.step(delta, cfg_.wrap_around))
    {
        awaiting_present_ = true;
        load_current();
    }
}

// The current image is on screen: play out one queued step, if any.
void App::image_presented()
{
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
    if (r.prefetch)
    {
        // Never replace what is already cached: it may carry the user's edits.
        if (r.image && r.path != image_path_ && !cache_.contains(r.path))
            cache_.put(r.path, {r.image, ImageCache::file_mtime(r.path)});
        if (r.image && r.path == folder_.current() && !image_)
        {  // user moved onto it while it was decoding
            r.generation = load_generation_;
            r.prefetch = false;
        }
        else
        {
            return;
        }
    }
    // A develop for the image on screen is adopted in place.
    if (!r.preview && !r.cached && r.image && image_ && image_->is_preview() && image_path_ == r.path)
    {
        image_->adopt(std::move(*r.image));
        if (tracing())
            std::fprintf(stderr, "developed %s %dx%d\n", r.path.c_str(), image_->width, image_->height);
        if (r.generation == load_generation_)
        {
            status_message_.clear();
            view_->image_changed();
            fit_window_to_image();
            sync_exposure_slider();
            update_status();
            refresh_metadata();
        }
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
    schedule_prefetch();
    if (tracing() && r.image)
        std::fprintf(stderr, "loaded %s %dx%d preview=%d cache=%zuMB\n", r.path.c_str(), r.image->width, r.image->height, int(r.image->is_preview()),
                     cache_.bytes() >> 20);
    if (!r.image)
    {
        image_.reset();
        image_path_ = r.path;
        view_->set_image(nullptr);
        status_message_ = "Cannot load " + folder_.current_name() + ": " + r.error;
        update_status();
        update_title();
        image_presented();
        return;
    }
    stop_animation();
    image_ = r.image;
    image_path_ = r.path;
    std::error_code ec;
    file_size_ = std::filesystem::file_size(r.path, ec);
    if (ec)
        file_size_ = 0;
    status_message_ = image_->is_preview() && image_->is_raw ? "Developing…" : "";
    if (!from_cache && cfg_.apply_xmp_edits && image_->xmp.present)
        apply_xmp(true);
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
    exposure_->set_range(bias - down, bias + 3.0);
    exposure_->set_value(bias + image_->exposure_edit);
    exposure_->set_enabled(!image_->is_preview());
}

void App::set_exposure(double ev)
{
    exposure_->set_value(ev);
    retone();
}

// Replaces the displayed image with the selected region. Nothing is written
// to disk; F5 reloads the original.
void App::crop_to_selection()
{
    int x, y, w, h;
    if (!image_ || !view_->selection(x, y, w, h))
        return;
    *image_ = image_->crop(x, y, w, h);  // same object: the view, cache and pane keep their pointer
    if (image_->tonable())
        image_->retone(image_->exposure_edit);
    image_->info = (image_->info.empty() ? "" : image_->info + "  ·  ") + "cropped";
    view_->image_changed();
    fit_window_to_image();
    update_status();
    refresh_metadata();
}

namespace
{

std::string lower_extension(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    if (!ext.empty())
        ext = ext.substr(1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == "jpeg" ? std::string("jpg") : ext;
}

std::string human_bytes(uintmax_t n)
{
    char buf[32];
    if (n >= 1000 * 1000)
        std::snprintf(buf, sizeof buf, "%.1f MB", n / 1e6);
    else
        std::snprintf(buf, sizeof buf, "%.0f kB", n / 1e3);
    return buf;
}

}  // namespace

// Save As: the format and its settings first (previewing lossy ones in the
// view), then the file dialog for that format's files.
void App::save_as()
{
    if (!image_ || save_dialog_)
        return;
    // Comparing two files, Save saves where they differ, as it's shown.
    save_source_ = diff_session_ ? view_->difference_shown() : image_;
    if (!save_source_)
    {
        status_message_ = "No difference image to save: the images are different sizes";
        update_status();
        return;
    }
    SaveDialog::Callbacks cb;
    cb.changed = [this](const Encoder& e, const EncodeOptions& o) { preview_save(e, o); };
    cb.preview_shown = [this](bool on) { show_save_preview(on); };
    cb.chosen = [this](const Encoder& e, const EncodeOptions& o)
    {
        // After the dialog has gone, so the file dialog has the main window as parent.
        ui_.post([this, e = &e, o, source = save_source_] { choose_save_path(*e, o, source); });
    };
    cb.closed = [this]
    {
        ++preview_->generation;
        preview_image_.reset();
        preview_difference_.reset();
        save_source_.reset();
        if (!diff_session_)
            end_comparing();
        save_dialog_.reset();
        status_message_.clear();
        update_toolbar();
        update_status();
    };
    if (slideshow_)
        stop_slideshow();
    save_dialog_ = SaveDialog::open(ui_, window_.get(), diff_session_ ? std::string("png") : lower_extension(image_path_), save_options_, std::move(cb));
    update_toolbar();
}

// The view compares the original (A) with the image as it would save (B),
// starting on B: what the save would look like.
void App::show_save_preview(bool on)
{
    if (diff_session_)
        return;  // saving the difference: the comparison stays as it is
    // The encode is already done: this only changes what the view shows.
    if (on && preview_image_ && image_)
    {
        if (view_->comparing())
        {
            view_->set_compare_b(preview_image_, preview_difference_);
            show_side();
        }
        else
        {
            view_->begin_compare(image_, preview_image_, "Original", "Saved", preview_difference_, View::Compare::B);
            view_->set_compare_gain(int(gain_->value() + 0.5));  // the gain stays as last set
        }
    }
    else
    {
        end_comparing();
        return;
    }
    update_toolbar();
    update_status();
}

// Encodes the image as it would save, decodes that back and shows it in the
// view, with the file size, a moment after the settings stop changing.
void App::preview_save(const Encoder& e, const EncodeOptions& o)
{
    const uint64_t generation = ++preview_->generation;
    if (!save_source_ || !e.lossy(o))
    {
        preview_image_.reset();
        preview_difference_.reset();
        show_save_preview(false);
        if (save_dialog_)
            save_dialog_->set_info("");
        return;
    }
    if (save_dialog_)
        save_dialog_->set_info("…");
    std::shared_ptr<Image> source = save_source_;
    const Encoder* encoder = &e;
    ++preview_->running;
    std::thread(
        [this, generation, source, encoder, o, state = preview_]
        {
            struct Done
            {
                PreviewState& s;
                ~Done() { --s.running; }
            } done{*state};
            std::this_thread::sleep_for(std::chrono::milliseconds(120));  // let a dragged slider settle
            if (state->generation.load() != generation)
                return;
            platform::lower_thread_priority();
            const std::string path = (std::filesystem::temp_directory_path() /
                                      ("ifritview-preview-" + std::to_string(generation) + "." + encoder->extension))
                                         .string();
            std::string error;
            auto result = std::make_shared<Image>();
            uintmax_t size = 0;
            bool ok = encoder->encode(*source, path, o, error);
            if (ok)
            {
                std::error_code ec;
                size = std::filesystem::file_size(path, ec);
                ok = decode_image(path, *result, error);
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
            if (state->generation.load() != generation)
                return;
            std::shared_ptr<const ImageDifference> difference;
            if (ok)
                difference = std::make_shared<ImageDifference>(compare_images(*source, *result));
            platform::post_main(
                [this, generation, result, difference, ok, size, error, state]
                {
                    if (state->generation.load() != generation || !save_dialog_)
                        return;
                    if (!ok)
                    {
                        save_dialog_->set_info("failed");
                        status_message_ = "Preview failed: " + error;
                        update_status();
                        return;
                    }
                    preview_image_ = result;
                    preview_difference_ = difference;
                    if (save_dialog_->preview_shown())
                        show_save_preview(true);
                    save_dialog_->set_info(human_bytes(size));
                });
        })
        .detach();
}

void App::choose_save_path(const Encoder& e, const EncodeOptions& o, std::shared_ptr<Image> source)
{
    if (!source)
        return;
    rap::file_dialog_spec spec;
    spec.mode = rap::file_dialog_spec::mode::save;
    spec.title = std::string("Save as ") + e.name;
    std::string stem = std::filesystem::path(diff_session_ ? image_path_ : folder_.current_name()).stem().string();
    if (diff_session_)
        stem += "-difference";
    const std::string name = (stem.empty() ? std::string("image") : stem) + "." + e.extension;
    const std::string dir = diff_session_ ? std::filesystem::path(image_path_).parent_path().string() : folder_.directory();
    spec.initial_path = dir.empty() ? name : dir + "/" + name;
    spec.filters = {{e.name, std::string("*.") + e.extension}};
    ui_.choose_files(window_.get(), spec,
                    [this, enc = &e, o, source](std::vector<std::string> files)
                    {
                        if (files.empty())
                            return;
                        std::string path = files.front();
                        if (lower_extension(path) != enc->extension)
                            path += std::string(".") + enc->extension;  // not every toolkit's dialog adds it
                        std::string error;
                        const bool ok = enc->encode(*source, path, o, error);
                        status_message_ = ok ? "Saved " + std::filesystem::path(path).filename().string() : "Save failed: " + error;
                        if (ok)
                            folder_.rescan(cfg_);  // the new file may now be part of the folder
                        update_status();
                        update_title();
                    });
}

// Neighbours in the browsing direction first, then behind; skips what the
// cache already has.
void App::schedule_prefetch()
{
    std::vector<std::string> paths;
    const int n = folder_.count();
    if (n <= 1)
    {
        loader_.prefetch({});
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
        const std::string path = folder_.directory() + "/" + folder_.name_at(i);
        if (!cache_.contains(path) && std::find(paths.begin(), paths.end(), path) == paths.end())
            paths.push_back(path);
    };
    for (int k = 1; k <= cfg_.prefetch_ahead || k <= cfg_.prefetch_behind; ++k)
    {
        if (k <= cfg_.prefetch_ahead)
            add(k);
        if (k <= cfg_.prefetch_behind)
            add(-k);
    }
    loader_.prefetch(std::move(paths));
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
        status_message_ = "Developing…";
        update_status();
    }
    loader_.request_develop(load_generation_, folder_.current());
}

void App::retone()
{
    if (!image_ || !image_->tonable())
        return;
    image_->exposure_edit = exposure_->value() - image_->exposure_bias;
    request_retone();
}

// Render the current exposure on a thread into a fresh buffer; swap it in on
// the UI thread. If the slider moved meanwhile, go again.
void App::request_retone()
{
    if (retone_busy_ || !image_ || !image_->tonable())
        return;
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
                    const bool current = image->linear == linear && image->source == source && result->size() == image->pixels.size();
                    if (current)
                    {
                        image->pixels = std::move(*result);
                        image->mips.clear();
                        if (image == image_)
                            view_->image_changed();
                    }
                    if (image == image_ && image_->tonable() && std::fabs(image_->exposure_edit - ev) > 1e-9)
                        request_retone();  // the slider moved on while we rendered
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

    // Leave room for the title bar and a little air around the window.
    const float max_w = work.w - 24, max_h = work.h - 72;
    const bool stand_in = image_->final_width > 0 && image_->final_height > 0;
    const double iw = (stand_in ? image_->final_width : image_->width) / scale;  // logical pixels at 1:1
    const double ih = (stand_in ? image_->final_height : image_->height) / scale;
    const double s = std::min({1.0, (max_w - chrome_w - pane_w) / (images * iw), (max_h - chrome_h) / ih});
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
    spec.title = "Open Image";
    spec.initial_path = folder_.directory();
    std::string patterns;
    for (const std::string& e : cfg_.extensions)
        patterns += (patterns.empty() ? "*." : ";*.") + e;
    spec.filters = {{"Images", patterns}, {"All files", "*"}};
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
            smooth_toggle_->set_checked(cfg_.smooth_upscale);
            xmp_toggle_->set_checked(cfg_.apply_xmp_edits);
            window_->set_min_size({float(cfg_.min_window_width), float(cfg_.min_window_height)});
            view_->config_changed();
            refresh_metadata();
            const bool reload =
                ((cfg_.raw_mode != old_raw_mode || cfg_.raw_camera_crop != old_raw_crop) && image_ && image_->format.find("RAW") != std::string::npos) ||
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
        if (image_->is_preview() && status_message_.empty())
            center += image_->is_raw ? "  ·  embedded preview" : "  ·  preview";
        const double shown = image_->final_width > 0 ? view_->zoom() * image_->width / image_->final_width : view_->zoom();
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
            right = clock(video_.position()) + " / " + clock(video_.duration()) + (cfg_.video_muted && image_->has_audio ? "  ·  muted" : "") + "  ·  " + right;
        }
        if (view_->zoom_mode() == View::ZoomMode::Fit && view_->zoom() < 1.0)
            right += " (fit)";
        if (const std::shared_ptr<Image> b = view_->compare_image(true); b && (b->width != image_->width || b->height != image_->height))
            center += "  ·  B " + std::to_string(b->width) + " × " + std::to_string(b->height) + ", scaled to match";
        if (const ImageDifference* d = view_->compare_difference())
        {
            char buf[160];
            if (d->max_error == 0)
                std::snprintf(buf, sizeof buf, "identical");
            else
                std::snprintf(buf, sizeof buf, "PSNR %.1f dB  ·  largest error %d  ·  %.1f%% changed", d->psnr,
                              d->max_error, d->changed * 100.0);
            center += std::string("  ·  ") + buf;
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
    const bool busy = status_message_.rfind("Loading", 0) == 0 || status_message_.rfind("Developing", 0) == 0;
    view_->set_busy(busy);
    view_->set_overlay(overlay_sections());
    status_left_->set_text(left);
    status_center_->set_text(center);
    status_right_->set_text(right);
}

bool App::on_key(const rap::key_event& e)
{
    if (!e.press)
        return false;
    // The exposure slider keeps its keys while it has focus: arrows step it
    // (Shift refines, as on the wheel), Home is the camera's own exposure,
    // and Esc or Enter hand the keys back to the image.
    if (exposure_->has_focus())
    {
        const double step = e.modifiers & rap::mod_shift ? 0.01 : 0.1;
        switch (e.key)
        {
            case rap::key::left:
            case rap::key::down:
                set_exposure(exposure_->value() - step);
                return true;
            case rap::key::right:
            case rap::key::up:
                set_exposure(exposure_->value() + step);
                return true;
            case rap::key::home:
                set_exposure(image_ ? image_->exposure_bias : 0.0);
                return true;
            case rap::key::escape:
            case rap::key::enter:
                view_->canvas()->focus();
                return true;
            default:
                break;  // everything else keeps its usual meaning
        }
    }
    const bool ctrl = e.modifiers & rap::mod_ctrl;
    const char32_t c = e.key == rap::key::character ? e.codepoint : 0;
    switch (e.key)
    {
        case rap::key::left:
        case rap::key::page_up:
        case rap::key::backspace:
            navigate(-1);
            return true;
        case rap::key::space:
            if (image_ && image_->is_video)
            {
                toggle_video();  // on a video, Space is play/pause
                return true;
            }
            navigate(+1);
            return true;
        case rap::key::right:
        case rap::key::page_down:
            navigate(+1);
            return true;
        case rap::key::home:
            go_first();
            return true;
        case rap::key::end:
            go_last();
            return true;
        case rap::key::enter:
        case rap::key::f11:
            toggle_fullscreen();
            return true;
        case rap::key::escape:
            if (slideshow_)
                stop_slideshow();
            if (fullscreen_)
                toggle_fullscreen();
            else
                quit();
            return true;
        case rap::key::f5:
            reload();
            return true;
        case rap::key::tab:
            if (!view_->comparing())
                return false;
            set_compare(view_->compare_mode() == View::Compare::A ? View::Compare::B : View::Compare::A);
            return true;
        default:
            break;
    }
    switch (c)
    {
        case '+':
        case '=':
            zoom_in();
            return true;
        case '-':
            zoom_out();
            return true;
        case '1':
            zoom_original();
            return true;
        case 'f':
            zoom_fit();
            return true;
        case 'm':
            if (image_ && image_->is_video && image_->has_audio)
                toggle_mute();
            return true;
        case 'l':
            transform(8);
            return true;  // rotate left
        case 'r':
            transform(6);
            return true;  // rotate right
        case 'h':
            transform(2);
            return true;  // mirror horizontally
        case 'v':
            transform(4);
            return true;  // mirror vertically
        case 'o':
            if (ctrl)
            {
                open_dialog();
                return true;
            }
            break;
        case 'p':
            if (ctrl)
            {
                open_settings();
                return true;
            }
            break;
        case 'q':
            if (ctrl)
            {
                quit();
                return true;
            }
            break;
        case 'y':
            if (ctrl)
            {
                crop_to_selection();
                return true;
            }
            break;
        case 's':
            if (ctrl)
                save_as();
            else
                toggle_slideshow();
            return true;
        case 'i':
            if (fullscreen_)
            {
                // In fullscreen, I is the overlay switch; the choice is kept.
                cfg_.overlay_enabled = !cfg_.overlay_enabled;
                cfg_.save();
                view_->config_changed();
            }
            else
                toggle_metadata();
            return true;
        case 'a':
            toggle_animation();
            return true;
        case 'c':
            if (ctrl)
            {
                copy_to_clipboard();
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}
