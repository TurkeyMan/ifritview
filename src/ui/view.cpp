#include "view.h"

#include "config.h"
#include "controls.h"
#include "scale.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>

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

constexpr double kZoomStep = 1.25;
constexpr double kMinZoom = 0.02;
constexpr double kMaxZoom = 64.0;
constexpr int kDoubleClickMs = 400;
constexpr double kPi = std::numbers::pi;

int64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// "#rrggbb" (or "rrggbb"); `fallback` otherwise.
Color parse_colour(const std::string& s, Color fallback)
{
    const char* p = s.c_str();
    if (*p == '#')
        ++p;
    unsigned v = 0;
    if (std::strlen(p) != 6 || std::sscanf(p, "%x", &v) != 1)
        return fallback;
    return Color{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), 255};
}

Color rgba(double r, double g, double b, double a)
{
    return Color{uint8_t(r * 255 + 0.5), uint8_t(g * 255 + 0.5), uint8_t(b * 255 + 0.5), uint8_t(a * 255 + 0.5)};
}

Control click_control(int button, bool twice)
{
    switch (button)
    {
        case 1:
            return twice ? Control::LeftDouble : Control::LeftClick;
        case 2:
            return twice ? Control::MiddleDouble : Control::MiddleClick;
        default:
            return twice ? Control::RightDouble : Control::RightClick;
    }
}

Control drag_control(int button)
{
    return button == 1 ? Control::LeftDrag : button == 2 ? Control::MiddleDrag : Control::RightDrag;
}

}  // namespace

View::View(Ui& ui, const Config& cfg) : ui_(ui), cfg_(cfg) {}

View::~View()
{
    drop_outgoing();
}

Node View::node()
{
    return anyui::layout::Canvas{}
        .onPaint([this](Painter& p, SizeF s) { paint(p, s); })
        .onResize(
            [this](SizeF)
            {
                layout();
                if (on_resized)
                    on_resized();
            })
        .onPointer([this](const PointerEvent& e) { return pointer(e); })
        .onScroll([this](const ScrollEvent& e) { return scroll(e); })
        .bind(canvas_)
        .stretch();
}

void View::attach(Canvas* canvas)
{
    canvas_ = canvas;
}

void View::invalidate()
{
    if (canvas_)
        canvas_->invalidate();
}

int View::scale_factor() const
{
    return canvas_ ? std::max(1, int(std::lround(canvas_->deviceScale()))) : 1;
}

void View::view_size(int& w, int& h) const
{
    const int scale = scale_factor();
    const SizeF s = canvas_ ? canvas_->size() : SizeF{};
    w = int(s.w) * scale;
    h = int(s.h) * scale;
}

void View::cancel_deferred_click()
{
    deferred_click_.reset();
}

// A magnifier over the selection says "click here to zoom in"; a closed
// hand while panning.
void View::update_cursor(double x, double y)
{
    Cursor want = Cursor::Default;
    if (drag_active_ && pressed_button_ && cfg_.control(drag_control(pressed_button_), fullscreen_) == Action::Pan)
    {
        want = Cursor::Grabbing;
    }
    else if (has_selection_ && image_ && !pressed_button_)
    {
        double ix, iy;
        to_image(x, y, ix, iy);
        if (ix >= sel_x0_ && ix < sel_x1_ && iy >= sel_y0_ && iy < sel_y1_)
            want = Cursor::ZoomIn;
    }
    if (want == cursor_ || !canvas_)
        return;
    cursor_ = want;
    canvas_->setCursor(want);
}

void View::crossfade_next(int ms)
{
    pending_crossfade_ms_ = ms;
}

void View::crossfade_live()
{
    pending_live_ = true;
}

void View::drop_outgoing()
{
    outgoing_live_.reset();
    outgoing_live_image_.reset();
    outgoing_tick_.reset();
    outgoing_.image.reset();
    outgoing_.fade_us = 0;
}

// Where the outgoing slide is at `now`: continuing its motion if it had one.
void View::outgoing_transform(int64_t now, double& zoom, double& px, double& py) const
{
    const Motion& m = outgoing_.motion;
    if (!m.active)
    {
        zoom = outgoing_.zoom;
        px = outgoing_.pan_x;
        py = outgoing_.pan_y;
        return;
    }
    double t = m.duration_us > 0 ? double(now - m.start_us) / m.duration_us : 1.0;
    t = std::clamp(t, 0.0, 1.0);
    const double e = t * t * (3 - 2 * t);
    int vw, vh;
    view_size(vw, vh);
    zoom = m.z0 + (m.z1 - m.z0) * e;
    const double cx = m.cx0 + (m.cx1 - m.cx0) * e, cy = m.cy0 + (m.cy1 - m.cy0) * e;
    px = vw / 2.0 - cx * zoom;
    py = vh / 2.0 - cy * zoom;
    const double sw = outgoing_.width * zoom, sh = outgoing_.height * zoom;
    px = sw <= vw ? (vw - sw) / 2 : std::clamp(px, vw - sw, 0.0);
    py = sh <= vh ? (vh - sh) / 2 : std::clamp(py, vh - sh, 0.0);
}

void View::set_image(std::shared_ptr<Image> image)
{
    previewed_.reset();
    presented_ = nullptr;  // even the same object again counts as a new presentation
    // A same-size replacement (a raw's develop landing) mid-fade keeps the
    // fade; anything else ends it.
    const bool replacement = outgoing_.image && image_ && image && image->width == image_->width && image->height == image_->height;
    if (!replacement)
        drop_outgoing();
    if (pending_crossfade_ms_ > 0 && !replacement && image_ && image && image.get() != image_.get() && image_->width > 0)
    {
        // Snapshot the current rendering: a pre-scaled image at the motion's
        // zoom (or the current zoom) plus the motion itself, so it carries on.
        const double z = motion_.active ? motion_.z_cache : zoom_;
        // At 1:1 or larger that's the full-size upload itself, not a scaled copy.
        const anyui::Image* snap = scaled_image(z);
        if (snap && snap == scaled_.get())
        {
            outgoing_.image = std::move(scaled_);
            scaled_zoom_ = 0;
        }
        else if (snap && snap == base_.get())
            outgoing_.image = std::move(base_);
        if (pending_live_)
            outgoing_live_ = image_;
        outgoing_.scale_zoom = z;
        outgoing_.motion = motion_;
        outgoing_.zoom = zoom_;
        outgoing_.pan_x = pan_x_;
        outgoing_.pan_y = pan_y_;
        outgoing_.width = image_->width;
        outgoing_.height = image_->height;
        outgoing_.has_alpha = image_->has_alpha;
        outgoing_.fade_start_us = now_us();
        outgoing_.fade_us = int64_t(pending_crossfade_ms_) * 1000;
        if (canvas_)
            outgoing_tick_ = canvas_->onFrame(
                [this](double)
                {
                    if (now_us() >= outgoing_.fade_start_us + outgoing_.fade_us)
                        ui_.post([this] { drop_outgoing(); });  // not from inside its own callback
                    invalidate();
                });
    }
    pending_crossfade_ms_ = 0;
    pending_live_ = false;
    end_motion();
    drop_caches();
    base_.reset();
    base_dirty_ = true;
    image_ = std::move(image);
    has_selection_ = false;
    drag_active_ = false;
    pressed_button_ = 0;
    mode_ = ZoomMode::Fit;
    layout();
    zoom_changed();
    if (on_selection_changed)
        on_selection_changed();
}

void View::set_fullscreen(bool on)
{
    fullscreen_ = on;
    // The transition moves the window under the pointer; a press still held
    // from the double-click would otherwise turn into a drag.
    pressed_button_ = 0;
    drag_active_ = false;
    suppress_click_ = false;
    cancel_deferred_click();
    if (on)
        clear_selection();
    zoom_fit();
}

// The image object stayed; its pixels, and possibly its dimensions, did not.
// A size change keeps what is on screen where it is.
void View::show_preview(std::shared_ptr<Image> preview)
{
    if (!image_ || !preview || preview->width != image_->width || preview->height != image_->height)
        return;
    if (!previewed_)
        previewed_ = image_;
    image_ = std::move(preview);
    image_changed();
}

void View::end_preview()
{
    if (!previewed_)
        return;
    image_ = std::move(previewed_);
    previewed_.reset();
    image_changed();
}

void View::image_changed()
{
    drop_caches();
    if (!image_)
        return;
    const int old_w = base_ ? base_->width() : image_->width;
    if (base_ && base_->width() == image_->width && base_->height() == image_->height)
        base_->update(image_->pixels.data(), image_->stride);  // a video frame: same size, new pixels
    else
        base_.reset();
    base_dirty_ = !base_;
    if (old_w > 0 && old_w != image_->width)
    {
        const double ratio = double(old_w) / image_->width;  // old image px per new image px
        zoom_ *= ratio;
        if (motion_.active)
        {
            motion_.z0 *= ratio;
            motion_.z1 *= ratio;
            motion_.z_cache *= ratio;
            motion_.cx0 /= ratio;
            motion_.cy0 /= ratio;
            motion_.cx1 /= ratio;
            motion_.cy1 /= ratio;
        }
        if (mode_ == ZoomMode::Fit)
            layout();
        else
            clamp_pan();
    }
    invalidate();
}

bool View::selection(int& x, int& y, int& w, int& h) const
{
    if (!has_selection_)
        return false;
    x = int(sel_x0_);
    y = int(sel_y0_);
    w = int(sel_x1_ - sel_x0_);
    h = int(sel_y1_ - sel_y0_);
    return true;
}

void View::clear_selection()
{
    if (!has_selection_)
        return;
    has_selection_ = false;
    invalidate();
    if (on_selection_changed)
        on_selection_changed();
}

void View::set_overlay(OverlaySections sections)
{
    overlay_ = std::move(sections);
    invalidate();
}

void View::config_changed()
{
    drop_caches();
    layout();
}

void View::drop_caches()
{
    scaled_.reset();
    scaled_zoom_ = 0;
}

void View::zoom_changed()
{
    if (on_zoom_changed)
        on_zoom_changed();
}

double View::fit_zoom() const
{
    int vw, vh;
    view_size(vw, vh);
    if (!image_ || vw <= 0 || vh <= 0)
        return 1.0;
    // A stand-in is shown at the size its real image will have.
    const bool stand_in = image_->final_width > 0 && image_->final_height > 0;
    const double w = stand_in ? image_->final_width : image_->width;
    const double h = stand_in ? image_->final_height : image_->height;
    double z = std::min(vw / w, vh / h);
    if (!(fullscreen_ && cfg_.fullscreen_upscale))
        z = std::min(z, 1.0);
    if (std::fabs(z - 1.0) < 0.01)
        z = 1.0;
    if (stand_in)
        z *= w / image_->width;
    return std::clamp(z, kMinZoom, kMaxZoom);
}

void View::layout()
{
    if (mode_ == ZoomMode::Fit)
    {
        const double z = fit_zoom();
        if (z != zoom_)
        {
            zoom_ = z;
            zoom_changed();
        }
    }
    checker_.reset();  // sized to the view
    clamp_pan();
    invalidate();
}

void View::clamp_pan()
{
    if (!image_)
        return;
    int vw, vh;
    view_size(vw, vh);
    const double sw = image_->width * zoom_, sh = image_->height * zoom_;
    if (sw <= vw)
        pan_x_ = (vw - sw) / 2;
    else
        pan_x_ = std::clamp(pan_x_, vw - sw, 0.0);
    if (sh <= vh)
        pan_y_ = (vh - sh) / 2;
    else
        pan_y_ = std::clamp(pan_y_, vh - sh, 0.0);
    // Integer origins keep 1:1 rendering crisp; motion wants sub-pixel steps.
    if (!motion_.active)
    {
        pan_x_ = std::floor(pan_x_);
        pan_y_ = std::floor(pan_y_);
    }
}

void View::set_zoom(double z, double anchor_x, double anchor_y)
{
    end_motion();
    z = std::clamp(z, kMinZoom, kMaxZoom);
    if (std::fabs(z - 1.0) < 0.03)
        z = 1.0;
    if (!image_)
        return;
    const double ix = (anchor_x - pan_x_) / zoom_;
    const double iy = (anchor_y - pan_y_) / zoom_;
    zoom_ = z;
    mode_ = ZoomMode::Manual;
    pan_x_ = anchor_x - ix * z;
    pan_y_ = anchor_y - iy * z;
    clamp_pan();
    invalidate();
    zoom_changed();
}

void View::zoom_step_at(int direction, double x, double y)
{
    const double factor = direction > 0 ? kZoomStep : 1.0 / kZoomStep;
    set_zoom(zoom_ * factor, x, y);
}

void View::begin_motion(double zoom0, double zoom1, double cx0, double cy0, double cx1, double cy1, int duration_ms, int64_t start_us)
{
    if (!image_)
        return;
    end_motion();
    motion_.active = true;
    motion_.z0 = zoom0;
    motion_.z1 = zoom1;
    motion_.cx0 = cx0;
    motion_.cy0 = cy0;
    motion_.cx1 = cx1;
    motion_.cy1 = cy1;
    motion_.z_cache = std::max(zoom0, zoom1);
    motion_.start_us = start_us ? start_us : now_us();
    motion_.duration_us = int64_t(duration_ms) * 1000;
    mode_ = ZoomMode::Manual;
    scaled_image(motion_.z_cache);  // build the intermediate now, not on the first frame
    motion_frame(now_us());
    if (canvas_)
        motion_tick_ = canvas_->onFrame(
            [this](double)
            {
                if (motion_.active)
                    motion_frame(now_us());
            });
}

void View::set_busy(bool busy)
{
    if (busy == busy_)
        return;
    busy_ = busy;
    busy_tick_.reset();
    if (busy && canvas_)
        busy_tick_ = canvas_->onFrame([this](double) { invalidate(); });
    invalidate();
}

void View::end_motion()
{
    motion_tick_.reset();
    motion_.active = false;
}

void View::motion_frame(int64_t now)
{
    double t = motion_.duration_us > 0 ? double(now - motion_.start_us) / motion_.duration_us : 1.0;
    t = std::clamp(t, 0.0, 1.0);
    const double e = t * t * (3 - 2 * t);  // smoothstep
    int vw, vh;
    view_size(vw, vh);
    zoom_ = motion_.z0 + (motion_.z1 - motion_.z0) * e;
    const double cx = motion_.cx0 + (motion_.cx1 - motion_.cx0) * e;
    const double cy = motion_.cy0 + (motion_.cy1 - motion_.cy0) * e;
    pan_x_ = vw / 2.0 - cx * zoom_;
    pan_y_ = vh / 2.0 - cy * zoom_;
    clamp_pan();
    invalidate();
}

void View::zoom_step(int direction)
{
    int vw, vh;
    view_size(vw, vh);
    zoom_step_at(direction, vw / 2.0, vh / 2.0);
}

void View::zoom_original()
{
    int vw, vh;
    view_size(vw, vh);
    set_zoom(1.0, vw / 2.0, vh / 2.0);
}

void View::zoom_fit()
{
    end_motion();
    mode_ = ZoomMode::Fit;
    layout();
    zoom_changed();
}

// ------------------------------------------------------------- pixels

anyui::Image* View::base_image()
{
    if (!image_ || image_->width <= 0 || image_->height <= 0)
        return nullptr;
    if (!base_ || base_dirty_)
    {
        base_ = ui_.createImage(image_->width, image_->height, APixelFormat::Bgra8Premultiplied, image_->pixels.data(), image_->stride);
        base_dirty_ = false;
    }
    return base_.get();
}

// Downscaling a many-megapixel image with a quality filter on every redraw
// is too slow for smooth panning, so render it once per zoom level.
anyui::Image* View::scaled_image(double zoom)
{
    if (scaled_ && scaled_zoom_ == zoom)
        return scaled_.get();
    if (!image_ || image_->width <= 0)
        return nullptr;
    const int w = std::max(1, int(std::ceil(image_->width * zoom)));
    const int h = std::max(1, int(std::ceil(image_->height * zoom)));
    if (w >= image_->width)  // nothing to reduce: the original, drawn with a filter
        return base_image();
    const int64_t t0 = now_us();
    const PixelBuffer px = downscale(*image_, w, h);
    const int64_t t1 = now_us();
    scaled_ = ui_.createImage(w, h, APixelFormat::Bgra8Premultiplied, px.data(), size_t(w) * 4);
    if (std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "scale: %dx%d -> %dx%d  downscale %.1f ms  upload %.1f ms  (%zu mips)\n", image_->width, image_->height, w, h, (t1 - t0) / 1000.0,
                     (now_us() - t1) / 1000.0, image_->mips.size());
    scaled_zoom_ = zoom;
    return scaled_.get();
}

// Transparency checkerboard, 8 device px squares, the size of the view.
anyui::Image* View::checker_image(int w, int h)
{
    if (checker_ && checker_->width() == w && checker_->height() == h)
        return checker_.get();
    const int s = 8 * scale_factor();
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const uint8_t v = ((x / s + y / s) & 1) ? 102 : 140;
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    checker_ = ui_.createImage(w, h, APixelFormat::Bgra8Premultiplied, px.data(), size_t(w) * 4);
    return checker_.get();
}

// ------------------------------------------------------------- drawing

void View::paint(Painter& p, SizeF)
{
    const double scale = p.deviceScale();
    const auto L = [scale](double device) { return float(device / scale); };  // device px -> logical
    int vw, vh;
    view_size(vw, vh);

    const Color bg = parse_colour(fullscreen_ ? cfg_.fullscreen_background : cfg_.background, Color{38, 38, 38});
    p.rect({0, 0, L(vw), L(vh)}, 0, bg, std::nullopt);
    if (!image_ || image_->width <= 0 || image_->height <= 0)
        return;

    // The transparency checkerboard under an image's rectangle (device px),
    // 1:1 from the view-sized checkerboard.
    const auto checker = [&](double x, double y, double w, double h, float alpha)
    {
        const double x0 = std::max(0.0, x), y0 = std::max(0.0, y);
        const double x1 = std::min(double(vw), x + std::ceil(w)), y1 = std::min(double(vh), y + std::ceil(h));
        if (x1 > x0 && y1 > y0)
            p.image(*checker_image(vw, vh), {float(x0), float(y0), float(x1 - x0), float(y1 - y0)}, {L(x0), L(y0), L(x1 - x0), L(y1 - y0)},
                    Filter::Nearest, alpha);
    };

    double fade_alpha = 1.0;
    if (outgoing_.image)
    {
        const int64_t now = now_us();
        fade_alpha = std::clamp(double(now - outgoing_.fade_start_us) / std::max<int64_t>(1, outgoing_.fade_us), 0.0, 1.0);
        double oz, ox, oy;
        outgoing_transform(now, oz, ox, oy);
        if (outgoing_.has_alpha && cfg_.checkerboard)
            checker(ox, oy, outgoing_.width * oz, outgoing_.height * oz, 1.f);
        if (outgoing_live_ && outgoing_live_->width == outgoing_.width && outgoing_live_->height == outgoing_.height)
        {
            // Still playing: draw its current frame.
            if (!outgoing_live_image_)
                outgoing_live_image_ = ui_.createImage(outgoing_live_->width, outgoing_live_->height, APixelFormat::Bgra8Premultiplied,
                                                       outgoing_live_->pixels.data(), outgoing_live_->stride);
            else
                outgoing_live_image_->update(outgoing_live_->pixels.data(), outgoing_live_->stride);
            p.image(*outgoing_live_image_, {0, 0, float(outgoing_.width), float(outgoing_.height)},
                    {L(ox), L(oy), L(outgoing_.width * oz), L(outgoing_.height * oz)}, Filter::Linear);
        }
        else
        {
            const auto& img = *outgoing_.image;
            p.image(img, {0, 0, float(img.width()), float(img.height())}, {L(ox), L(oy), L(outgoing_.width * oz), L(outgoing_.height * oz)}, Filter::Linear);
        }
        // Where the incoming image won't cover it, the outgoing one fades to
        // the background at the same rate (four bands around the image).
        const double ix0 = pan_x_, iy0 = pan_y_, ix1 = pan_x_ + std::ceil(image_->width * zoom_), iy1 = pan_y_ + std::ceil(image_->height * zoom_);
        const Color veil{bg.r, bg.g, bg.b, uint8_t(255 * fade_alpha)};
        p.rect({0, 0, L(vw), L(std::max(0.0, iy0))}, 0, veil, std::nullopt);
        p.rect({0, L(iy1), L(vw), L(std::max(0.0, vh - iy1))}, 0, veil, std::nullopt);
        p.rect({0, L(iy0), L(std::max(0.0, ix0)), L(iy1 - iy0)}, 0, veil, std::nullopt);
        p.rect({L(ix1), L(iy0), L(std::max(0.0, vw - ix1)), L(iy1 - iy0)}, 0, veil, std::nullopt);
    }

    const double sw = image_->width * zoom_, sh = image_->height * zoom_;
    const RectF dst{L(pan_x_), L(pan_y_), L(sw), L(sh)};
    // Mid-fade the checkerboard fades in with its image (not quite a group
    // fade where the image is part-transparent, but close).
    if (image_->has_alpha && cfg_.checkerboard)
        checker(pan_x_, pan_y_, sw, sh, float(fade_alpha));
    const int64_t t_draw = now_us();
    const float alpha = float(fade_alpha);
    if (motion_.active)
    {
        // Pre-scaled to the motion's largest zoom; each frame only shrinks it a little.
        anyui::Image* src = scaled_image(motion_.z_cache);
        if (src)
            p.image(*src, {0, 0, float(src->width()), float(src->height())}, dst, Filter::Linear, alpha);
    }
    else if (zoom_ < 1.0 && cfg_.smooth_downscale && !image_->is_video)
    {
        anyui::Image* src = scaled_image(zoom_);
        if (src)
            p.image(*src, {0, 0, float(src->width()), float(src->height())}, {L(pan_x_), L(pan_y_), L(src->width()), L(src->height())}, Filter::Nearest, alpha);
    }
    else
    {
        const bool smooth = (zoom_ > 1.0 && cfg_.smooth_upscale) || (zoom_ < 1.0 && image_->is_video);
        if (anyui::Image* base = base_image())  // the full-size upload, only when it's what gets drawn
            p.image(*base, {0, 0, float(image_->width), float(image_->height)}, dst, smooth ? Filter::Linear : Filter::Nearest, alpha);
    }
    if (motion_.active)
        frame_ms_ = (now_us() - t_draw) / 1000.0;
    if (presented_ != image_.get())
    {
        presented_ = image_.get();
        if (on_presented)
            ui_.post(
                [this]
                {
                    if (on_presented)
                        on_presented();
                });
    }
    if (present_since_ && present_image_ == image_.get())
    {
        std::fprintf(stderr, "present: %6.1f ms  %dx%d %s\n", (now_us() - present_since_) / 1000.0, image_->width, image_->height,
                     image_->is_preview() ? "(preview)" : "");
        present_since_ = 0;
    }

    // Busy ring: bottom-right, small and dim, a short arc chasing round.
    if (busy_)
    {
        const double r = 9.0 * scale, cx = vw - 20.0 * scale, cy = vh - 20.0 * scale;
        const double phase = (now_us() % 1000000) / 1000000.0 * 2 * kPi;
        p.arc({L(cx), L(cy)}, L(r), 0, float(2 * kPi), rgba(1, 1, 1, 0.18), 2.5f, LineCap::Round);
        p.arc({L(cx), L(cy)}, L(r), float(phase), float(phase + kPi * 0.6), rgba(1, 1, 1, 0.75), 2.5f, LineCap::Round);
    }

    if (cfg_.overlay_enabled && !overlay_.empty() && (fullscreen_ || cfg_.overlay_windowed))
        draw_overlay(p);

    if (controls_visible())
        draw_video_controls(p);

    if (has_selection_)
    {
        // A white rectangle with black dashes on top.
        const double x = std::floor(pan_x_ + sel_x0_ * zoom_) + 0.5, y = std::floor(pan_y_ + sel_y0_ * zoom_) + 0.5;
        const double w = std::round((sel_x1_ - sel_x0_) * zoom_) - 1, h = std::round((sel_y1_ - sel_y0_) * zoom_) - 1;
        const float lw = float(1.0 / scale);
        p.rect({L(x), L(y), L(w), L(h)}, 0, std::nullopt, Color{255, 255, 255});
        const double dash = 4.0 * scale;
        auto dashes = [&](double ax, double ay, double bx, double by)
        {
            const double len = std::hypot(bx - ax, by - ay);
            for (double d = 0; d < len; d += 2 * dash)
            {
                const double e = std::min(len, d + dash);
                p.line({L(ax + (bx - ax) * d / len), L(ay + (by - ay) * d / len)}, {L(ax + (bx - ax) * e / len), L(ay + (by - ay) * e / len)}, Color{0, 0, 0},
                       lw);
            }
        };
        dashes(x, y, x + w, y);
        dashes(x + w, y, x + w, y + h);
        dashes(x + w, y + h, x, y + h);
        dashes(x, y + h, x, y);
    }
}

// Info overlay: small text in a contrasting colour over a faint backdrop,
// one block per category with a thin rule between them. Logical px.
void View::draw_overlay(Painter& p)
{
    const Color colour = parse_colour(cfg_.overlay_color, Color{166, 217, 106});
    TextStyle style;
    style.fg = Paint{colour};
    style.size = float(cfg_.overlay_font_pt);
    std::vector<std::string> blocks;
    float width = 0, height = 0;
    const float gap = 8, pad = 10, margin = 16;
    std::vector<float> heights;
    for (const auto& section : overlay_)
    {
        std::string text;
        for (const std::string& l : section)
            text += (text.empty() ? "" : "\n") + l;
        const SizeF s = p.measure(text, style);
        width = std::max(width, s.w);
        height += s.h + (blocks.empty() ? 0 : 2 * gap + 1);
        blocks.push_back(std::move(text));
        heights.push_back(s.h);
    }
    p.rect({margin, margin, width + 2 * pad, height + 2 * pad}, 0, rgba(0, 0, 0, 0.45), std::nullopt);
    float y = margin + pad;
    for (size_t i = 0; i < blocks.size(); ++i)
    {
        if (i > 0)
        {
            y += gap;
            p.rect({margin + pad, std::floor(y), width, 1}, 0, Color{colour.r, colour.g, colour.b, 128}, std::nullopt);
            y += gap + 1;
        }
        p.text({margin + pad, y}, blocks[i], style);
        y += heights[i];
    }
}

// ------------------------------------------------------------- video controls

void View::set_video(bool on, bool audio)
{
    video_ = on;
    video_audio_ = audio;
    volume_hover_ = false;
    scrubbing_ = false;
    video_fraction_ = 0;
    if (!on)
        controls_until_ = 0;
    invalidate();
}

void View::set_video_muted(bool muted)
{
    if (muted == video_muted_)
        return;
    video_muted_ = muted;
    if (controls_visible())
        invalidate();
}

void View::set_video_volume(double volume)
{
    video_volume_ = std::clamp(volume, 0.0, 1.0);
    if (controls_visible())
        invalidate();
}

void View::set_video_progress(double fraction, bool playing)
{
    if (scrubbing_)
        return;  // the knob follows the pointer while dragging
    const bool changed = std::fabs(fraction - video_fraction_) > 1e-4 || playing != video_playing_;
    video_fraction_ = std::clamp(fraction, 0.0, 1.0);
    video_playing_ = playing;
    if (changed && controls_visible())
        invalidate();
}

View::ControlsGeometry View::controls_geometry() const
{
    int vw, vh;
    view_size(vw, vh);
    const double s = scale_factor();
    const double margin = 28 * s;
    const double y = vh - 34 * s;
    // The glyph is 12 wide with 18 to the line; the speaker (8 left of its
    // centre) gets the same 18 on the other side. Without audio the line
    // runs to the margin.
    const double speaker_x = vw - margin - 16 * s;
    const double line_x1 = video_audio_ ? speaker_x - 26 * s : vw - margin;
    return {margin, margin + 30 * s, line_x1, speaker_x, y, 7 * s, y - 110 * s, y - 22 * s};
}

bool View::controls_visible() const
{
    return video_ && (scrubbing_ || now_us() < controls_until_);
}

void View::poke_controls()
{
    if (!video_)
        return;
    const bool was = controls_visible();
    controls_until_ = now_us() + 1500000;
    if (!was)
        invalidate();
    controls_timer_ = ui_.timer(1550, false, [this] { invalidate(); });
}

double View::fraction_at(double device_x) const
{
    const ControlsGeometry g = controls_geometry();
    return std::clamp((device_x - g.line_x0) / std::max(1.0, g.line_x1 - g.line_x0), 0.0, 1.0);
}

// -----O------ with a play/pause glyph: white over a dark outline, so it reads
// on any picture without a panel behind it.
void View::draw_video_controls(Painter& p)
{
    const double scale = p.deviceScale();
    const auto L = [scale](double device) { return float(device / scale); };
    const ControlsGeometry g = controls_geometry();
    const double knob_x = g.line_x0 + (g.line_x1 - g.line_x0) * video_fraction_;
    const Color dark = rgba(0, 0, 0, 0.5), white = rgba(1, 1, 1, 0.95);
    p.line({L(g.line_x0), L(g.y)}, {L(g.line_x1), L(g.y)}, rgba(0, 0, 0, 0.45), 5, LineCap::Round);
    p.line({L(g.line_x0), L(g.y)}, {L(g.line_x1), L(g.y)}, rgba(1, 1, 1, 0.45), 2, LineCap::Round);
    p.line({L(g.line_x0), L(g.y)}, {L(knob_x), L(g.y)}, white, 2);  // played part
    p.circle({L(knob_x), L(g.y)}, L(g.radius), white, dark);
    // Glyph: pause bars while playing, a triangle while paused.
    const double h = 14 * scale, x = g.glyph_x, top = g.y - h / 2;
    if (video_playing_)
    {
        p.rect({L(x), L(top), 4, L(h)}, 0, white, dark);
        p.rect({L(x + 8 * scale), L(top), 4, L(h)}, 0, white, dark);
    }
    else
    {
        p.polygon({{L(x), L(top)}, {L(x + 12 * scale), L(g.y)}, {L(x), L(top + h)}}, white, dark, 2);
    }

    if (!video_audio_)
        return;
    // Speaker at the right end: waves when sound is on, a cross when muted.
    const double sx = g.speaker_x, sy = g.y, u = scale;
    p.polygon({{L(sx - 8 * u), L(sy - 3 * u)},
               {L(sx - 4 * u), L(sy - 3 * u)},
               {L(sx + 1 * u), L(sy - 7 * u)},
               {L(sx + 1 * u), L(sy + 7 * u)},
               {L(sx - 4 * u), L(sy + 3 * u)},
               {L(sx - 8 * u), L(sy + 3 * u)}},
              white, dark, 2);
    auto mark = [&](float width, Color c)
    {
        if (video_muted_)
        {
            p.line({L(sx + 4 * u), L(sy - 4 * u)}, {L(sx + 11 * u), L(sy + 4 * u)}, c, width, LineCap::Round);
            p.line({L(sx + 11 * u), L(sy - 4 * u)}, {L(sx + 4 * u), L(sy + 4 * u)}, c, width, LineCap::Round);
        }
        else
        {
            p.arc({L(sx + 1 * u), L(sy)}, 5, -0.8f, 0.8f, c, width, LineCap::Round);
            p.arc({L(sx + 1 * u), L(sy)}, 9, -0.8f, 0.8f, c, width, LineCap::Round);
        }
    };
    mark(4.0f, dark);
    mark(1.8f, white);

    // Volume: a vertical line and knob above the speaker, with sound on and
    // the pointer on the speaker.
    if (!video_muted_ && (volume_hover_ || volume_dragging_))
    {
        const double vx = sx + 1 * u, top_y = g.volume_top, bottom = g.volume_bottom;
        const double knob_y = bottom - (bottom - top_y) * video_volume_;
        p.line({L(vx), L(top_y)}, {L(vx), L(bottom)}, rgba(0, 0, 0, 0.45), 5, LineCap::Round);
        p.line({L(vx), L(top_y)}, {L(vx), L(bottom)}, rgba(1, 1, 1, 0.45), 2, LineCap::Round);
        p.line({L(vx), L(knob_y)}, {L(vx), L(bottom)}, white, 2, LineCap::Round);
        p.circle({L(vx), L(knob_y)}, L(g.radius), white, dark);
    }
}

// ------------------------------------------------------------- input

void View::to_image(double x, double y, double& ix, double& iy) const
{
    const int scale = scale_factor();
    ix = (x * scale - pan_x_) / zoom_;
    iy = (y * scale - pan_y_) / zoom_;
}

void View::update_selection(double x, double y)
{
    double ix, iy;
    to_image(x, y, ix, iy);
    ix = std::clamp(ix, 0.0, double(image_->width));
    iy = std::clamp(iy, 0.0, double(image_->height));
    sel_x0_ = std::floor(std::min(sel_anchor_x_, ix));
    sel_y0_ = std::floor(std::min(sel_anchor_y_, iy));
    sel_x1_ = std::ceil(std::max(sel_anchor_x_, ix));
    sel_y1_ = std::ceil(std::max(sel_anchor_y_, iy));
    has_selection_ = sel_x1_ - sel_x0_ >= 2 && sel_y1_ - sel_y0_ >= 2;
    invalidate();
}

void View::zoom_to_selection()
{
    int vw, vh;
    view_size(vw, vh);
    const double w = sel_x1_ - sel_x0_, h = sel_y1_ - sel_y0_;
    const double z = std::clamp(std::min(vw / w, vh / h), kMinZoom, kMaxZoom);
    zoom_ = z;
    mode_ = ZoomMode::Manual;
    pan_x_ = vw / 2.0 - (sel_x0_ + w / 2) * z;
    pan_y_ = vh / 2.0 - (sel_y0_ + h / 2) * z;
    has_selection_ = false;
    clamp_pan();
    invalidate();
    zoom_changed();
}

void View::run_click(Action a, double x, double y)
{
    switch (a)
    {
        case Action::ZoomSelection:
        {
            if (!image_)
                return;
            double ix, iy;
            to_image(x, y, ix, iy);
            if (has_selection_ && ix >= sel_x0_ && ix < sel_x1_ && iy >= sel_y0_ && iy < sel_y1_)
                zoom_to_selection();
            else if (mode_ == ZoomMode::Manual)
                zoom_fit();
            has_selection_ = false;
            invalidate();
            return;
        }
        case Action::ZoomFit:
            zoom_fit();
            return;
        case Action::ZoomOriginal:
            zoom_original();
            return;
        case Action::None:
            return;
        default:
            if (on_action)
                on_action(a);
            return;
    }
}

void View::begin_drag(Action a, double x, double y)
{
    if (a == Action::Select && image_)
    {
        double ix, iy;
        to_image(x, y, ix, iy);
        sel_anchor_x_ = std::clamp(ix, 0.0, double(image_->width));
        sel_anchor_y_ = std::clamp(iy, 0.0, double(image_->height));
        has_selection_ = false;
    }
}

void View::drag_to(Action a, double x, double y)
{
    if (a == Action::Select)
    {
        if (image_)
            update_selection(x, y);
    }
    else if (a == Action::Pan)
    {
        const int scale = scale_factor();
        pan_x_ += (x - drag_x_) * scale;
        pan_y_ += (y - drag_y_) * scale;
        clamp_pan();
        invalidate();
    }
    drag_x_ = x;
    drag_y_ = y;
}

void View::run_wheel(Action a, int direction, double x, double y)
{
    const int scale = scale_factor();
    switch (a)
    {
        case Action::Zoom:
            zoom_step_at(-direction, x * scale, y * scale);
            return;  // wheel up zooms in
        case Action::Navigate:
            if (on_action)
                on_action(direction > 0 ? Action::NextImage : Action::PrevImage);
            return;
        case Action::Scroll:
            pan_y_ -= direction * 64.0 * scale;
            clamp_pan();
            invalidate();
            return;
        default:
            return;
    }
}

// Hovering the speaker reveals the volume line; it stays while the pointer is
// on the speaker or moves up along the line.
void View::update_volume_hover(double dx, double dy)
{
    bool hover = false;
    if (video_ && video_audio_)
    {
        const double s = scale_factor();
        const ControlsGeometry g = controls_geometry();
        const bool on_speaker = std::fabs(dx - g.speaker_x) < 16 * s && std::fabs(dy - g.y) < 16 * s;
        const bool on_line = std::fabs(dx - (g.speaker_x + 1 * s)) < 20 * s && dy > g.volume_top - 14 * s && dy < g.y + 16 * s;
        hover = on_speaker || (volume_hover_ && on_line);
    }
    if (hover != volume_hover_)
    {
        volume_hover_ = hover;
        invalidate();
    }
}

bool View::pointer(const PointerEvent& e)
{
    using T = PointerEvent::Type;
    const int s = scale_factor();
    const double ex = e.pos.x, ey = e.pos.y;
    const double dx = ex * s, dy = ey * s;

    if (e.type == T::Move || e.type == T::Enter)
    {
        poke_controls();
        update_volume_hover(dx, dy);
        if (volume_dragging_)
        {
            const ControlsGeometry g = controls_geometry();
            video_volume_ = std::clamp((g.volume_bottom - dy) / (g.volume_bottom - g.volume_top), 0.0, 1.0);
            if (on_video_volume)
                on_video_volume(video_volume_);
            invalidate();
            return true;
        }
        if (scrubbing_)
        {
            video_fraction_ = fraction_at(dx);
            if (on_video_seek)
                on_video_seek(video_fraction_, false);
            invalidate();
            return true;
        }
        if (!pressed_button_)
        {
            update_cursor(ex, ey);
            return false;
        }
        const Action a = cfg_.control(drag_control(pressed_button_), fullscreen_);
        if (!drag_active_)
        {
            if (std::hypot(ex - press_x_, ey - press_y_) < 4.0)
                return true;
            drag_active_ = true;
            begin_drag(a, press_x_, press_y_);
            update_cursor(ex, ey);
        }
        drag_to(a, ex, ey);
        return true;
    }
    if (e.type == T::Leave)
        return false;

    const bool press = e.type == T::Press;
    if (video_ && e.button == 1)
    {
        const ControlsGeometry g = controls_geometry();
        const double vx = g.speaker_x + 1 * s;
        auto volume_at = [&](double y) { return std::clamp((g.volume_bottom - y) / (g.volume_bottom - g.volume_top), 0.0, 1.0); };
        if (!press && volume_dragging_)
        {
            volume_dragging_ = false;
            poke_controls();
            return true;
        }
        if (press && e.clicks == 1 && controls_visible() && video_audio_ && !video_muted_ && volume_hover_ && std::fabs(dx - vx) < 14 * s &&
            dy > g.volume_top - 12 * s && dy < g.volume_bottom + 6 * s)
        {
            volume_dragging_ = true;
            video_volume_ = volume_at(dy);
            if (on_video_volume)
                on_video_volume(video_volume_);
            invalidate();
            return true;
        }
        if (!press && scrubbing_)
        {
            scrubbing_ = false;
            if (on_video_seek)
                on_video_seek(fraction_at(dx), true);
            poke_controls();
            return true;
        }
        if (press && controls_visible() && std::fabs(dy - g.y) < 16 * s)
        {
            if (e.clicks > 1)
                return true;  // the rest of a click on the controls
            if (dx < g.line_x0 - 6 * s)
            {
                if (on_video_toggle)
                    on_video_toggle();
            }
            else if (dx > g.line_x1 + 6 * s)
            {
                if (video_audio_ && on_video_mute)
                    on_video_mute();
            }
            else
            {
                scrubbing_ = true;
                video_fraction_ = fraction_at(dx);
                if (on_video_seek)
                    on_video_seek(video_fraction_, false);
            }
            invalidate();
            return true;
        }
        if (!press && controls_visible() && std::fabs(dy - g.y) < 16 * s)
            return true;
    }
    if (e.button < 1 || e.button > 3)
        return false;
    // In fullscreen a click is held back for the double-click interval when
    // the same button also has a double-click action, so one gesture can't
    // fire both.
    if (fullscreen_ && !press && e.button == pressed_button_ && !drag_active_ && !suppress_click_ &&
        cfg_.control(click_control(e.button, true), true) != Action::None)
    {
        pressed_button_ = 0;
        const Action a = cfg_.control(click_control(e.button, false), true);
        deferred_click_ = ui_.timer(kDoubleClickMs, false, [this, a, ex, ey] { run_click(a, ex, ey); });
        return true;
    }
    if (press && e.clicks >= 2)
    {
        suppress_click_ = true;
        cancel_deferred_click();  // the first click of the pair never fires
        pressed_button_ = e.button;
        drag_active_ = false;
        run_click(cfg_.control(click_control(e.button, true), fullscreen_), ex, ey);
    }
    else if (press)
    {
        if (pressed_button_)
            return true;  // a second button during a drag is ignored
        pressed_button_ = e.button;
        drag_active_ = false;
        suppress_click_ = false;
        press_x_ = drag_x_ = ex;
        press_y_ = drag_y_ = ey;
    }
    else if (e.button == pressed_button_)
    {
        const int button = pressed_button_;
        pressed_button_ = 0;
        if (drag_active_)
            drag_to(cfg_.control(drag_control(button), fullscreen_), ex, ey);
        else if (!suppress_click_)
            run_click(cfg_.control(click_control(button, false), fullscreen_), ex, ey);
        drag_active_ = false;
        update_cursor(ex, ey);
        if (on_selection_changed)
            on_selection_changed();
    }
    return true;
}

bool View::scroll(const ScrollEvent& e)
{
    // Over the speaker or the volume line, the wheel is the volume.
    if (video_ && video_audio_ && controls_visible())
    {
        const int s = scale_factor();
        update_volume_hover(e.pos.x * s, e.pos.y * s);
        if (volume_hover_)
        {
            const double step = -0.05 * e.dy;
            if (step > 0 && video_muted_ && on_video_mute)
                on_video_mute();  // wheeling up on a muted video brings the sound back
            else if (step != 0 && !video_muted_)
            {
                video_volume_ = std::clamp(video_volume_ + step, 0.0, 1.0);
                if (on_video_volume)
                    on_video_volume(video_volume_);
            }
            poke_controls();
            invalidate();
            return true;
        }
    }
    // Some compositors deliver a notch as 0.9 rather than 1.0, so fire at half
    // a notch and carry the remainder; a change of direction starts afresh.
    if (e.dy * scroll_accum_ < 0)
        scroll_accum_ = 0;
    scroll_accum_ += e.dy;
    int direction = 0;
    if (std::fabs(scroll_accum_) >= 0.5)
    {
        direction = scroll_accum_ > 0 ? 1 : -1;
        scroll_accum_ -= direction;
        if (std::fabs(scroll_accum_) >= 0.5)
            scroll_accum_ = 0;  // large touchpad flicks: one step, not a burst
    }
    if (direction == 0)
        return true;
    const Control c = (e.modifiers & ModCtrl) ? Control::CtrlWheel : (e.modifiers & ModShift) ? Control::ShiftWheel : Control::Wheel;
    run_wheel(cfg_.control(c, fullscreen_), direction, e.pos.x, e.pos.y);
    return true;
}
