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
    void image_changed();  // the image's pixels (and maybe its size) changed in place
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
    // Slideshow motion: zoom/pan smoothly between two framings over
    // `duration_ms`, drawing from a surface pre-scaled to the larger zoom so
    // each frame is a cheap bilinear blit. Centres are image coordinates.
    // `start_us` lets a replacement image (preview -> developed) continue a motion in phase; 0 = now.
    void begin_motion(double zoom0, double zoom1, double cx0, double cy0, double cx1, double cy1, int duration_ms, gint64 start_us = 0);
    void end_motion();
    // On the next set_image, keep the current rendering and fade the new one
    // in over `ms` (the outgoing slide keeps moving meanwhile).
    void crossfade_next(int ms);
    // ...and the outgoing image keeps changing (a video playing out under the fade).
    void crossfade_live();
    void outgoing_changed() { gtk_widget_queue_draw(area_); }
    bool motion_active() const { return motion_.active; }
    double frame_ms() const { return frame_ms_; }  // last measured draw cost
    void set_busy(bool busy);
    // Trace aid: report the time from `since_us` to the first paint that shows `image`.
    void stamp_present(gint64 since_us, const Image* image)
    {
        present_since_ = since_us;
        present_image_ = image;
    }  // a small spinning ring in the corner while a decode runs

    void zoom_step(int direction);  // around the view centre
    int scale_factor() const;
    void view_size(int& w, int& h) const;  // device pixels

    // Current selection in image pixels; false when there is none.
    bool selection(int& x, int& y, int& w, int& h) const;
    void clear_selection();

    std::function<void()> on_zoom_changed;
    std::function<void()> on_presented;  // first paint of a newly set image

    // Video controls: a line with a position knob (and play/pause glyph) over
    // the bottom of the picture, shown while the mouse moves.
    void set_video(bool on, bool audio);
    void set_video_progress(double fraction, bool playing);
    void set_video_muted(bool muted);
    void set_video_volume(double volume);
    void poke_controls();  // the mouse moved: show the controls for a moment
    std::function<void()> on_video_toggle;
    std::function<void()> on_video_mute;
    std::function<void(double volume)> on_video_volume;
    std::function<void(double fraction, bool final)> on_video_seek;
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
    cairo_surface_t* scaled_surface(double zoom);
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
    struct Motion
    {
        bool active = false;
        double z0 = 1, z1 = 1, cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
        double z_cache = 1;
        gint64 start_us = 0, duration_us = 0;
        guint tick = 0;
    } motion_;
    double frame_ms_ = 0;
    gint64 present_since_ = 0;
    bool video_ = false;
    double video_fraction_ = 0;
    bool video_playing_ = false;
    bool video_muted_ = true;
    bool video_audio_ = false;  // no audio stream: no speaker or volume
    double video_volume_ = 0.7;
    bool volume_dragging_ = false;
    gint64 controls_until_ = 0;  // controls visible until this time
    guint controls_timer_ = 0;
    bool scrubbing_ = false;
    struct ControlsGeometry
    {
        double glyph_x, line_x0, line_x1, speaker_x, y, radius;
        double volume_top, volume_bottom;  // the vertical volume line above the speaker
    };
    ControlsGeometry controls_geometry() const;
    bool controls_visible() const;
    double fraction_at(double device_x) const;
    void draw_video_controls(cairo_t* cr, int scale);
    const Image* presented_ = nullptr;  // the image whose first paint has been reported
    const Image* present_image_ = nullptr;
    // Outgoing slide during a cross-fade: its own pre-scaled surface and motion.
    struct Outgoing
    {
        cairo_surface_t* surface = nullptr;  // pre-scaled at `scale_zoom`
        double scale_zoom = 1;
        Motion motion;
        double zoom = 1, pan_x = 0, pan_y = 0;
        int width = 0, height = 0;
        gint64 fade_start_us = 0, fade_us = 0;
        guint tick = 0;
    } outgoing_;
    int pending_crossfade_ms_ = 0;
    bool pending_live_ = false;
    std::shared_ptr<Image> outgoing_live_;  // drawn from its current pixels each frame
    void drop_outgoing();
    void outgoing_transform(gint64 now_us, double& zoom, double& px, double& py) const;
    bool busy_ = false;
    guint busy_tick_ = 0;
    void motion_frame(gint64 now_us);
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
