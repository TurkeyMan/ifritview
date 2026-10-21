#pragma once

#include "controls.h"
#include "image.h"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct Config;

// The image panel. Everything else in the window follows the UI scale
// factor; this widget deliberately renders device pixels 1:1, so a zoom of
// 100% is one image pixel per screen pixel whatever the desktop scaling is.
class ImageView
{
public:
    explicit ImageView(const Config& cfg);
    ~ImageView();
    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;

    GtkWidget* widget() const { return area_; }

    void set_image(std::shared_ptr<Image> image);  // resets zoom to fit
    const Image* image() const { return image_.get(); }
    void set_fullscreen(bool on);
    void config_changed();
    void image_changed();  // pixels were rewritten in place
    using OverlaySections = std::vector<std::vector<std::string>>;
    void set_overlay(OverlaySections sections);  // info drawn over the image (fullscreen HUD), a rule between sections

    enum class ZoomMode
    {
        Fit,
        Manual
    };
    ZoomMode zoom_mode() const { return mode_; }
    double zoom() const { return zoom_; }
    void zoom_fit();
    void zoom_original();
    void zoom_step(int direction);  // around the view centre
    int scale_factor() const;
    void view_size(int& w, int& h) const;  // device pixels

    // Current selection in image pixels; false when there is none.
    bool selection(int& x, int& y, int& w, int& h) const;
    void clear_selection();

    std::function<void()> on_zoom_changed;
    std::function<void()> on_selection_changed;
    std::function<void(Action)> on_action;  // app-level actions the view can't do itself

private:
    void layout();
    double fit_zoom() const;
    void clamp_pan();
    void set_zoom(double z, double anchor_x, double anchor_y);  // anchor in device pixels
    void zoom_step_at(int direction, double x, double y);
    void zoom_to_selection();
    void update_selection(double x, double y);  // x,y logical px
    void to_image(double x, double y, double& ix, double& iy) const;
    void update_cursor(double x, double y);  // logical px
    void run_click(Action a, double x, double y);
    void begin_drag(Action a, double x, double y);
    void drag_to(Action a, double x, double y);
    void run_wheel(Action a, int direction, double x, double y);
    void draw(cairo_t* cr);
    cairo_surface_t* scaled_surface();
    cairo_pattern_t* checker_pattern(int scale);
    void drop_caches();
    void zoom_changed();
    gboolean on_button(GdkEventButton* e);
    gboolean on_motion(GdkEventMotion* e);
    gboolean on_scroll(GdkEventScroll* e);

    const Config& cfg_;
    OverlaySections overlay_;
    GtkWidget* area_ = nullptr;
    std::shared_ptr<Image> image_;
    cairo_surface_t* surface_ = nullptr;  // wraps image_->pixels
    cairo_surface_t* scaled_ = nullptr;   // cached downscaled rendering
    double scaled_zoom_ = 0;
    cairo_pattern_t* checker_ = nullptr;
    const char* cursor_name_ = nullptr;  // currently set named cursor, null for the default
    int checker_scale_ = 0;

    ZoomMode mode_ = ZoomMode::Fit;
    double zoom_ = 1.0;
    double pan_x_ = 0, pan_y_ = 0;  // image origin in device pixels
    bool fullscreen_ = false;
    // Mouse state: which button is down, where it went down, and whether
    // the pointer has moved far enough for it to count as a drag.
    unsigned pressed_button_ = 0;
    bool drag_active_ = false;
    bool suppress_click_ = false;                 // a double-click consumed this press
    double press_x_ = 0, press_y_ = 0;            // logical px
    double drag_x_ = 0, drag_y_ = 0;              // last pointer position, logical px
    double sel_anchor_x_ = 0, sel_anchor_y_ = 0;  // image coords where the drag began
    bool has_selection_ = false;
    double sel_x0_ = 0, sel_y0_ = 0, sel_x1_ = 0, sel_y1_ = 0;  // image coords, normalised
    double scroll_accum_ = 0;
    // In fullscreen a single click is held back for the double-click interval
    // so a double-click doesn't also step through two images.
    guint deferred_click_ = 0;
    Action deferred_action_ = Action::None;
    double deferred_x_ = 0, deferred_y_ = 0;
    void cancel_deferred_click();
};
