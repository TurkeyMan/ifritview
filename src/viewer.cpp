#include "viewer.h"

#include "config.h"

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

ImageView::~ImageView() {
    drop_caches();
    if (surface_) cairo_surface_destroy(surface_);
    if (checker_) cairo_pattern_destroy(checker_);
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
    mode_ = ZoomMode::Fit;
    layout();
    zoom_changed();
}

void ImageView::set_fullscreen(bool on) {
    fullscreen_ = on;
    zoom_fit();
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
}

gboolean ImageView::on_button(GdkEventButton* e) {
    if (e->button != 1) return FALSE;
    if (e->type == GDK_2BUTTON_PRESS) {
        dragging_ = false;
        if (on_toggle_fullscreen) on_toggle_fullscreen();
    } else if (e->type == GDK_BUTTON_PRESS) {
        dragging_ = true;
        drag_x_ = e->x;
        drag_y_ = e->y;
    } else if (e->type == GDK_BUTTON_RELEASE) {
        dragging_ = false;
    }
    return TRUE;
}

gboolean ImageView::on_motion(GdkEventMotion* e) {
    if (!dragging_ || !(e->state & GDK_BUTTON1_MASK)) return FALSE;
    const int scale = scale_factor();
    pan_x_ += (e->x - drag_x_) * scale;
    pan_y_ += (e->y - drag_y_) * scale;
    drag_x_ = e->x;
    drag_y_ = e->y;
    clamp_pan();
    gtk_widget_queue_draw(area_);
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

    if (e->state & GDK_CONTROL_MASK) {
        const int scale = scale_factor();
        zoom_step_at(-direction, e->x * scale, e->y * scale);  // wheel up zooms in
    } else if (on_navigate) {
        on_navigate(direction);
    }
    return TRUE;
}
