#include "viewer.h"

#include "config.h"
#include "controls.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kZoomStep = 1.25;
constexpr double kMinZoom = 0.02;
constexpr double kMaxZoom = 64.0;

}  // namespace

ImageView::ImageView(const Config& cfg) : cfg_(cfg) {
    area_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(area_, 64, 64);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_widget_set_can_focus(area_, TRUE);
    gtk_widget_add_events(area_, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                     GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);

    g_signal_connect(area_, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* cr, gpointer self) -> gboolean {
                         static_cast<ImageView*>(self)->draw(cr);
                         return TRUE;
                     }), this);
    g_signal_connect(area_, "size-allocate", G_CALLBACK(+[](GtkWidget*, GdkRectangle*, gpointer self) {
                         static_cast<ImageView*>(self)->layout();
                     }), this);
    g_signal_connect(area_, "notify::scale-factor", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer self) {
                         static_cast<ImageView*>(self)->layout();
                     }), this);
    g_signal_connect(area_, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* e, gpointer self) {
                         return static_cast<ImageView*>(self)->on_button(e);
                     }), this);
    g_signal_connect(area_, "button-release-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* e, gpointer self) {
                         return static_cast<ImageView*>(self)->on_button(e);
                     }), this);
    g_signal_connect(area_, "motion-notify-event", G_CALLBACK(+[](GtkWidget*, GdkEventMotion* e, gpointer self) {
                         return static_cast<ImageView*>(self)->on_motion(e);
                     }), this);
    g_signal_connect(area_, "scroll-event", G_CALLBACK(+[](GtkWidget*, GdkEventScroll* e, gpointer self) {
                         return static_cast<ImageView*>(self)->on_scroll(e);
                     }), this);
}

void ImageView::cancel_deferred_click() {
    if (deferred_click_) g_source_remove(deferred_click_);
    deferred_click_ = 0;
}

ImageView::~ImageView() {
    cancel_deferred_click();
    drop_caches();
    if (surface_) cairo_surface_destroy(surface_);
    if (checker_) cairo_pattern_destroy(checker_);
}

namespace {
Control drag_control(unsigned button);
}  // namespace

// A magnifier over the selection says "click here to zoom in"; a closed
// hand while panning.
void ImageView::update_cursor(double x, double y) {
    const char* want = nullptr;
    if (drag_active_ && pressed_button_ && cfg_.control(drag_control(pressed_button_), fullscreen_) == Action::Pan) {
        want = "grabbing";
    } else if (has_selection_ && image_ && !pressed_button_) {
        double ix, iy;
        to_image(x, y, ix, iy);
        if (ix >= sel_x0_ && ix < sel_x1_ && iy >= sel_y0_ && iy < sel_y1_) want = "zoom-in";
    }
    if (want == cursor_name_) return;
    cursor_name_ = want;
    GdkWindow* gw = gtk_widget_get_window(area_);
    if (!gw) return;
    GdkCursor* cursor = want ? gdk_cursor_new_from_name(gtk_widget_get_display(area_), want) : nullptr;
    gdk_window_set_cursor(gw, cursor);
    if (cursor) g_object_unref(cursor);
}

int ImageView::scale_factor() const {
    return std::max(1, gtk_widget_get_scale_factor(area_));
}

void ImageView::view_size(int& w, int& h) const {
    const int scale = scale_factor();
    w = gtk_widget_get_allocated_width(area_) * scale;
    h = gtk_widget_get_allocated_height(area_) * scale;
}

void ImageView::set_image(std::shared_ptr<Image> image) {
    drop_caches();
    if (surface_) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }
    image_ = std::move(image);
    if (image_ && image_->width > 0 && image_->height > 0) {
        surface_ = cairo_image_surface_create_for_data(image_->pixels.data(), CAIRO_FORMAT_ARGB32,
                                                       image_->width, image_->height, image_->stride);
    }
    has_selection_ = false;
    drag_active_ = false;
    pressed_button_ = 0;
    mode_ = ZoomMode::Fit;
    layout();
    zoom_changed();
    if (on_selection_changed) on_selection_changed();
}

void ImageView::set_fullscreen(bool on) {
    fullscreen_ = on;
    if (on) clear_selection();
    zoom_fit();
}

void ImageView::image_changed() {
    drop_caches();
    if (surface_) cairo_surface_mark_dirty(surface_);
    gtk_widget_queue_draw(area_);
}

bool ImageView::selection(int& x, int& y, int& w, int& h) const {
    if (!has_selection_) return false;
    x = int(sel_x0_);
    y = int(sel_y0_);
    w = int(sel_x1_ - sel_x0_);
    h = int(sel_y1_ - sel_y0_);
    return true;
}

void ImageView::clear_selection() {
    if (!has_selection_) return;
    has_selection_ = false;
    gtk_widget_queue_draw(area_);
    if (on_selection_changed) on_selection_changed();
}

void ImageView::config_changed() {
    drop_caches();
    layout();
}

void ImageView::drop_caches() {
    if (scaled_) {
        cairo_surface_destroy(scaled_);
        scaled_ = nullptr;
    }
    scaled_zoom_ = 0;
}

void ImageView::zoom_changed() {
    if (on_zoom_changed) on_zoom_changed();
}

double ImageView::fit_zoom() const {
    int vw, vh;
    view_size(vw, vh);
    if (!image_ || vw <= 0 || vh <= 0) return 1.0;
    double z = std::min(double(vw) / image_->width, double(vh) / image_->height);
    if (!(fullscreen_ && cfg_.fullscreen_upscale)) z = std::min(z, 1.0);
    // A window sized for the image may be a pixel or two larger from rounding to logical units.
    if (std::fabs(z - 1.0) < 0.01) z = 1.0;
    return std::clamp(z, kMinZoom, kMaxZoom);
}

void ImageView::layout() {
    if (mode_ == ZoomMode::Fit) {
        const double z = fit_zoom();
        if (z != zoom_) {
            zoom_ = z;
            zoom_changed();
        }
    }
    clamp_pan();
    gtk_widget_queue_draw(area_);
}

void ImageView::clamp_pan() {
    if (!image_) return;
    int vw, vh;
    view_size(vw, vh);
    const double sw = image_->width * zoom_, sh = image_->height * zoom_;
    if (sw <= vw) pan_x_ = (vw - sw) / 2;
    else pan_x_ = std::clamp(pan_x_, vw - sw, 0.0);
    if (sh <= vh) pan_y_ = (vh - sh) / 2;
    else pan_y_ = std::clamp(pan_y_, vh - sh, 0.0);
    // Integer origins keep 1:1 rendering crisp.
    pan_x_ = std::floor(pan_x_);
    pan_y_ = std::floor(pan_y_);
}

void ImageView::set_zoom(double z, double anchor_x, double anchor_y) {
    z = std::clamp(z, kMinZoom, kMaxZoom);
    if (std::fabs(z - 1.0) < 0.03) z = 1.0;
    if (!image_) return;
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

void ImageView::zoom_step_at(int direction, double x, double y) {
    const double factor = direction > 0 ? kZoomStep : 1.0 / kZoomStep;
    set_zoom(zoom_ * factor, x, y);
}

void ImageView::zoom_step(int direction) {
    int vw, vh;
    view_size(vw, vh);
    zoom_step_at(direction, vw / 2.0, vh / 2.0);
}

void ImageView::zoom_original() {
    int vw, vh;
    view_size(vw, vh);
    set_zoom(1.0, vw / 2.0, vh / 2.0);
}

void ImageView::zoom_fit() {
    mode_ = ZoomMode::Fit;
    layout();
    zoom_changed();
}

cairo_pattern_t* ImageView::checker_pattern(int scale) {
    if (checker_ && checker_scale_ == scale) return checker_;
    if (checker_) cairo_pattern_destroy(checker_);
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
cairo_surface_t* ImageView::scaled_surface() {
    if (scaled_ && scaled_zoom_ == zoom_) return scaled_;
    drop_caches();
    const int w = std::max(1, int(std::ceil(image_->width * zoom_)));
    const int h = std::max(1, int(std::ceil(image_->height * zoom_)));
    scaled_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* c = cairo_create(scaled_);
    cairo_scale(c, zoom_, zoom_);
    cairo_set_source_surface(c, surface_, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_GOOD);
    cairo_pattern_set_extend(cairo_get_source(c), CAIRO_EXTEND_PAD);
    cairo_paint(c);
    cairo_destroy(c);
    scaled_zoom_ = zoom_;
    return scaled_;
}

void ImageView::draw(cairo_t* cr) {
    const int scale = scale_factor();
    cairo_scale(cr, 1.0 / scale, 1.0 / scale);  // from here on, device pixels

    GdkRGBA bg;
    if (!gdk_rgba_parse(&bg, (fullscreen_ ? cfg_.fullscreen_background : cfg_.background).c_str()))
        gdk_rgba_parse(&bg, "#262626");
    gdk_cairo_set_source_rgba(cr, &bg);
    cairo_paint(cr);
    if (!surface_) return;

    const double sw = image_->width * zoom_, sh = image_->height * zoom_;
    cairo_save(cr);
    cairo_rectangle(cr, pan_x_, pan_y_, std::ceil(sw), std::ceil(sh));
    cairo_clip(cr);
    if (image_->has_alpha && cfg_.checkerboard) {
        cairo_set_source(cr, checker_pattern(scale));
        cairo_paint(cr);
    }
    cairo_filter_t filter = CAIRO_FILTER_NEAREST;
    if (zoom_ < 1.0 && cfg_.smooth_downscale) {
        cairo_set_source_surface(cr, scaled_surface(), pan_x_, pan_y_);
    } else {
        cairo_translate(cr, pan_x_, pan_y_);
        cairo_scale(cr, zoom_, zoom_);
        cairo_set_source_surface(cr, surface_, 0, 0);
        if (zoom_ > 1.0 && cfg_.smooth_upscale) filter = CAIRO_FILTER_BILINEAR;
    }
    cairo_pattern_set_filter(cairo_get_source(cr), filter);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
    cairo_restore(cr);

    if (has_selection_) {
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

void ImageView::to_image(double x, double y, double& ix, double& iy) const {
    const int scale = scale_factor();
    ix = (x * scale - pan_x_) / zoom_;
    iy = (y * scale - pan_y_) / zoom_;
}

void ImageView::update_selection(double x, double y) {
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

void ImageView::zoom_to_selection() {
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

void ImageView::run_click(Action a, double x, double y) {
    switch (a) {
        case Action::ZoomSelection: {
            if (!image_) return;
            double ix, iy;
            to_image(x, y, ix, iy);
            if (has_selection_ && ix >= sel_x0_ && ix < sel_x1_ && iy >= sel_y0_ && iy < sel_y1_) zoom_to_selection();
            else if (mode_ == ZoomMode::Manual) zoom_fit();
            has_selection_ = false;
            gtk_widget_queue_draw(area_);
            return;
        }
        case Action::ZoomFit: zoom_fit(); return;
        case Action::ZoomOriginal: zoom_original(); return;
        case Action::None: return;
        default:
            if (on_action) on_action(a);
            return;
    }
}

void ImageView::begin_drag(Action a, double x, double y) {
    if (a == Action::Select && image_) {
        double ix, iy;
        to_image(x, y, ix, iy);
        sel_anchor_x_ = std::clamp(ix, 0.0, double(image_->width));
        sel_anchor_y_ = std::clamp(iy, 0.0, double(image_->height));
        has_selection_ = false;
    }
}

void ImageView::drag_to(Action a, double x, double y) {
    if (a == Action::Select) {
        if (image_) update_selection(x, y);
    } else if (a == Action::Pan) {
        const int scale = scale_factor();
        pan_x_ += (x - drag_x_) * scale;
        pan_y_ += (y - drag_y_) * scale;
        clamp_pan();
        gtk_widget_queue_draw(area_);
    }
    drag_x_ = x;
    drag_y_ = y;
}

void ImageView::run_wheel(Action a, int direction, double x, double y) {
    const int scale = scale_factor();
    switch (a) {
        case Action::Zoom: zoom_step_at(-direction, x * scale, y * scale); return;   // wheel up zooms in
        case Action::Navigate: if (on_action) on_action(direction > 0 ? Action::NextImage : Action::PrevImage); return;
        case Action::Scroll:
            pan_y_ -= direction * 64.0 * scale;
            clamp_pan();
            gtk_widget_queue_draw(area_);
            return;
        default: return;
    }
}

namespace {

Control click_control(unsigned button, bool twice) {
    switch (button) {
        case 1: return twice ? Control::LeftDouble : Control::LeftClick;
        case 2: return twice ? Control::MiddleDouble : Control::MiddleClick;
        default: return twice ? Control::RightDouble : Control::RightClick;
    }
}

Control drag_control(unsigned button) {
    return button == 1 ? Control::LeftDrag : button == 2 ? Control::MiddleDrag : Control::RightDrag;
}

}  // namespace

gboolean ImageView::on_button(GdkEventButton* e) {
    if (e->button < 1 || e->button > 3) return FALSE;
    // Fullscreen is for flicking through: plain left/right clicks have their
    // own bindings there, and there is no selecting.
    // In fullscreen a click is held back for the double-click interval when
    // the same button also has a double-click action, so one gesture can't
    // fire both.
    if (fullscreen_ && e->type == GDK_BUTTON_RELEASE && e->button == pressed_button_ && !drag_active_ && !suppress_click_ &&
        cfg_.control(click_control(e->button, true), true) != Action::None) {
        pressed_button_ = 0;
        cancel_deferred_click();
        deferred_action_ = cfg_.control(click_control(e->button, false), true);
        deferred_x_ = e->x;
        deferred_y_ = e->y;
        gint interval = 400;
        g_object_get(gtk_widget_get_settings(area_), "gtk-double-click-time", &interval, nullptr);
        deferred_click_ = g_timeout_add(guint(interval), +[](gpointer p) -> gboolean {
            auto* self = static_cast<ImageView*>(p);
            self->deferred_click_ = 0;
            self->run_click(self->deferred_action_, self->deferred_x_, self->deferred_y_);
            return G_SOURCE_REMOVE;
        }, this);
        return TRUE;
    }
    if (e->type == GDK_2BUTTON_PRESS) {
        suppress_click_ = true;
        cancel_deferred_click();   // the first click of the pair never fires
        run_click(cfg_.control(click_control(e->button, true), fullscreen_), e->x, e->y);
    } else if (e->type == GDK_BUTTON_PRESS) {
        gtk_widget_grab_focus(area_);
        if (pressed_button_) return TRUE;  // a second button during a drag is ignored
        pressed_button_ = e->button;
        drag_active_ = false;
        suppress_click_ = false;
        press_x_ = drag_x_ = e->x;
        press_y_ = drag_y_ = e->y;
    } else if (e->type == GDK_BUTTON_RELEASE && e->button == pressed_button_) {
        const unsigned button = pressed_button_;
        pressed_button_ = 0;
        if (drag_active_) drag_to(cfg_.control(drag_control(button), fullscreen_), e->x, e->y);
        else if (!suppress_click_) run_click(cfg_.control(click_control(button, false), fullscreen_), e->x, e->y);
        drag_active_ = false;
        update_cursor(e->x, e->y);
        if (on_selection_changed) on_selection_changed();
    }
    return TRUE;
}

gboolean ImageView::on_motion(GdkEventMotion* e) {
    if (!pressed_button_) {
        update_cursor(e->x, e->y);
        return FALSE;
    }
    const Action a = cfg_.control(drag_control(pressed_button_), fullscreen_);
    if (!drag_active_) {
        if (std::hypot(e->x - press_x_, e->y - press_y_) < 4.0) return TRUE;
        drag_active_ = true;
        begin_drag(a, press_x_, press_y_);
        update_cursor(e->x, e->y);
    }
    drag_to(a, e->x, e->y);
    return TRUE;
}

gboolean ImageView::on_scroll(GdkEventScroll* e) {
    int direction = 0;
    if (e->direction == GDK_SCROLL_SMOOTH) {
        // A wheel click on Wayland arrives as both a discrete event and an
        // emulated smooth one; the discrete event is enough.
        if (gdk_event_get_pointer_emulated(reinterpret_cast<GdkEvent*>(e))) return TRUE;
        scroll_accum_ += e->delta_y;
        if (std::fabs(scroll_accum_) >= 1.0) {
            direction = scroll_accum_ > 0 ? 1 : -1;
            scroll_accum_ = 0;
        }
    } else if (e->direction == GDK_SCROLL_UP) {
        direction = -1;
    } else if (e->direction == GDK_SCROLL_DOWN) {
        direction = 1;
    } else {
        return FALSE;
    }
    if (direction == 0) return TRUE;
    const Control c = (e->state & GDK_CONTROL_MASK) ? Control::CtrlWheel
                    : (e->state & GDK_SHIFT_MASK) ? Control::ShiftWheel : Control::Wheel;
    run_wheel(cfg_.control(c, fullscreen_), direction, e->x, e->y);
    return TRUE;
}
