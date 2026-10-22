#include "app.h"

#include "platform.h"
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

using anyui::Canvas;
using anyui::Color;
using anyui::Cursor;
using anyui::Extent;
using anyui::FileDialogSpec;
using anyui::Filter;
using anyui::Icon;
using anyui::Key;
using anyui::KeyEvent;
using anyui::LineCap;
using anyui::MessageSpec;
using anyui::ModCtrl;
using anyui::ModShift;
using anyui::Node;
using anyui::Paint;
using anyui::Painter;
using anyui::PointerEvent;
using anyui::PointF;
using anyui::RectF;
using anyui::ScrollEvent;
using anyui::SelectionMode;
using anyui::SizeF;
using anyui::TextStyle;
using anyui::ThemeColor;
using anyui::Ui;
using APixelFormat = anyui::PixelFormat;

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

App::App(Ui& ui) : ui_(ui), loader_([this](Loader::Result& r) { on_loaded(r); }, [&ui](std::function<void()> fn, int) { ui.post(std::move(fn)); }), cache_(0)
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
}

Node App::tool_button(const char* icon, const char* tooltip, void (App::*action)())
{
    return anyui::layout::ToolButton{Icon::named(icon), tooltip}.onClick([this, action] { (this->*action)(); });
}

void App::build_ui()
{
    view_ = std::make_unique<View>(ui_, cfg_);
    namespace L = anyui::layout;
    using L::HBox;
    using L::Horizontal;
    using L::Separator;
    using L::Spacer;
    using L::VBox;

    // Exposure, in stops around the camera's compensation; raw only.
    Node exposure =
        HBox{
            L::Label{"EV"},
            L::Slider{-3.0, 3.0, 0.0}
                .step(0.1, 1.0)
                .showValue(2)
                .mark(0.0)
                .onChanged([this](double) { retone(); })
                .bind(exposure_)
                .width(anyui::Extent{24, anyui::Extent::Unit::Chars})
                .tooltip("Exposure (EV). Rests at the camera's compensation, i.e. as shot.\n"
                         "Greyed while the embedded preview stands in for the raw"),
        }
            .handle(exposure_box_);

    window_ = ui_.create(L::Window{
        "IfritView",
        {VBox{
            L::Toolbar{
                tool_button("document-open-symbolic", "Open… (Ctrl+O)", &App::open_dialog),
                tool_button("media-floppy-symbolic", "Save as… (Ctrl+S)", &App::save_as),
                Separator{},
                L::ToolButton{Icon::named("go-previous-symbolic"), "Previous image (Left, wheel up)"}.onClick([this] { navigate(-1); }),
                L::ToolButton{Icon::named("go-next-symbolic"), "Next image (Right, wheel down)"}.onClick([this] { navigate(+1); }),
                Separator{},
                tool_button("zoom-out-symbolic", "Zoom out (-, Ctrl+wheel)", &App::zoom_out),
                tool_button("zoom-in-symbolic", "Zoom in (+, Ctrl+wheel)", &App::zoom_in),
                tool_button("zoom-original-symbolic", "Actual pixels (1)", &App::zoom_original),
                tool_button("zoom-fit-best-symbolic", "Fit to window (F)", &App::zoom_fit),
                Separator{},
                tool_button("view-fullscreen-symbolic", "Fullscreen (Enter, double-click)", &App::toggle_fullscreen),
                tool_button("media-playback-start-symbolic", "Slideshow (S): fullscreen, timed, with gentle motion", &App::toggle_slideshow),
                Separator{},
                L::ToolButton{Icon::named("edit-cut-symbolic"), "Crop to selection (Ctrl+Y)"}.onClick([this] { crop_to_selection(); }).bind(crop_button_),
                L::ToolButton{Icon::named("view-grid-symbolic"), "Smooth when zoomed in (off shows crisp pixels)"}
                    .toggle(cfg_.smooth_upscale)
                    .onToggled(
                        [this](bool on)
                        {
                            cfg_.smooth_upscale = on;
                            cfg_.save();
                            view_->config_changed();
                        })
                    .bind(smooth_toggle_),
                L::ToolButton{Icon::named("document-edit-symbolic"), "Apply edits from XMP sidecars (exposure, crop, rotation)"}
                    .toggle(cfg_.apply_xmp_edits)
                    .onToggled([this](bool) { toggle_xmp(); })
                    .bind(xmp_toggle_),
                Spacer{},
                exposure,
                tool_button("dialog-information-symbolic", "Metadata / EXIF (I)", &App::toggle_metadata),
                tool_button("preferences-system-symbolic", "Settings (Ctrl+P)", &App::open_settings),
            }
                .handle(toolbar_),
            L::Splitter{
                Horizontal,
                view_->node(),
                VBox{
                    L::List{{{"Tag", anyui::Extent{14, anyui::Extent::Unit::Chars}}, {"Value", {}, true}}, &metadata_model_}
                        .selection(SelectionMode::Single)
                        .onActivate(
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
            HBox{
                L::Label{""}.bind(status_left_).stretch(),
                L::Label{""}.bind(status_center_),
                L::Button{""}.flat().onClick([this] { open_url(location_url_); }).bind(status_location_),
                L::Label{""}.bind(status_right_),
            }
                .margins()
                .handle(footer_),
        }}}.size(Extent{float(cfg_.min_window_width), Extent::Unit::Px}, Extent{float(cfg_.min_window_height), Extent::Unit::Px})
                             .margins(false));
    view_->attach(view_->canvas());
    window_->onKey([this](const KeyEvent& e) { return on_key(e); });
    window_->onFullscreenChanged([this](bool fs) { on_fullscreen_changed(fs); });
    window_->onCloseRequest(
        [this]
        {
            ui_.quit();
            return true;
        });
    window_->setIcon(Icon::named("ifritview"));
    metadata_pane_->setVisible(false);
    exposure_box_->setVisible(false);
    crop_button_->setEnabled(false);
    status_location_->setVisible(false);
    status_location_->setTooltip("Open in maps");

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
    view_->on_zoom_changed = [this] { update_status(); };
    view_->on_presented = [this] { image_presented(); };
    view_->on_resized = [this]
    {
        if (refit_after_layout_)
            refit_after_pane_toggle();
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
        crop_button_->setEnabled(view_->selection(x, y, w, h));
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
    ui_.setClipboardImage(region.width, region.height, APixelFormat::Bgra8Premultiplied, region.pixels.data(), region.stride);
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

// ------------------------------------------------------------ slideshow

void App::toggle_slideshow()
{
    if (slideshow_)
    {
        stop_slideshow();
        return;
    }
    if (folder_.empty())
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

// --------------------------------------------------------------- video

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

// ----------------------------------------------------------- animation

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

// ------------------------------------------------------------ metadata

void App::toggle_metadata()
{
    const SizeF before = window_->size();
    metadata_visible_ = !metadata_visible_;
    metadata_pane_->setVisible(metadata_visible_);
    if (metadata_visible_)
    {
        splitter_->setSizes({std::max(100.f, before.w), float(cfg_.metadata_pane_width)});
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
        const RectF work = window_->workArea();
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
    const RectF work = window_->workArea();
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
        else if (image_->linear)
        {
            char ev[32];
            std::snprintf(ev, sizeof ev, "%+.2f EV", exposure_->value());
            line += std::string("  ·  developed  ·  ") + ev;
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

// ------------------------------------------------------------ navigation

void App::open_path(const std::string& path)
{
    gchar* canonical = g_canonicalize_filename(path.c_str(), nullptr);
    folder_.scan(canonical, cfg_);
    g_free(canonical);
    load_current();
}

void App::load_current()
{
    const std::string path = folder_.current();
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
    if (folder_.go(0))
        load_current();
}

void App::go_last()
{
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
    const bool raw = image_ && image_->is_raw;
    if (!fullscreen_)
        exposure_box_->setVisible(raw);
    if (!raw)
        return;
    const double bias = image_->exposure_bias;
    exposure_->setRange(bias - 3.0, bias + 3.0);
    exposure_->setValue(bias + image_->exposure_edit);
    exposure_->setEnabled(!image_->is_preview());
}

void App::set_exposure(double ev)
{
    exposure_->setValue(ev);
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
    if (image_->linear)
        image_->retone(image_->exposure_edit);
    image_->info = (image_->info.empty() ? "" : image_->info + "  ·  ") + "cropped";
    view_->image_changed();
    fit_window_to_image();
    update_status();
    refresh_metadata();
}

// Save As: the format follows the chosen file's extension; quality 90 for
// the lossy formats.
// TODO: a format and quality chooser in the dialog (anyui needs extra
// widgets on file dialogs).
void App::save_as()
{
    if (!image_)
        return;
    FileDialogSpec spec;
    spec.mode = FileDialogSpec::Mode::Save;
    spec.title = "Save Image As";
    spec.initialPath = folder_.directory().empty() ? folder_.current_name() : folder_.directory() + "/" + folder_.current_name();
    // The image's own format first: it's the default for a name typed
    // without an extension.
    const auto lower_ext = [](const std::string& path)
    {
        std::string ext = std::filesystem::path(path).extension().string();
        if (!ext.empty())
            ext = ext.substr(1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return ext == "jpeg" ? std::string("jpg") : ext;
    };
    std::vector<const Encoder*> order = encoders();
    const std::string own = lower_ext(image_path_);
    std::stable_partition(order.begin(), order.end(), [&](const Encoder* e) { return own == e->extension; });
    for (const Encoder* e : order)
        spec.filters.push_back({e->name, std::string("*.") + e->extension});
    ui_.chooseFiles(window_.get(), spec,
                    [this, lower_ext, fallback = order.empty() ? std::string() : std::string(order.front()->extension)](std::vector<std::string> files)
                    {
                        if (files.empty() || !image_)
                            return;
                        std::string path = files.front();
                        std::string ext = lower_ext(path);
                        if (ext.empty() && !fallback.empty())
                        {
                            // Not every toolkit's dialog adds one.
                            path += "." + fallback;
                            ext = fallback;
                        }
                        EncodeOptions options;
                        options.quality = 90;
                        std::string error = "unknown format ." + ext;
                        bool ok = false;
                        for (const Encoder* e : encoders())
                            if (ext == e->extension)
                                ok = e->encode(*image_, path, options, error);
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
    if (!image_ || !image_->linear)
        return;
    image_->exposure_edit = exposure_->value() - image_->exposure_bias;
    request_retone();
}

// Render the current exposure on a thread into a fresh buffer; swap it in on
// the UI thread. If the slider moved meanwhile, go again.
void App::request_retone()
{
    if (retone_busy_ || !image_ || !image_->linear)
        return;
    retone_busy_ = true;
    auto image = image_;
    auto linear = image_->linear;
    const double ev = image_->exposure_edit;
    std::thread(
        [this, image, linear, ev]
        {
            auto result = std::make_shared<PixelBuffer>(image->retone_buffer(ev));
            ui_.post(
                [this, image, linear, ev, result]
                {
                    retone_busy_ = false;
                    const bool current = image->linear == linear && result->size() == image->pixels.size();
                    if (current)
                    {
                        image->pixels = std::move(*result);
                        image->mips.clear();
                        if (image == image_)
                            view_->image_changed();
                    }
                    if (image == image_ && image_->linear && std::fabs(image_->exposure_edit - ev) > 1e-9)
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
    if (image_->linear)
        image_->retone(image_->exposure_edit);
    status_message_ = "XMP edits applied";
}

// --------------------------------------------------------------- window

// Size the window so the image shows 1:1 when it fits on the monitor,
// but never smaller than the configured minimum and never larger than
// the work area. Where the platform lets us, keep it on screen.
void App::fit_window_to_image()
{
    if (!image_ || fullscreen_ || !cfg_.fit_window_to_image || !window_)
        return;
    RectF work = window_->workArea();
    if (work.w <= 0 || work.h <= 0)
        work = {0, 0, 1920, 1080};
    // Chrome: what the window holds besides the image area.
    int vw = 0, vh = 0;
    view_->view_size(vw, vh);
    const double scale = view_->scale_factor();
    const SizeF cur = window_->size();
    const float pane_w = metadata_visible_ ? float(cfg_.metadata_pane_width) : 0.f;
    const float chrome_w = vw > 0 ? std::max(0.f, cur.w - float(vw / scale) - pane_w) : 0.f;
    const float chrome_h = vh > 0 ? std::max(0.f, cur.h - float(vh / scale)) : 80.f;

    // Leave room for the title bar and a little air around the window.
    const float max_w = work.w - 24, max_h = work.h - 72;
    const bool stand_in = image_->final_width > 0 && image_->final_height > 0;
    const double iw = (stand_in ? image_->final_width : image_->width) / scale;  // logical pixels at 1:1
    const double ih = (stand_in ? image_->final_height : image_->height) / scale;
    const double s = std::min({1.0, (max_w - chrome_w - pane_w) / iw, (max_h - chrome_h) / ih});
    const float cw = float(std::ceil(iw * s)), ch = float(std::ceil(ih * s));
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
    window_->setFullscreen(!fullscreen_);
}

void App::on_fullscreen_changed(bool fs)
{
    if (fs == fullscreen_)
        return;
    fullscreen_ = fs;
    if (!fs && slideshow_)
        stop_slideshow();
    toolbar_->setVisible(!fs);
    footer_->setVisible(!fs);
    // The info pane gives way to the fullscreen overlay, and comes back after.
    if (fs)
    {
        pane_before_fullscreen_ = metadata_visible_;
        metadata_pane_->setVisible(false);
    }
    else
    {
        if (pane_before_fullscreen_)
            metadata_pane_->setVisible(true);
        update_title();
        exposure_box_->setVisible(image_ && image_->is_raw);
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
    FileDialogSpec spec;
    spec.mode = FileDialogSpec::Mode::Open;
    spec.title = "Open Image";
    spec.initialPath = folder_.directory();
    std::string patterns;
    for (const std::string& e : cfg_.extensions)
        patterns += (patterns.empty() ? "*." : ";*.") + e;
    spec.filters = {{"Images", patterns}, {"All files", "*"}};
    ui_.chooseFiles(window_.get(), spec,
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
    open_settings_dialog(
        ui_, window_.get(), cfg_,
        [this, old_raw_mode, old_apply_xmp, old_raw_crop](bool ok)
        {
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
            smooth_toggle_->setChecked(cfg_.smooth_upscale);
            xmp_toggle_->setChecked(cfg_.apply_xmp_edits);
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

// --------------------------------------------------------------- status

void App::update_title()
{
    // In fullscreen the title is invisible, and under some compositors a
    // title change re-activates the window, stealing focus.
    if (fullscreen_ || !window_)
        return;
    std::string title = "IfritView";
    if (!folder_.empty())
        title = folder_.current_name() + " (" + std::to_string(folder_.index() + 1) + "/" + std::to_string(folder_.count()) + ") – IfritView";
    window_->setTitle(title);
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
    }
    if (image_ && image_->has_location)
    {
        const std::string place = image_->place == " " ? "" : image_->place;
        location_url_ = cfg_.map_link(image_->latitude, image_->longitude);
        status_location_->setText(image_->location_text() + (place.empty() ? "" : "  ·  " + place));
        status_location_->setVisible(true);
    }
    else
    {
        status_location_->setVisible(false);
    }
    const bool busy = status_message_.rfind("Loading", 0) == 0 || status_message_.rfind("Developing", 0) == 0;
    view_->set_busy(busy);
    view_->set_overlay(overlay_sections());
    status_left_->setText(left);
    status_center_->setText(center);
    status_right_->setText(right);
}

bool App::on_key(const KeyEvent& e)
{
    if (!e.press)
        return false;
    const bool ctrl = e.modifiers & ModCtrl;
    const char32_t c = e.key == Key::Character ? e.codepoint : 0;
    switch (e.key)
    {
        case Key::Left:
        case Key::PageUp:
        case Key::Backspace:
            navigate(-1);
            return true;
        case Key::Space:
            if (image_ && image_->is_video)
            {
                toggle_video();  // on a video, Space is play/pause
                return true;
            }
            navigate(+1);
            return true;
        case Key::Right:
        case Key::PageDown:
            navigate(+1);
            return true;
        case Key::Home:
            go_first();
            return true;
        case Key::End:
            go_last();
            return true;
        case Key::Enter:
        case Key::F11:
            toggle_fullscreen();
            return true;
        case Key::Escape:
            if (slideshow_)
                stop_slideshow();
            if (fullscreen_)
                toggle_fullscreen();
            else
                quit();
            return true;
        case Key::F5:
            reload();
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
