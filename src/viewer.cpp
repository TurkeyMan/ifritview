#include "viewer.h"

#include "config.h"
#include "controls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{

constexpr double kZoomStep = 1.25;
constexpr double kMinZoom = 0.02;
constexpr double kMaxZoom = 64.0;

}  // namespace

ImageView::ImageView(const Config& cfg) : cfg_(cfg)
{
    area_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(area_, 64, 64);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_widget_set_can_focus(area_, TRUE);
    gtk_widget_add_events(area_, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);

    g_signal_connect(area_, "draw",
                     G_CALLBACK(+[](GtkWidget*, cairo_t* cr, gpointer self) -> gboolean
                                {
                                    static_cast<ImageView*>(self)->draw(cr);
                                    return TRUE;
                                }),
                     this);
    g_signal_connect(area_, "size-allocate", G_CALLBACK(+[](GtkWidget*, GdkRectangle*, gpointer self) { static_cast<ImageView*>(self)->layout(); }), this);
    g_signal_connect(area_, "notify::scale-factor", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer self) { static_cast<ImageView*>(self)->layout(); }), this);
    g_signal_connect(area_, "button-press-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEventButton* e, gpointer self) { return static_cast<ImageView*>(self)->on_button(e); }), this);
    g_signal_connect(area_, "button-release-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEventButton* e, gpointer self) { return static_cast<ImageView*>(self)->on_button(e); }), this);
    g_signal_connect(area_, "motion-notify-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEventMotion* e, gpointer self) { return static_cast<ImageView*>(self)->on_motion(e); }), this);
    g_signal_connect(area_, "scroll-event",
                     G_CALLBACK(+[](GtkWidget*, GdkEventScroll* e, gpointer self) { return static_cast<ImageView*>(self)->on_scroll(e); }), this);
}

void ImageView::cancel_deferred_click()
{
    if (deferred_click_)
        g_source_remove(deferred_click_);
    deferred_click_ = 0;
}

ImageView::~ImageView()
{
    if (controls_timer_)
        g_source_remove(controls_timer_);
    drop_outgoing();
    set_busy(false);
    end_motion();
    cancel_deferred_click();
    drop_caches();
    if (surface_)
        cairo_surface_destroy(surface_);
    if (checker_)
        cairo_pattern_destroy(checker_);
}

namespace
{
Control drag_control(unsigned button);
}  // namespace

// A magnifier over the selection says "click here to zoom in"; a closed
// hand while panning.
void ImageView::update_cursor(double x, double y)
{
    const char* want = nullptr;
    if (drag_active_ && pressed_button_ && cfg_.control(drag_control(pressed_button_), fullscreen_) == Action::Pan)
    {
        want = "grabbing";
    }
    else if (has_selection_ && image_ && !pressed_button_)
    {
        double ix, iy;
        to_image(x, y, ix, iy);
        if (ix >= sel_x0_ && ix < sel_x1_ && iy >= sel_y0_ && iy < sel_y1_)
            want = "zoom-in";
    }
    if (want == cursor_name_)
        return;
    cursor_name_ = want;
    GdkWindow* gw = gtk_widget_get_window(area_);
    if (!gw)
        return;
    GdkCursor* cursor = want ? gdk_cursor_new_from_name(gtk_widget_get_display(area_), want) : nullptr;
    gdk_window_set_cursor(gw, cursor);
    if (cursor)
        g_object_unref(cursor);
}

int ImageView::scale_factor() const
{
    return std::max(1, gtk_widget_get_scale_factor(area_));
}

void ImageView::view_size(int& w, int& h) const
{
    const int scale = scale_factor();
    w = gtk_widget_get_allocated_width(area_) * scale;
    h = gtk_widget_get_allocated_height(area_) * scale;
}

void ImageView::crossfade_next(int ms)
{
    pending_crossfade_ms_ = ms;
}

void ImageView::drop_outgoing()
{
    if (outgoing_.tick)
        gtk_widget_remove_tick_callback(area_, outgoing_.tick);
    outgoing_.tick = 0;
    if (outgoing_.surface)
        cairo_surface_destroy(outgoing_.surface);
    outgoing_.surface = nullptr;
    outgoing_.fade_us = 0;
}

// Where the outgoing slide is at `now`: continuing its motion if it had one.
void ImageView::outgoing_transform(gint64 now_us, double& zoom, double& px, double& py) const
{
    const Motion& m = outgoing_.motion;
    if (!m.active)
    {
        zoom = outgoing_.zoom;
        px = outgoing_.pan_x;
        py = outgoing_.pan_y;
        return;
    }
    double t = m.duration_us > 0 ? double(now_us - m.start_us) / m.duration_us : 1.0;
    t = std::clamp(t, 0.0, 1.0);
    const double e = t * t * (3 - 2 * t);
    int vw, vh;
    view_size(vw, vh);
    zoom = m.z0 + (m.z1 - m.z0) * e;
    const double cx = m.cx0 + (m.cx1 - m.cx0) * e, cy = m.cy0 + (m.cy1 - m.cy0) * e;
    px = vw / 2.0 - cx * zoom;
    py = vh / 2.0 - cy * zoom;
    // Keep it covering the view the same way clamp_pan does.
    const double sw = outgoing_.width * zoom, sh = outgoing_.height * zoom;
    px = sw <= vw ? (vw - sw) / 2 : std::clamp(px, vw - sw, 0.0);
    py = sh <= vh ? (vh - sh) / 2 : std::clamp(py, vh - sh, 0.0);
}

void ImageView::set_image(std::shared_ptr<Image> image)
{
    presented_ = nullptr;  // even the same object again counts as a new presentation
    // A same-size replacement (a raw's develop landing) mid-fade keeps the
    // fade; anything else ends it.
    const bool replacement = outgoing_.surface && image_ && image && image->width == image_->width && image->height == image_->height;
    if (!replacement)
        drop_outgoing();
    if (pending_crossfade_ms_ > 0 && !replacement && surface_ && image_ && image && image.get() != image_.get())
    {
        // Snapshot the current rendering: a pre-scaled surface at the motion's
        // zoom (or the current zoom) plus the motion itself, so it carries on.
        const double z = motion_.active ? motion_.z_cache : zoom_;
        outgoing_.surface = cairo_surface_reference(scaled_surface(z));
        outgoing_.scale_zoom = z;
        outgoing_.motion = motion_;
        outgoing_.motion.tick = 0;
        outgoing_.zoom = zoom_;
        outgoing_.pan_x = pan_x_;
        outgoing_.pan_y = pan_y_;
        outgoing_.width = image_->width;
        outgoing_.height = image_->height;
        outgoing_.fade_start_us = g_get_monotonic_time();
        outgoing_.fade_us = gint64(pending_crossfade_ms_) * 1000;
        outgoing_.tick = gtk_widget_add_tick_callback(
            area_,
            +[](GtkWidget* w, GdkFrameClock* clock, gpointer self) -> gboolean
            {
                auto* view = static_cast<ImageView*>(self);
                if (!view->outgoing_.surface)
                    return G_SOURCE_REMOVE;
                if (gdk_frame_clock_get_frame_time(clock) >= view->outgoing_.fade_start_us + view->outgoing_.fade_us)
                {
                    view->outgoing_.tick = 0;
                    view->drop_outgoing();
                    gtk_widget_queue_draw(w);
                    return G_SOURCE_REMOVE;
                }
                gtk_widget_queue_draw(w);
                return G_SOURCE_CONTINUE;
            },
            this, nullptr);
        scaled_ = nullptr;  // the snapshot owns that surface now; don't let drop_caches free it
        scaled_zoom_ = 0;
    }
    pending_crossfade_ms_ = 0;
    end_motion();
    drop_caches();
    if (surface_)
    {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }
    image_ = std::move(image);
    if (image_ && image_->width > 0 && image_->height > 0)
    {
        surface_ = cairo_image_surface_create_for_data(image_->pixels.data(), CAIRO_FORMAT_ARGB32, image_->width, image_->height, image_->stride);
    }
    has_selection_ = false;
    drag_active_ = false;
    pressed_button_ = 0;
    mode_ = ZoomMode::Fit;
    layout();
    zoom_changed();
    if (on_selection_changed)
        on_selection_changed();
}

void ImageView::set_fullscreen(bool on)
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
// A size change (a preview adopting its develop) keeps what is on screen
// where it is: zoom and any motion are rescaled so the picture doesn't move.
void ImageView::image_changed()
{
    drop_caches();
    if (!image_)
        return;
    const int old_w = surface_ ? cairo_image_surface_get_width(surface_) : image_->width;
    if (surface_)
        cairo_surface_destroy(surface_);
    surface_ = nullptr;
    if (image_->width > 0 && image_->height > 0)
        surface_ = cairo_image_surface_create_for_data(image_->pixels.data(), CAIRO_FORMAT_ARGB32, image_->width, image_->height, image_->stride);
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
    gtk_widget_queue_draw(area_);
}

bool ImageView::selection(int& x, int& y, int& w, int& h) const
{
    if (!has_selection_)
        return false;
    x = int(sel_x0_);
    y = int(sel_y0_);
    w = int(sel_x1_ - sel_x0_);
    h = int(sel_y1_ - sel_y0_);
    return true;
}

void ImageView::clear_selection()
{
    if (!has_selection_)
        return;
    has_selection_ = false;
    gtk_widget_queue_draw(area_);
    if (on_selection_changed)
        on_selection_changed();
}

void ImageView::set_overlay(OverlaySections sections)
{
    overlay_ = std::move(sections);
    gtk_widget_queue_draw(area_);
}

void ImageView::config_changed()
{
    drop_caches();
    layout();
}

void ImageView::drop_caches()
{
    if (scaled_)
    {
        cairo_surface_destroy(scaled_);
        scaled_ = nullptr;
    }
    scaled_zoom_ = 0;
}

void ImageView::zoom_changed()
{
    if (on_zoom_changed)
        on_zoom_changed();
}

double ImageView::fit_zoom() const
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
    // A window sized for the image may be a pixel or two larger from rounding to logical units.
    if (std::fabs(z - 1.0) < 0.01)
        z = 1.0;
    if (stand_in)
        z *= w / image_->width;
    return std::clamp(z, kMinZoom, kMaxZoom);
}

void ImageView::layout()
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
    clamp_pan();
    gtk_widget_queue_draw(area_);
}

void ImageView::clamp_pan()
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

void ImageView::set_zoom(double z, double anchor_x, double anchor_y)
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
    gtk_widget_queue_draw(area_);
    zoom_changed();
}

void ImageView::zoom_step_at(int direction, double x, double y)
{
    const double factor = direction > 0 ? kZoomStep : 1.0 / kZoomStep;
    set_zoom(zoom_ * factor, x, y);
}

void ImageView::begin_motion(double zoom0, double zoom1, double cx0, double cy0, double cx1, double cy1, int duration_ms, gint64 start_us)
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
    motion_.start_us = start_us ? start_us : g_get_monotonic_time();
    motion_.duration_us = gint64(duration_ms) * 1000;
    mode_ = ZoomMode::Manual;
    scaled_surface(motion_.z_cache);  // build the intermediate now, not on the first frame
    motion_frame(g_get_monotonic_time());
    motion_.tick = gtk_widget_add_tick_callback(
        area_,
        +[](GtkWidget*, GdkFrameClock* clock, gpointer self) -> gboolean
        {
            auto* view = static_cast<ImageView*>(self);
            if (!view->motion_.active)
                return G_SOURCE_REMOVE;
            view->motion_frame(gdk_frame_clock_get_frame_time(clock));
            return G_SOURCE_CONTINUE;
        },
        this, nullptr);
}

void ImageView::set_busy(bool busy)
{
    if (busy == busy_)
        return;
    busy_ = busy;
    if (busy_tick_)
        gtk_widget_remove_tick_callback(area_, busy_tick_);
    busy_tick_ = 0;
    if (busy)
        busy_tick_ = gtk_widget_add_tick_callback(
            area_,
            +[](GtkWidget* w, GdkFrameClock*, gpointer self) -> gboolean
            {
                auto* view = static_cast<ImageView*>(self);
                if (!view->busy_)
                    return G_SOURCE_REMOVE;
                // Redraw only the ring's corner.
                const int scale = view->scale_factor();
                const int s = 40 * scale;
                gtk_widget_queue_draw_area(w, gtk_widget_get_allocated_width(w) - s / scale, gtk_widget_get_allocated_height(w) - s / scale, s / scale,
                                           s / scale);
                return G_SOURCE_CONTINUE;
            },
            this, nullptr);
    gtk_widget_queue_draw(area_);
}

void ImageView::end_motion()
{
    if (motion_.tick)
        gtk_widget_remove_tick_callback(area_, motion_.tick);
    motion_.tick = 0;
    motion_.active = false;
}

void ImageView::motion_frame(gint64 now_us)
{
    double t = motion_.duration_us > 0 ? double(now_us - motion_.start_us) / motion_.duration_us : 1.0;
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
    gtk_widget_queue_draw(area_);
}

void ImageView::zoom_step(int direction)
{
    int vw, vh;
    view_size(vw, vh);
    zoom_step_at(direction, vw / 2.0, vh / 2.0);
}

void ImageView::zoom_original()
{
    int vw, vh;
    view_size(vw, vh);
    set_zoom(1.0, vw / 2.0, vh / 2.0);
}

void ImageView::zoom_fit()
{
    end_motion();
    mode_ = ZoomMode::Fit;
    layout();
    zoom_changed();
}

cairo_pattern_t* ImageView::checker_pattern(int scale)
{
    if (checker_ && checker_scale_ == scale)
        return checker_;
    if (checker_)
        cairo_pattern_destroy(checker_);
    const int s = 8 * scale;
    cairo_surface_t* tile = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 2 * s, 2 * s);
    cairo_t* c = cairo_create(tile);
    cairo_set_source_rgb(c, 0.40, 0.40, 0.40);
    cairo_paint(c);
    cairo_set_source_rgb(c, 0.55, 0.55, 0.55);
    cairo_rectangle(c, 0, 0, s, s);
    cairo_rectangle(c, s, s, s, s);
    cairo_fill(c);
    cairo_destroy(c);
    checker_ = cairo_pattern_create_for_surface(tile);
    cairo_pattern_set_extend(checker_, CAIRO_EXTEND_REPEAT);
    cairo_surface_destroy(tile);
    checker_scale_ = scale;
    return checker_;
}

// Downscaling a many-megapixel image with a quality filter on every redraw
// is too slow for smooth panning, so render it once per zoom level.
cairo_surface_t* ImageView::scaled_surface(double zoom)
{
    if (scaled_ && scaled_zoom_ == zoom)
        return scaled_;
    drop_caches();
    const int w = std::max(1, int(std::ceil(image_->width * zoom)));
    const int h = std::max(1, int(std::ceil(image_->height * zoom)));
    scaled_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    // Start from the smallest mip that is still at least the target size, so
    // the filter only ever reduces by up to 2x.
    cairo_surface_t* src = surface_;
    int src_w = image_->width;
    for (const Image::Mip& m : image_->mips)
    {
        if (m.width < w || m.height < h)
            break;
        src = nullptr;
        src_w = m.width;
        if (&m == &image_->mips.back() || image_->mips[&m - image_->mips.data() + 1].width < w)
        {
            src = cairo_image_surface_create_for_data(const_cast<uint8_t*>(m.pixels.data()), CAIRO_FORMAT_ARGB32, m.width, m.height, m.width * 4);
            break;
        }
    }
    if (!src)
    {
        src = surface_;
        src_w = image_->width;
    }
    const double s = double(w) / src_w;
    cairo_t* c = cairo_create(scaled_);
    cairo_scale(c, s, s);
    cairo_set_source_surface(c, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_GOOD);
    cairo_pattern_set_extend(cairo_get_source(c), CAIRO_EXTEND_PAD);
    cairo_paint(c);
    cairo_destroy(c);
    if (src != surface_)
        cairo_surface_destroy(src);
    scaled_zoom_ = zoom;
    return scaled_;
}

void ImageView::set_video(bool on)
{
    video_ = on;
    scrubbing_ = false;
    video_fraction_ = 0;
    if (!on)
        controls_until_ = 0;
    gtk_widget_queue_draw(area_);
}

void ImageView::set_video_progress(double fraction, bool playing)
{
    if (scrubbing_)
        return;  // the knob follows the pointer while dragging
    const bool changed = std::fabs(fraction - video_fraction_) > 1e-4 || playing != video_playing_;
    video_fraction_ = std::clamp(fraction, 0.0, 1.0);
    video_playing_ = playing;
    if (changed && controls_visible())
        gtk_widget_queue_draw(area_);
}

ImageView::ControlsGeometry ImageView::controls_geometry() const
{
    int vw, vh;
    view_size(vw, vh);
    const double s = scale_factor();
    const double margin = 28 * s;
    return {margin, margin + 30 * s, vw - margin, vh - 34 * s, 7 * s};
}

bool ImageView::controls_visible() const
{
    return video_ && (scrubbing_ || g_get_monotonic_time() < controls_until_);
}

void ImageView::poke_controls()
{
    if (!video_)
        return;
    const bool was = controls_visible();
    controls_until_ = g_get_monotonic_time() + 1500000;
    if (!was)
        gtk_widget_queue_draw(area_);
    if (controls_timer_)
        g_source_remove(controls_timer_);
    controls_timer_ = g_timeout_add(
        1550,
        +[](gpointer self) -> gboolean
        {
            auto* view = static_cast<ImageView*>(self);
            view->controls_timer_ = 0;
            gtk_widget_queue_draw(view->area_);
            return G_SOURCE_REMOVE;
        },
        this);
}

double ImageView::fraction_at(double device_x) const
{
    const ControlsGeometry g = controls_geometry();
    return std::clamp((device_x - g.line_x0) / std::max(1.0, g.line_x1 - g.line_x0), 0.0, 1.0);
}

// -----O------ with a play/pause glyph: white over a dark outline, so it reads
// on any picture without a panel behind it.
void ImageView::draw_video_controls(cairo_t* cr, int scale)
{
    const ControlsGeometry g = controls_geometry();
    const double knob_x = g.line_x0 + (g.line_x1 - g.line_x0) * video_fraction_;
    auto strokes = [&](double r, double gr, double b, double a, double width)
    {
        cairo_set_source_rgba(cr, r, gr, b, a);
        cairo_set_line_width(cr, width);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, g.line_x0, g.y);
        cairo_line_to(cr, g.line_x1, g.y);
        cairo_stroke(cr);
    };
    strokes(0, 0, 0, 0.45, 5.0 * scale);
    strokes(1, 1, 1, 0.45, 2.0 * scale);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);  // played part
    cairo_set_line_width(cr, 2.0 * scale);
    cairo_move_to(cr, g.line_x0, g.y);
    cairo_line_to(cr, knob_x, g.y);
    cairo_stroke(cr);
    cairo_arc(cr, knob_x, g.y, g.radius, 0, 2 * G_PI);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_set_line_width(cr, 2.0 * scale);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_fill(cr);
    // Glyph: pause bars while playing, a triangle while paused.
    const double h = 14 * scale, x = g.glyph_x, top = g.y - h / 2;
    cairo_new_path(cr);
    if (video_playing_)
    {
        cairo_rectangle(cr, x, top, 4 * scale, h);
        cairo_rectangle(cr, x + 8 * scale, top, 4 * scale, h);
    }
    else
    {
        cairo_move_to(cr, x, top);
        cairo_line_to(cr, x + 12 * scale, g.y);
        cairo_line_to(cr, x, top + h);
        cairo_close_path(cr);
    }
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_set_line_width(cr, 2.0 * scale);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_fill(cr);
}

void ImageView::draw(cairo_t* cr)
{
    const int scale = scale_factor();
    cairo_scale(cr, 1.0 / scale, 1.0 / scale);  // from here on, device pixels

    GdkRGBA bg;
    if (!gdk_rgba_parse(&bg, (fullscreen_ ? cfg_.fullscreen_background : cfg_.background).c_str()))
        gdk_rgba_parse(&bg, "#262626");
    gdk_cairo_set_source_rgba(cr, &bg);
    cairo_paint(cr);
    if (!surface_)
        return;

    double fade_alpha = 1.0;
    if (outgoing_.surface)
    {
        const gint64 now = g_get_monotonic_time();
        fade_alpha = std::clamp(double(now - outgoing_.fade_start_us) / std::max<gint64>(1, outgoing_.fade_us), 0.0, 1.0);
        double oz, ox, oy;
        outgoing_transform(now, oz, ox, oy);
        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, oz / outgoing_.scale_zoom, oz / outgoing_.scale_zoom);
        cairo_set_source_surface(cr, outgoing_.surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_NONE);
        cairo_paint(cr);
        cairo_restore(cr);
        // Where the incoming image won't cover it, the outgoing one fades to
        // the background at the same rate, instead of vanishing at the end.
        int vw, vh;
        view_size(vw, vh);
        cairo_save(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle(cr, 0, 0, vw, vh);
        cairo_rectangle(cr, pan_x_, pan_y_, std::ceil(image_->width * zoom_), std::ceil(image_->height * zoom_));
        cairo_clip(cr);
        gdk_cairo_set_source_rgba(cr, &bg);
        cairo_paint_with_alpha(cr, fade_alpha);
        cairo_restore(cr);
    }
    const double sw = image_->width * zoom_, sh = image_->height * zoom_;
    cairo_save(cr);
    cairo_rectangle(cr, pan_x_, pan_y_, std::ceil(sw), std::ceil(sh));
    cairo_clip(cr);
    if (image_->has_alpha && cfg_.checkerboard && !outgoing_.surface)
    {
        cairo_set_source(cr, checker_pattern(scale));
        cairo_paint(cr);
    }
    cairo_filter_t filter = CAIRO_FILTER_NEAREST;
    const gint64 t_draw = g_get_monotonic_time();
    if (motion_.active)
    {
        // Pre-scaled to the motion's largest zoom; each frame only shrinks it a little.
        cairo_surface_t* base = scaled_surface(motion_.z_cache);
        cairo_translate(cr, pan_x_, pan_y_);
        cairo_scale(cr, zoom_ / motion_.z_cache, zoom_ / motion_.z_cache);
        cairo_set_source_surface(cr, base, 0, 0);
        filter = CAIRO_FILTER_BILINEAR;
    }
    else if (zoom_ < 1.0 && cfg_.smooth_downscale && !image_->is_video)
    {
        cairo_set_source_surface(cr, scaled_surface(zoom_), pan_x_, pan_y_);
    }
    else
    {
        cairo_translate(cr, pan_x_, pan_y_);
        cairo_scale(cr, zoom_, zoom_);
        cairo_set_source_surface(cr, surface_, 0, 0);
        if ((zoom_ > 1.0 && cfg_.smooth_upscale) || (zoom_ < 1.0 && image_->is_video))
            filter = CAIRO_FILTER_BILINEAR;
    }
    cairo_pattern_set_filter(cairo_get_source(cr), filter);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    if (fade_alpha < 1.0)
        cairo_paint_with_alpha(cr, fade_alpha);
    else
        cairo_paint(cr);
    cairo_restore(cr);
    if (motion_.active)
        frame_ms_ = (g_get_monotonic_time() - t_draw) / 1000.0;
    if (image_ && presented_ != image_.get())
    {
        presented_ = image_.get();
        if (on_presented)
            g_idle_add(
                +[](gpointer self) -> gboolean
                {
                    auto* view = static_cast<ImageView*>(self);
                    if (view->on_presented)
                        view->on_presented();
                    return G_SOURCE_REMOVE;
                },
                this);
    }
    if (present_since_ && present_image_ == image_.get())
    {
        std::fprintf(stderr, "present: %6.1f ms  %dx%d %s\n", (g_get_monotonic_time() - present_since_) / 1000.0, image_->width, image_->height,
                     image_->is_preview() ? "(preview)" : "");
        present_since_ = 0;
    }

    // Busy ring: bottom-right, small and dim, a short arc chasing round.
    if (busy_)
    {
        int vw, vh;
        view_size(vw, vh);
        const double r = 9.0 * scale, cx = vw - 20.0 * scale, cy = vh - 20.0 * scale;
        const double phase = (g_get_monotonic_time() % 1000000) / 1000000.0 * 2 * G_PI;
        cairo_set_line_width(cr, 2.5 * scale);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
        cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
        cairo_arc(cr, cx, cy, r, phase, phase + G_PI * 0.6);
        cairo_stroke(cr);
    }

    // Info overlay: small text in a contrasting colour over a faint backdrop,
    // one block per category with a thin rule between them.
    if (cfg_.overlay_enabled && !overlay_.empty() && (fullscreen_ || cfg_.overlay_windowed))
    {
        GdkRGBA colour;
        if (!gdk_rgba_parse(&colour, cfg_.overlay_color.c_str()))
            gdk_rgba_parse(&colour, "#a6d96a");
        PangoFontDescription* font = pango_font_description_from_string("Sans");
        pango_font_description_set_size(font, cfg_.overlay_font_pt * PANGO_SCALE * scale);  // device pixels
        std::vector<PangoLayout*> layouts;
        int width = 0, height = 0;
        const double gap = 8.0 * scale;  // space around each rule
        for (const auto& section : overlay_)
        {
            std::string text;
            for (const std::string& l : section)
                text += (text.empty() ? "" : "\n") + l;
            PangoLayout* layout = pango_cairo_create_layout(cr);
            pango_layout_set_font_description(layout, font);
            pango_layout_set_text(layout, text.c_str(), -1);
            int tw = 0, th = 0;
            pango_layout_get_pixel_size(layout, &tw, &th);
            width = std::max(width, tw);
            height += th + (layouts.empty() ? 0 : int(2 * gap + 1));
            layouts.push_back(layout);
        }
        pango_font_description_free(font);
        const double pad = 10.0 * scale, margin = 16.0 * scale;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_rectangle(cr, margin, margin, width + 2 * pad, height + 2 * pad);
        cairo_fill(cr);
        double y = margin + pad;
        for (size_t i = 0; i < layouts.size(); ++i)
        {
            if (i > 0)
            {
                y += gap;
                cairo_set_source_rgba(cr, colour.red, colour.green, colour.blue, 0.5);
                cairo_rectangle(cr, margin + pad, std::floor(y), width, 1.0 * scale);
                cairo_fill(cr);
                y += gap + 1;
            }
            gdk_cairo_set_source_rgba(cr, &colour);
            cairo_move_to(cr, margin + pad, y);
            pango_cairo_show_layout(cr, layouts[i]);
            int tw = 0, th = 0;
            pango_layout_get_pixel_size(layouts[i], &tw, &th);
            y += th;
            g_object_unref(layouts[i]);
        }
    }

    if (controls_visible())
        draw_video_controls(cr, scale);

    if (has_selection_)
    {
        // Marching-ants style rectangle: a white line with black dashes on top.
        const double x = std::floor(pan_x_ + sel_x0_ * zoom_) + 0.5, y = std::floor(pan_y_ + sel_y0_ * zoom_) + 0.5;
        const double w = std::round((sel_x1_ - sel_x0_) * zoom_) - 1, h = std::round((sel_y1_ - sel_y0_) * zoom_) - 1;
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, x, y, w, h);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_stroke_preserve(cr);
        const double dashes[] = {4.0 * scale, 4.0 * scale};
        cairo_set_dash(cr, dashes, 2, 0);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_stroke(cr);
    }
}

void ImageView::to_image(double x, double y, double& ix, double& iy) const
{
    const int scale = scale_factor();
    ix = (x * scale - pan_x_) / zoom_;
    iy = (y * scale - pan_y_) / zoom_;
}

void ImageView::update_selection(double x, double y)
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
    gtk_widget_queue_draw(area_);
}

void ImageView::zoom_to_selection()
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
    gtk_widget_queue_draw(area_);
    zoom_changed();
}

// ------------------------------------------------------------- bindings

void ImageView::run_click(Action a, double x, double y)
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
            gtk_widget_queue_draw(area_);
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

void ImageView::begin_drag(Action a, double x, double y)
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

void ImageView::drag_to(Action a, double x, double y)
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
        gtk_widget_queue_draw(area_);
    }
    drag_x_ = x;
    drag_y_ = y;
}

void ImageView::run_wheel(Action a, int direction, double x, double y)
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
            gtk_widget_queue_draw(area_);
            return;
        default:
            return;
    }
}

namespace
{

Control click_control(unsigned button, bool twice)
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

Control drag_control(unsigned button)
{
    return button == 1 ? Control::LeftDrag : button == 2 ? Control::MiddleDrag : Control::RightDrag;
}

}  // namespace

gboolean ImageView::on_button(GdkEventButton* e)
{
    if (video_ && e->button == 1)
    {
        const int s = scale_factor();
        const double dx = e->x * s, dy = e->y * s;
        const ControlsGeometry g = controls_geometry();
        if (e->type == GDK_BUTTON_RELEASE && scrubbing_)
        {
            scrubbing_ = false;
            if (on_video_seek)
                on_video_seek(fraction_at(dx), true);
            poke_controls();
            return TRUE;
        }
        if (e->type == GDK_BUTTON_PRESS && controls_visible() && std::fabs(dy - g.y) < 16 * s)
        {
            if (dx < g.line_x0 - 6 * s)
            {
                if (on_video_toggle)
                    on_video_toggle();
            }
            else
            {
                scrubbing_ = true;
                video_fraction_ = fraction_at(dx);
                if (on_video_seek)
                    on_video_seek(video_fraction_, false);
            }
            gtk_widget_queue_draw(area_);
            return TRUE;
        }
        if ((e->type == GDK_2BUTTON_PRESS || e->type == GDK_BUTTON_RELEASE) && controls_visible() && std::fabs(dy - g.y) < 16 * s)
            return TRUE;  // the rest of a click on the controls
    }
    if (e->button < 1 || e->button > 3)
        return FALSE;
    // Fullscreen is for flicking through: plain left/right clicks have their
    // own bindings there, and there is no selecting.
    // In fullscreen a click is held back for the double-click interval when
    // the same button also has a double-click action, so one gesture can't
    // fire both.
    if (fullscreen_ && e->type == GDK_BUTTON_RELEASE && e->button == pressed_button_ && !drag_active_ && !suppress_click_ &&
        cfg_.control(click_control(e->button, true), true) != Action::None)
    {
        pressed_button_ = 0;
        cancel_deferred_click();
        deferred_action_ = cfg_.control(click_control(e->button, false), true);
        deferred_x_ = e->x;
        deferred_y_ = e->y;
        gint interval = 400;
        g_object_get(gtk_widget_get_settings(area_), "gtk-double-click-time", &interval, nullptr);
        deferred_click_ = g_timeout_add(
            guint(interval),
            +[](gpointer p) -> gboolean
            {
                auto* self = static_cast<ImageView*>(p);
                self->deferred_click_ = 0;
                self->run_click(self->deferred_action_, self->deferred_x_, self->deferred_y_);
                return G_SOURCE_REMOVE;
            },
            this);
        return TRUE;
    }
    if (e->type == GDK_2BUTTON_PRESS)
    {
        suppress_click_ = true;
        cancel_deferred_click();  // the first click of the pair never fires
        run_click(cfg_.control(click_control(e->button, true), fullscreen_), e->x, e->y);
    }
    else if (e->type == GDK_BUTTON_PRESS)
    {
        gtk_widget_grab_focus(area_);
        if (pressed_button_)
            return TRUE;  // a second button during a drag is ignored
        pressed_button_ = e->button;
        drag_active_ = false;
        suppress_click_ = false;
        press_x_ = drag_x_ = e->x;
        press_y_ = drag_y_ = e->y;
    }
    else if (e->type == GDK_BUTTON_RELEASE && e->button == pressed_button_)
    {
        const unsigned button = pressed_button_;
        pressed_button_ = 0;
        if (drag_active_)
            drag_to(cfg_.control(drag_control(button), fullscreen_), e->x, e->y);
        else if (!suppress_click_)
            run_click(cfg_.control(click_control(button, false), fullscreen_), e->x, e->y);
        drag_active_ = false;
        update_cursor(e->x, e->y);
        if (on_selection_changed)
            on_selection_changed();
    }
    return TRUE;
}

gboolean ImageView::on_motion(GdkEventMotion* e)
{
    poke_controls();
    if (scrubbing_)
    {
        video_fraction_ = fraction_at(e->x * scale_factor());
        if (on_video_seek)
            on_video_seek(video_fraction_, false);
        gtk_widget_queue_draw(area_);
        return TRUE;
    }
    if (!pressed_button_)
    {
        update_cursor(e->x, e->y);
        return FALSE;
    }
    const Action a = cfg_.control(drag_control(pressed_button_), fullscreen_);
    if (!drag_active_)
    {
        if (std::hypot(e->x - press_x_, e->y - press_y_) < 4.0)
            return TRUE;
        drag_active_ = true;
        begin_drag(a, press_x_, press_y_);
        update_cursor(e->x, e->y);
    }
    drag_to(a, e->x, e->y);
    return TRUE;
}

gboolean ImageView::on_scroll(GdkEventScroll* e)
{
    int direction = 0;
    if (e->direction == GDK_SCROLL_SMOOTH)
    {
        // A wheel click on Wayland arrives as both a discrete event and an
        // emulated smooth one; the discrete event is enough.
        if (gdk_event_get_pointer_emulated(reinterpret_cast<GdkEvent*>(e)))
            return TRUE;
        // Some compositors deliver a wheel notch as 0.9 rather than 1.0, so
        // fire at half a notch and carry the remainder; a change of direction
        // starts afresh so the carry can't swallow the first tick back.
        if (e->delta_y * scroll_accum_ < 0)
            scroll_accum_ = 0;
        scroll_accum_ += e->delta_y;
        if (std::fabs(scroll_accum_) >= 0.5)
        {
            direction = scroll_accum_ > 0 ? 1 : -1;
            scroll_accum_ -= direction;
            if (std::fabs(scroll_accum_) >= 0.5)
                scroll_accum_ = 0;  // large touchpad flicks: one step, not a burst
        }
    }
    else if (e->direction == GDK_SCROLL_UP)
    {
        direction = -1;
    }
    else if (e->direction == GDK_SCROLL_DOWN)
    {
        direction = 1;
    }
    else
    {
        return FALSE;
    }
    if (direction == 0)
        return TRUE;
    const Control c = (e->state & GDK_CONTROL_MASK) ? Control::CtrlWheel : (e->state & GDK_SHIFT_MASK) ? Control::ShiftWheel : Control::Wheel;
    run_wheel(cfg_.control(c, fullscreen_), direction, e->x, e->y);
    return TRUE;
}
