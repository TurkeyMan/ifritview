#pragma once

#include "controls.h"
#include "image.h"

#include <anyui/anyui.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct Config;

// The image panel on an anyui::Canvas. Everything else in the window follows
// the UI scale; this view renders device pixels 1:1, so 100% is one image
// pixel per screen pixel whatever the desktop scaling is. Geometry is kept
// in device pixels and converted to logical ones only when drawing.
class View
{
public:
    View(anyui::Ui& ui, const Config& cfg);
    ~View();
    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // The canvas node for the window layout, and attaching to the created widget.
    anyui::Node node();
    void attach(anyui::Canvas* canvas);
    anyui::Canvas* canvas() const { return canvas_; }

    void set_image(std::shared_ptr<Image> image);  // resets zoom to fit
    const Image* image() const { return image_.get(); }
    void set_fullscreen(bool on);
    void config_changed();
    void image_changed();  // the image's pixels (and maybe its size) changed in place
    // Shows `preview` (the same size: the image as it would save) in its
    // place, keeping zoom and pan; end_preview() puts the image back.
    void show_preview(std::shared_ptr<Image> preview);
    void end_preview();
    using OverlaySections = std::vector<std::vector<std::string>>;
    void set_overlay(OverlaySections sections);

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

    // Slideshow motion between two framings (centres in image coordinates).
    void begin_motion(double zoom0, double zoom1, double cx0, double cy0, double cx1, double cy1, int duration_ms, int64_t start_us = 0);
    void end_motion();
    bool motion_active() const { return motion_.active; }
    void crossfade_next(int ms);
    void crossfade_live();
    void outgoing_changed() { invalidate(); }
    double frame_ms() const { return frame_ms_; }
    void set_busy(bool busy);
    void stamp_present(int64_t since_us, const Image* image)
    {
        present_since_ = since_us;
        present_image_ = image;
    }

    int scale_factor() const;
    void view_size(int& w, int& h) const;  // device pixels

    bool selection(int& x, int& y, int& w, int& h) const;
    void clear_selection();

    std::function<void()> on_zoom_changed;
    std::function<void()> on_presented;
    std::function<void()> on_resized;  // after the toolkit's layout gave the view a new size

    void set_video(bool on, bool audio);
    void set_video_progress(double fraction, bool playing);
    void set_video_muted(bool muted);
    void set_video_volume(double volume);
    void poke_controls();
    std::function<void()> on_video_toggle;
    std::function<void()> on_video_mute;
    std::function<void(double volume)> on_video_volume;
    std::function<void(double fraction, bool final)> on_video_seek;
    std::function<void()> on_selection_changed;
    std::function<void(Action)> on_action;

private:
    void invalidate();
    void layout();
    double fit_zoom() const;
    void clamp_pan();
    void set_zoom(double z, double anchor_x, double anchor_y);  // anchor in device pixels
    void zoom_step_at(int direction, double x, double y);
    void zoom_to_selection();
    void update_selection(double x, double y);  // logical px
    void to_image(double x, double y, double& ix, double& iy) const;
    void update_cursor(double x, double y);
    void run_click(Action a, double x, double y);
    void begin_drag(Action a, double x, double y);
    void drag_to(Action a, double x, double y);
    void run_wheel(Action a, int direction, double x, double y);
    void paint(anyui::Painter& p, anyui::SizeF size);
    void draw_overlay(anyui::Painter& p);
    void draw_video_controls(anyui::Painter& p);
    anyui::Image* base_image();
    anyui::Image* scaled_image(double zoom);
    anyui::Image* checker_image(int w, int h);
    void drop_caches();
    void zoom_changed();
    bool pointer(const anyui::PointerEvent& e);
    bool scroll(const anyui::ScrollEvent& e);
    void motion_frame(int64_t now_us);
    void drop_outgoing();
    void outgoing_transform(int64_t now_us, double& zoom, double& px, double& py) const;
    void cancel_deferred_click();

    anyui::Ui& ui_;
    const Config& cfg_;
    anyui::Canvas* canvas_ = nullptr;
    OverlaySections overlay_;
    std::shared_ptr<Image> image_;
    std::shared_ptr<Image> previewed_;  // the real image while a preview stands in for it

    // Uploaded pixels: the image itself, a downscaled copy for the current
    // zoom, and a checkerboard for transparency.
    std::unique_ptr<anyui::Image> base_;
    bool base_dirty_ = true;
    std::unique_ptr<anyui::Image> scaled_;
    double scaled_zoom_ = 0;
    std::unique_ptr<anyui::Image> checker_;

    struct Motion
    {
        bool active = false;
        double z0 = 1, z1 = 1, cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
        double z_cache = 1;
        int64_t start_us = 0, duration_us = 0;
    } motion_;
    anyui::Subscription motion_tick_;
    double frame_ms_ = 0;
    int64_t present_since_ = 0;
    const Image* presented_ = nullptr;
    const Image* present_image_ = nullptr;

    struct Outgoing
    {
        std::unique_ptr<anyui::Image> image;  // pre-scaled at `scale_zoom`
        double scale_zoom = 1;
        Motion motion;
        double zoom = 1, pan_x = 0, pan_y = 0;
        int width = 0, height = 0;
        bool has_alpha = false;
        int64_t fade_start_us = 0, fade_us = 0;
    } outgoing_;
    anyui::Subscription outgoing_tick_;
    int pending_crossfade_ms_ = 0;
    bool pending_live_ = false;
    std::shared_ptr<Image> outgoing_live_;
    std::unique_ptr<anyui::Image> outgoing_live_image_;

    bool busy_ = false;
    anyui::Subscription busy_tick_;

    // Video controls.
    bool video_ = false;
    double video_fraction_ = 0;
    bool video_playing_ = false;
    bool video_muted_ = true;
    bool video_audio_ = false;
    double video_volume_ = 0.7;
    bool volume_dragging_ = false;
    bool volume_hover_ = false;
    void update_volume_hover(double device_x, double device_y);
    int64_t controls_until_ = 0;
    anyui::Subscription controls_timer_;
    bool scrubbing_ = false;
    struct ControlsGeometry
    {
        double glyph_x, line_x0, line_x1, speaker_x, y, radius;
        double volume_top, volume_bottom;
    };
    ControlsGeometry controls_geometry() const;
    bool controls_visible() const;
    double fraction_at(double device_x) const;

    anyui::Cursor cursor_ = anyui::Cursor::Default;
    ZoomMode mode_ = ZoomMode::Fit;
    double zoom_ = 1.0;
    double pan_x_ = 0, pan_y_ = 0;  // image origin in device pixels
    bool fullscreen_ = false;
    int pressed_button_ = 0;
    bool drag_active_ = false;
    bool suppress_click_ = false;
    double press_x_ = 0, press_y_ = 0;  // logical px
    double drag_x_ = 0, drag_y_ = 0;
    double sel_anchor_x_ = 0, sel_anchor_y_ = 0;
    bool has_selection_ = false;
    double sel_x0_ = 0, sel_y0_ = 0, sel_x1_ = 0, sel_y1_ = 0;
    double scroll_accum_ = 0;
    // In fullscreen a single click waits out the double-click interval when
    // the same button also has a double-click action.
    anyui::Subscription deferred_click_;
};
