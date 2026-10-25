#pragma once

#include "compare.h"
#include "controls.h"
#include "image.h"

#include <rapport/rapport.h>
#ifdef HAVE_GPU
#include "gpu_view.h"
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct Config;

// The image panel on an rap::Canvas. Everything else in the window follows
// the UI scale; this view renders device pixels 1:1, so 100% is one image
// pixel per screen pixel whatever the desktop scaling is. Geometry is kept
// in device pixels and converted to logical ones only when drawing.
class View
{
public:
    View(rap::ui& ui, const Config& cfg);
    ~View();
    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // The canvas node for the window layout, and attaching to the created widget.
    rap::node node();
    void attach(rap::canvas* canvas);
    rap::canvas* canvas() const { return canvas_; }

    void set_image(std::shared_ptr<Image> image);  // resets zoom to fit
    const Image* image() const { return image_.get(); }
    void set_fullscreen(bool on);
    void config_changed();
    void image_changed();  // the image's pixels (and maybe its size) changed in place
    void retoned();        // only its 8-bit pixels did, for a new exposure: its own data is the same
    // Shows `preview` (the same size: the image as it would save) in its
    // place, keeping zoom and pan; end_preview() puts the image back.
    void show_preview(std::shared_ptr<Image> preview);
    void end_preview();
    // Comparing A with B: one shown at a time, a swipe between them, or where
    // they differ. A becomes the view's image (zoom, pan and selection follow
    // it); B and the difference are drawn in its place, B scaled to A's width
    // when the sizes differ. `difference` (from compare_images, same sizes
    // only) may come later.
    enum class Compare
    {
        A,
        B,
        Swipe,
        Difference
    };
    void begin_compare(std::shared_ptr<Image> a, std::shared_ptr<Image> b, std::string a_label, std::string b_label,
                       std::shared_ptr<const ImageDifference> difference, Compare mode);
    void set_compare_b(std::shared_ptr<Image> b, std::shared_ptr<const ImageDifference> difference);
    void set_compare(Compare mode);
    void set_compare_gain(int gain);
    void swap_compare();  // A and B change places
    void end_compare();
    bool comparing() const { return bool(compare_); }
    Compare compare_mode() const { return compare_ ? compare_->mode : Compare::A; }
    const ImageDifference* compare_difference() const { return compare_ ? compare_->difference.get() : nullptr; }
    std::shared_ptr<Image> difference_shown();  // the difference as an image, at the current gain
    std::shared_ptr<Image> compare_image(bool b) const { return compare_ ? (b ? compare_->b.image : compare_->a.image) : nullptr; }
    std::string compare_label(bool b) const { return compare_ ? (b ? compare_->b_label : compare_->a_label) : std::string(); }
    // A name in a tag at the image's top-left (a side-by-side pane's image).
    void set_label(std::string label);
    void set_divided(bool on) { divided_ = on; }  // a line down the left edge, from a view beside it

    // What part of the image shows, independent of its size: for keeping a
    // second view (side by side) on the same place.
    struct Framing
    {
        bool fit = true;
        double width = 0;          // the image's shown width, device px
        double cx = 0.5, cy = 0.5;  // the view's centre, as a fraction across the image
        bool operator==(const Framing&) const = default;
    };
    Framing framing() const;
    void set_framing(const Framing& f);  // doesn't fire on_framing_changed
    std::function<void()> on_framing_changed;
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
    // The zoom that is 1:1: one image pixel to a device pixel, or for an
    // image that renders at any size (a PDF page), its actual size.
    double natural_zoom() const;
    void zoom_step(int direction);  // around the view centre

    // Slideshow motion between two framings (centres in image coordinates).
    void begin_motion(double zoom0, double zoom1, double cx0, double cy0, double cx1, double cy1, int duration_ms, int64_t start_us = 0);
    void end_motion();
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

    double scale_factor() const;  // device pixels per logical one
    void view_size(int& w, int& h) const;  // device pixels

    bool selection(int& x, int& y, int& w, int& h) const;
    // Pointer input as if from the canvas (IFRIT_SCRIPT's press/move/release).
    bool feed_pointer(const rap::pointer_event& e) { return pointer(e); }
    void clear_selection();

    std::function<void()> on_zoom_changed;
    std::function<void()> on_presented;
    std::function<void()> on_resized;  // after the toolkit's layout gave the view a new size

    void set_video(bool on, bool audio);
    void set_video_progress(double fraction, bool playing);
    void set_video_muted(bool muted);
    void set_video_volume(double volume);
    void poke_controls();
    void script_show_volume();  // scripts can't hover: reveal the volume line as if they did
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
    void paint(rap::painter& p, rap::size_f size);
    void draw_overlay(rap::painter& p);
    void draw_video_controls(rap::painter& p);
    rap::image* base_image();
    rap::image* scaled_image(double zoom);
    rap::image* checker_image(int w, int h);
    // A compared image, with its own uploads so switching between them is instant.
    struct Layer
    {
        Layer() = default;
        explicit Layer(std::shared_ptr<Image> im) : image(std::move(im)) {}
        std::shared_ptr<Image> image;
        std::unique_ptr<rap::image> base, scaled;
        double scaled_zoom = 0;
    };
    struct Comparison
    {
        Layer a, b, difference_layer;
        std::shared_ptr<Image> original;  // the A it began with, shown again when it ends
        std::string a_label, b_label;
        std::shared_ptr<const ImageDifference> difference;
        int difference_gain = 0;  // the gain difference_layer was made at
        int gain = 8;
        Compare mode = Compare::B;
        double swipe = 0.5;  // the divider, across the image (0..1)
        bool dragging = false;
    };
    void draw_layer(rap::painter& p, Layer& layer, float alpha, double rel = 1.0);  // rel: layer px to A px
    void paint_over(rap::painter& p, rap::color background);
    void draw_tag(rap::painter& p, const std::string& text, float x, float y, bool right_aligned);
    std::string label_;
    bool divided_ = false;
    Framing last_framing_;
    bool applying_framing_ = false;
    void draw_compare(rap::painter& p, float alpha);
    double divider_x() const;  // device px
    std::unique_ptr<Comparison> compare_;
    void drop_caches();
    void zoom_changed();
    bool pointer(const rap::pointer_event& e);
    bool scroll(const rap::scroll_event& e);
    void motion_frame(int64_t now_us);
    void drop_outgoing();
    void outgoing_transform(int64_t now_us, double& zoom, double& px, double& py) const;
    void cancel_deferred_click();

    rap::ui& ui_;
    const Config& cfg_;
    rap::canvas* canvas_ = nullptr;
    OverlaySections overlay_;
    std::shared_ptr<Image> image_;
    std::shared_ptr<Image> previewed_;  // the real image while a preview stands in for it

    // Uploaded pixels: the image itself, a downscaled copy for the current
    // zoom, and a checkerboard for transparency.
    std::unique_ptr<rap::image> base_;
    bool base_dirty_ = true;
    std::unique_ptr<rap::image> scaled_;
    double scaled_zoom_ = 0;
    std::unique_ptr<rap::image> checker_;

#ifdef HAVE_GPU
    // Drawn on the GPU (gpu_view.h) where it can be: then on_paint draws
    // only what goes over the image. `generation_` changes when the image's
    // data does; `gpu_drawn_` is what the frame shown was drawn from.
    std::unique_ptr<GpuView> gpu_;
    uint64_t generation_ = 1;
    struct GpuKey
    {
        const Image* image = nullptr;
        uint64_t generation = 0;
        int width = 0, height = 0;
        double zoom = 0, pan_x = 0, pan_y = 0, exposure = 0;
        uint32_t background = 0;
        bool checkerboard = false;
        ScaleFilter enlarge = ScaleFilter::Nearest, shrink = ScaleFilter::Nearest;
        bool measured = false;
        bool operator==(const GpuKey&) const = default;
    } gpu_drawn_;
    bool gpu_shown_ = false;
    bool gpu_wanted_ = false;          // the last paint was one the GPU would draw
    int64_t gpu_waiting_since_ = 0;    // a raw or HDR image waiting for the GPU's device, since
    rap::subscription gpu_wait_tick_;  // a repaint while it waits (for the patience to run out)
    // Draws the frame on the GPU when it's the plain case (one still image,
    // not compared, moving or fading); false when the CPU draws it.
    bool draw_on_gpu(rap::color background);
#endif

    // An image that renders at any size (Image::vector), rendered at the size
    // it's shown, made on a thread: the device-pixel rectangle of it from
    // (x, y) in image pixels, at `zoom`. Drawn over the stand-in while the
    // zoom is the same and it covers what's in view; asked for again when not.
    struct Native
    {
        std::shared_ptr<const VectorSource> of;
        double zoom = 0, x = 0, y = 0;
        int w = 0, h = 0;
        std::unique_ptr<rap::image> image;
    } native_;
    bool native_busy_ = false;  // one rendering at a time; the next paint asks again if it's stale
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // for work finishing after the view
    // The visible part of a vector image at this zoom; draws it if made,
    // else asks for it.
    void draw_native(rap::painter& p, float alpha);

    struct Motion
    {
        bool active = false;
        double z0 = 1, z1 = 1, cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
        double z_cache = 1;
        int64_t start_us = 0, duration_us = 0;
    } motion_;
    rap::subscription motion_tick_;
    double frame_ms_ = 0;
    int64_t present_since_ = 0;
    const Image* presented_ = nullptr;
    const Image* present_image_ = nullptr;

    struct Outgoing
    {
        std::unique_ptr<rap::image> image;  // pre-scaled at `scale_zoom`
        double scale_zoom = 1;
        Motion motion;
        double zoom = 1, pan_x = 0, pan_y = 0;
        int width = 0, height = 0;
        bool has_alpha = false;
        int64_t fade_start_us = 0, fade_us = 0;
    } outgoing_;
    rap::subscription outgoing_tick_;
    int pending_crossfade_ms_ = 0;
    bool pending_live_ = false;
    std::shared_ptr<Image> outgoing_live_;
    std::unique_ptr<rap::image> outgoing_live_image_;

    bool busy_ = false;
    rap::subscription busy_tick_;

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
    rap::subscription controls_timer_;
    bool scrubbing_ = false;
    struct ControlsGeometry
    {
        double glyph_x, line_x0, line_x1, speaker_x, y, radius;
        double volume_top, volume_bottom;
    };
    ControlsGeometry controls_geometry() const;
    bool controls_visible() const;
    double fraction_at(double device_x) const;

    rap::cursor cursor_ = rap::cursor::default_;
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
    // A drag on an existing selection adjusts it: an edge, a corner (from
    // the opposite one, which becomes the anchor), or the whole of it.
    enum class Grab
    {
        None,
        Move,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };
    Grab grab_ = Grab::None;
    double grab_dx_ = 0, grab_dy_ = 0;  // Move: the pointer against the selection's corner, image px
    Grab grab_at(double x, double y) const;  // logical px
    void adjust_selection(double x, double y);
    unsigned drag_modifiers_ = 0;  // rap::mod_*, from the latest pointer event
    bool has_selection_ = false;
    double sel_x0_ = 0, sel_y0_ = 0, sel_x1_ = 0, sel_y1_ = 0;
    double scroll_accum_ = 0;
    // In fullscreen a single click waits out the double-click interval when
    // the same button also has a double-click action.
    rap::subscription deferred_click_;
};
