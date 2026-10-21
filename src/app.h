#pragma once

#include "cache.h"
#include "config.h"
#include "folder.h"
#include "geocode.h"
#include "image.h"
#include "metadata.h"
#include "video.h"
#include "viewer.h"

#include <gtk/gtk.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Decodes on a worker thread. A newer request replaces a pending one, so
// scrolling quickly through a folder only decodes what ends up on screen.
// Decoding off the main thread, with the image being navigated to always
// first. A dedicated foreground worker handles only that image (its preview,
// or its full decode when there is none) and never waits behind anything.
// A pool of background workers, at lower OS priority, handles the develop of
// the image on screen and then prefetches of its neighbours, nearest first.
class Loader
{
public:
    struct Result
    {
        uint64_t generation;
        std::string path;
        std::shared_ptr<Image> image;  // null on failure
        std::string error;
        bool preview = false;                  // a quick stand-in; the real decode follows
        bool prefetch = false;                 // decoded ahead of time; cache it, don't show it
        bool cached = false;                   // served from the cache: pixels already carry any edits
        gint64 picked_us = 0, decoded_us = 0;  // trace: loader timeline
    };
    using Callback = std::function<void(Result&)>;  // runs on the GTK main thread

    explicit Loader(Callback callback);
    ~Loader();
    // The image to show now; a preview, if it has one, is followed by its develop.
    void request(uint64_t generation, const std::string& path);
    // Only the develop: the preview is already on screen (from the cache).
    void request_develop(uint64_t generation, const std::string& path);
    // Files to decode when idle, nearest first; replaces the previous list.
    void prefetch(std::vector<std::string> paths);

private:
    void run_foreground();
    void run_background();
    enum class Lane
    {
        Foreground,
        Develop,
        Prefetch
    };
    void post(Result* r, Lane lane);
    void decode_into(Result& r);

    Callback callback_;
    std::mutex mutex_;
    std::condition_variable fg_wake_, bg_wake_;
    struct Job
    {
        uint64_t generation;
        std::string path;
    };
    std::optional<Job> pending_;  // foreground
    std::optional<Job> develop_;  // background, ahead of prefetches
    std::vector<std::string> prefetch_;
    std::vector<std::string> in_flight_;  // background paths being decoded right now
    bool stop_ = false;
    std::thread foreground_;
    std::vector<std::thread> background_;
};

class App
{
public:
    App();
    void open_path(const std::string& path);
    void run();
    GtkWidget* window() const { return window_; }
    const ImageView& view() const { return *view_; }

    // Actions, bound to the toolbar and keys.
    void navigate(int delta);
    void image_presented();
    void go_first();
    void go_last();
    void reload();
    void toggle_fullscreen();
    void zoom_in();
    void zoom_out();
    void zoom_original();
    void zoom_fit();
    void open_dialog();
    void open_settings(const char* page = nullptr);
    void open_settings_action() { open_settings(); }
    void quit();
    void crop_to_selection();
    void save_as();
    void toggle_metadata();
    void toggle_xmp() { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(xmp_toggle_), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(xmp_toggle_))); }
    void copy_to_clipboard();
    void transform(int exif_orientation);  // rotate/mirror the displayed image
    void toggle_animation();
    void toggle_slideshow();
    void set_exposure(double ev);
    void script_toggle_video() { toggle_video(); }
    void script_poke_controls() { poke_video_controls(); }

private:
    void build_ui();
    GtkWidget* tool_button(const char* icon, const char* tooltip, void (App::*action)());

    void load_current();
    void go_prev_action();
    void go_next_action();
    void on_loaded(Loader::Result& r);
    void sync_exposure_slider();  // range, tick, value and sensitivity from the current image
    // Exposure renders run on a thread; the slider moves freely meanwhile and a
    // render that finishes behind the slider triggers another.
    void request_retone();
    bool retone_busy_ = false;
    gint64 nav_stamp_us_ = 0;
    // Every image is shown: a step that arrives before the current image has
    // been painted waits for it (capped, so a big flick doesn't run on).
    bool awaiting_present_ = false;
    int pending_steps_ = 0;  // trace: when the current navigation was triggered
    void fit_window_to_image();
    void place_window(int w, int h, const GdkRectangle& work);
    void update_status();
    void update_title();
    gboolean on_key(GdkEventKey* e);
    void on_window_state(GdkEventWindowState* e);
    void on_drop(GtkSelectionData* data, guint time, GdkDragContext* ctx);
    void on_exposure_changed();
    void retone();                          // re-render the raw from both sliders
    void apply_xmp(bool fresh);             // exposure always; geometry only for a fresh decode
    bool apply_xmp_geometry(Image& image);  // crop + orientation; true if anything changed
    void ensure_developed();                // kick off the real decode behind a preview
    void schedule_prefetch();
    void request_place(const std::shared_ptr<Image>& image);  // async reverse geocode, result pops into the UI
    void on_smooth_toggled();
    void on_xmp_toggled();
    void refresh_metadata();
    std::vector<MetadataGroup> collect_metadata() const;
    ImageView::OverlaySections overlay_sections() const;

    Config cfg_;
    Folder folder_;
    std::unique_ptr<ImageView> view_;
    std::unique_ptr<MetadataPane> metadata_;
    VideoPlayer video_;  // after view_: stops (and its frame callbacks) before the view goes
    guint video_timer_ = 0;
    void play_video();
    void toggle_video();
    void toggle_mute();
    const ImageView& view_ref() const { return *view_; }
    void poke_video_controls() { view_->poke_controls(); }
    void stop_video();
    Loader loader_;
    ImageCache cache_;
    Geocoder geocoder_;

    GtkWidget* window_ = nullptr;
    GtkWidget* toolbar_ = nullptr;
    GtkWidget* footer_ = nullptr;
    GtkWidget* paned_ = nullptr;  // image | metadata pane
    bool paned_adjusting_ = false;
    bool paned_dragging_ = false;
    bool pane_before_fullscreen_ = false;  // restore the info pane on leaving fullscreen        // the user has hold of the divider
    GtkWidget* exposure_box_ = nullptr;    // EV slider, shown for raw files only
    GtkWidget* exposure_ = nullptr;
    GtkWidget* smooth_toggle_ = nullptr;
    GtkWidget* crop_button_ = nullptr;
    GtkWidget* status_left_ = nullptr;
    GtkWidget* spinner_ = nullptr;  // turns while a decode is in flight
    GtkWidget* status_center_ = nullptr;
    GtkWidget* status_right_ = nullptr;
    GtkWidget* status_location_ = nullptr;  // GPS link, hidden when absent

    bool fullscreen_ = false;
    bool slideshow_pending_start_ = false;  // first slide starts once the fullscreen allocation has arrived
    bool chrome_pending_ = false;           // toolbar/footer visibility to apply on the next window allocation
    bool slideshow_ = false;
    guint slideshow_timer_ = 0;
    std::vector<int> slideshow_order_;  // shuffled indices when shuffling
    size_t slideshow_pos_ = 0;
    struct Slide  // the current slide's motion in image-relative terms, so a developed image can take over mid-way
    {
        bool active = false;
        double z0 = 1, z1 = 1;                              // zoom relative to fit
        double fx0 = 0.5, fy0 = 0.5, fx1 = 0.5, fy1 = 0.5;  // centre as fractions of the image
        gint64 start_us = 0;
        int duration_ms = 0;
    } slide_;
    void apply_slide_motion();
    void slideshow_start_slide();
    void slideshow_advance();
    void stop_slideshow();
    guint animation_timer_ = 0;  // next-frame timeout while an animation plays
    size_t animation_frame_ = 0;
    bool animation_playing_ = false;
    void start_animation();
    void stop_animation();
    void show_animation_frame(size_t index);
    static GSourceFunc animation_timer_callback();
    uint64_t load_generation_ = 0;
    GtkWidget* xmp_toggle_ = nullptr;
    std::shared_ptr<Image> image_;
    std::string image_path_;
    uint64_t file_size_ = 0;
    std::string status_message_;  // transient: "Loading…", errors
};
