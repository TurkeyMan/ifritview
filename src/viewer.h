#pragma once

#include "image.h"

#include <gtk/gtk.h>

#include <functional>
#include <memory>

struct Config;

// The image panel. Everything else in the window follows the UI scale
// factor; this widget deliberately renders device pixels 1:1, so a zoom of
// 100% is one image pixel per screen pixel whatever the desktop scaling is.
class ImageView {
public:
    explicit ImageView(const Config& cfg);
    ~ImageView();
    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;

    GtkWidget* widget() const { return area_; }

    void set_image(std::shared_ptr<Image> image);   // resets zoom to fit
    const Image* image() const { return image_.get(); }
    void set_fullscreen(bool on);
    void config_changed();

    enum class ZoomMode { Fit, Manual };
    ZoomMode zoom_mode() const { return mode_; }
    double zoom() const { return zoom_; }
    void zoom_fit();
    void zoom_original();
    void zoom_step(int direction);                  // around the view centre
    int scale_factor() const;
    void view_size(int& w, int& h) const;           // device pixels

    std::function<void()> on_zoom_changed;
    std::function<void()> on_toggle_fullscreen;
    std::function<void(int)> on_navigate;           // +1 next, -1 previous

private:
    void layout();
    double fit_zoom() const;
    void clamp_pan();
    void set_zoom(double z, double anchor_x, double anchor_y);   // anchor in device pixels
    void zoom_step_at(int direction, double x, double y);
    void draw(cairo_t* cr);
    cairo_surface_t* scaled_surface();
    cairo_pattern_t* checker_pattern(int scale);
    void drop_caches();
    void zoom_changed();
    gboolean on_button(GdkEventButton* e);
    gboolean on_motion(GdkEventMotion* e);
    gboolean on_scroll(GdkEventScroll* e);

    const Config& cfg_;
    GtkWidget* area_ = nullptr;
    std::shared_ptr<Image> image_;
    cairo_surface_t* surface_ = nullptr;     // wraps image_->pixels
    cairo_surface_t* scaled_ = nullptr;      // cached downscaled rendering
    double scaled_zoom_ = 0;
    cairo_pattern_t* checker_ = nullptr;
    int checker_scale_ = 0;

    ZoomMode mode_ = ZoomMode::Fit;
    double zoom_ = 1.0;
    double pan_x_ = 0, pan_y_ = 0;           // image origin in device pixels
    bool fullscreen_ = false;
    bool dragging_ = false;
    double drag_x_ = 0, drag_y_ = 0;
    double scroll_accum_ = 0;
};
