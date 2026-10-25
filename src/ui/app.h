#pragma once

#include "cache.h"
#include "config.h"
#include "folder.h"
#include "geocode.h"
#include "image.h"
#include "loader.h"
#include "metadata_model.h"
#include "save.h"
#include "video.h"
#include "view.h"

#include <rapport/rapport.h>
#include <rapport/sys.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// IfritView on rapport, with the toolkit picked at runtime (GTK or Qt on
// Linux, Win32 on Windows).
class App
{
public:
    // `settings`: from read_settings, started as early as can be.
    App(rap::ui& ui, std::shared_future<Config> settings);
    // The settings, read on a thread of their own, which also sets the
    // decoders' options from them: the first image waits for that, not the UI.
    static std::shared_future<Config> read_settings();
    ~App();
    // The file (and its folder) or folder to show. `first`, when given, is
    // the file already being decoded (from main), to open the window with.
    void open_path(const std::string& path, class FirstImage* first = nullptr);
    void show_window();  // after the first image is in, if it's quick
    // Two images compared, standalone (--diff, or two files dropped at once).
    // `a_label`/`b_label` name the sides instead of their files (a diff tool
    // passing temporary copies: "file.png (HEAD)").
    void open_diff(const std::string& a_path, const std::string& b_path, const std::string& a_label = {},
                   const std::string& b_label = {});
    int run();
    rap::ui& ui() { return ui_; }
    rap::window& window() { return *window_; }
    const View& view() const { return *view_; }

    // Actions, bound to the toolbar and keys (and the test script).
    void navigate(int delta);
    void image_presented();
    void go_first();
    void go_last();
    void reload();
    void on_dropped(const std::vector<std::string>& paths);  // files or folders dropped on the window
    void set_recursive(bool on, const std::string& root = {});  // Config::recursive: the toolbar's toggle, a folder dropped
    void toggle_fullscreen();
    void zoom_in();
    void zoom_out();
    void zoom_original();
    void zoom_fit();
    void open_dialog();
    void open_settings() { open_settings_page(0); }
    void open_settings_page(int page);
    void quit();
    void crop_to_selection();
    void save_as();
    void step_page(int delta);   // PgUp/PgDn: within the file, stopping at its ends
    void step_level(int delta);  // Shift+PgUp/PgDn: the mip levels
    void print_image();  // app_print.cpp
    void preview_save(const Encoder& e, const EncodeOptions& o);
    void preview_lossless();
    void leave_image();
    void watch_folder();                            // the folder shown, for changes to its files
    void folder_changed(const rap::sys::watch_batch& batch);
    void choose_save_path(const Encoder& e, const EncodeOptions& o, std::shared_ptr<Image> source, std::shared_ptr<const struct LosslessJpeg> lossless);
    void plan_lossless();
    void toggle_metadata();
    // G: the histogram, docked where the info goes (one or the other), or
    // over the image in fullscreen.
    void toggle_histogram();
    void toggle_xmp();
    void copy_to_clipboard();
    void transform(int exif_orientation);
    void toggle_animation();
    void toggle_slideshow();
    void set_exposure(double ev);
    void script_toggle_video() { toggle_video(); }
    void script_poke_controls() { view_->poke_controls(); }
    void script_show_volume() { view_->script_show_volume(); }
    void script_toggle_xmp();
    void remember_pane_width();
    void script_end_dialog(bool accept);
    void script_compare(const std::string& how);
    void script_export(const std::string& path);
    void script_instead(const std::string& what);
    void script_pointer(const rap::pointer_event& e) { view_->feed_pointer(e); }
    void script_toggle_lossless() { if (save_dialog_) save_dialog_->toggle_lossless(); }  // a, b, swipe, diff, swap, side or gain:N

private:
    void plugins_loaded();  // their extensions join the list; the folder is listed again
    void build_ui();
    rap::node tool_button(const char* icon, const char* tooltip, void (App::*action)());
    rap::node compare_button(rap::icon icon, const char* tooltip, View::Compare mode);
    void set_compare(View::Compare mode);
    void swap_compare();
    void show_save_preview(bool on);
    void end_comparing();
    void set_side_by_side(bool on);
    void show_side();  // B, its name and the main view's framing, in the side pane
    void settle_side_by_side();
    // The toolbar for what the view is doing: browsing the folder, or
    // comparing two images (and no browsing while Save As is open).
    void update_toolbar();
    // A raw, or an HDR image: the exposure control is shown for it.
    bool exposure_applies() const { return image_ && (image_->is_raw || image_->hdr()); }
    bool navigation_locked() const { return save_dialog_ || view_->comparing(); }
    bool on_key(const rap::key_event& e);

    // The folder's current file, or `file` (paging: the file on screen,
    // whatever the folder's listing says while it's still being read).
    void load_current(const std::string& file = {});
    void on_loaded(Loader::Result& r);
    void sync_exposure_slider();
    void request_retone();
    bool retone_busy_ = false;
    int64_t nav_stamp_us_ = 0;
    // The last second or so of steps through the folder (when, which way):
    // how fast and which way the user is going, for the prefetch.
    struct NavStep
    {
        int64_t us;
        int direction;
    };
    std::deque<NavStep> nav_steps_;
    double exposure_range_[2] = {-3, 3};  // the slider's, as last set
    bool flicking(int* direction = nullptr) const;
    bool awaiting_present_ = false;
    int pending_steps_ = 0;
    void fit_window_to_image();
    void update_status();
    void update_title();
    void on_fullscreen_changed(bool fs);
    void retone();
    void apply_xmp(bool fresh);
    bool apply_xmp_geometry(Image& image);
    void ensure_developed();
    void schedule_prefetch();
    bool cached_in_full(const std::string& key);
    void request_place(const std::shared_ptr<Image>& image);
    void refresh_metadata();
    std::vector<MetadataGroup> collect_metadata() const;
    View::OverlaySections overlay_sections() const;
    void open_url(const std::string& url);

    rap::ui& ui_;
    Config cfg_;
    Folder folder_;
    std::unique_ptr<View> view_;
    MetadataModel metadata_model_;
    VideoPlayer video_;  // after view_: stops (and its frame callbacks) before the view goes
    rap::subscription video_timer_;
    bool volume_dirty_ = false;
    VideoPlayer outgoing_video_;
    rap::subscription audio_fade_timer_;
    int64_t audio_fade_start_ = 0;
    int audio_fade_ms_ = 0;
    void start_audio_fade(int ms);
    bool scrubbing_ = false;
    bool resume_after_scrub_ = false;
    void play_video();
    void toggle_video();
    void toggle_mute();
    void stop_video();
    Loader loader_;
    ImageCache cache_;
    Geocoder geocoder_;

    std::unique_ptr<rap::window> window_;
    rap::widget* toolbar_ = nullptr;
    rap::widget* footer_ = nullptr;
    rap::widget* exposure_box_ = nullptr;
    rap::slider* exposure_ = nullptr;
    rap::widget* wb_box_ = nullptr;
    rap::slider *wb_kelvin_ = nullptr, *wb_tint_ = nullptr;
    bool syncing_wb_ = false;
    void white_balance_changed();  // the sliders moved
    void white_balance_as_shot();  // back to the camera's
    // The view: the raw as developed here, or instead (the G panel's checks,
    // for comparing) the camera's own rendering, the JPEG embedded in it, or
    // LibRaw's develop on the CPU.
    enum class Instead
    {
        Nothing,
        CameraJpeg,
        LibRaw,
    };
    void show_instead(Instead what);  // and keeps showing it while browsing
    void show_instead();              // the image shown changed
    rap::check* camera_jpeg_check_ = nullptr;
    rap::check* libraw_check_ = nullptr;
    Instead instead_ = Instead::Nothing;
    std::shared_ptr<Image> instead_image_;  // the shown raw's, while it's shown instead
    uint64_t instead_request_ = 0;
    bool syncing_instead_ = false;  // its checks set here, not by the user
    void sync_white_balance();     // the sliders to the image's
    rap::tool_button* smooth_toggle_ = nullptr;
    // The image's 8-bit pixels at its exposure, made again if the GPU's drawing
    // left them behind (Image::pixels_stale): before saving, printing,
    // copying, comparing, or the CPU drawing it.
    void pixels_for_cpu();
    ScaleFilter smooth_enlarge_ = ScaleFilter::Lanczos3;  // what the toolbar toggle turns enlarging back on with
    rap::tool_button* xmp_toggle_ = nullptr;
    rap::tool_button* recursive_toggle_ = nullptr;
    std::function<void(bool)> end_settings_;  // while Settings is open
    rap::tool_button* crop_button_ = nullptr;
    std::array<rap::widget*, 7> browse_widgets_{};  // hidden while comparing
    std::array<rap::widget*, 4> compare_widgets_{};  // by View::Compare; shown only while comparing
    std::array<rap::tool_button*, 4> compare_buttons_{};
    std::array<rap::widget*, 3> diff_widgets_{};  // swap, side by side, gain
    rap::slider* gain_ = nullptr;
    // Side by side: B in a second view to the right, framed as the first.
    std::unique_ptr<View> side_view_;
    rap::widget* side_pane_ = nullptr;
    rap::tool_button* side_button_ = nullptr;
    bool side_by_side_ = false;
    float side_restore_w_ = 0;  // the window's width before side by side opened
    struct SideSettle
    {
        int view_w;      // device px: the image area's width before, each pane's after
        float window_w;  // the width asked for
    };
    std::optional<SideSettle> side_settle_;
    bool syncing_framing_ = false;
    bool diff_session_ = false;  // comparing two files, not previewing a save
    std::string diff_name_;      // "a ↔ b"
    std::shared_ptr<Image> save_source_;                        // what Save As saves: the image, or the difference shown
    std::shared_ptr<const struct LosslessJpeg> lossless_plan_;  // for JPEG's Lossless, once worked out
    uint64_t lossless_generation_ = 0;                          // which working-out of it is the latest
    std::unique_ptr<rap::sys::directory_watch> watch_;
    std::string watched_dir_;          // what watch_ watches
    bool watched_recursive_ = false;   // and whether the folders inside it too
    std::array<int, 2> video_shape_{};  // the video frame last fitted to
    rap::label* status_left_ = nullptr;
    rap::label* status_center_ = nullptr;
    rap::label* status_right_ = nullptr;
    rap::button* status_location_ = nullptr;
    rap::splitter* splitter_ = nullptr;
    rap::widget* metadata_pane_ = nullptr;
    rap::list* metadata_list_ = nullptr;
    // The pane holds the info or the histogram (and what's measured), not both.
    rap::widget *info_box_ = nullptr, *histogram_box_ = nullptr;
    rap::canvas* histogram_canvas_ = nullptr;
    rap::label *hist_peak_ = nullptr, *hist_max_ = nullptr, *hist_min_ = nullptr, *hist_average_ = nullptr, *hist_clipped_ = nullptr;
    bool pane_histogram_ = false;
    void toggle_pane();          // the pane in or out, with the window following
    void show_pane_content();    // the info or the histogram in it
    void update_histogram();     // where the view draws its histogram
    // The histogram's legend: R, G, B, L chips in its corner, each switching
    // its channel (a hidden one's dim).
    void paint_histogram_legend(rap::painter& p, rap::size_f size);
    bool histogram_legend_click(const rap::pointer_event& e);
    bool metadata_visible_ = false;
    bool pane_before_fullscreen_ = false;
    std::string location_url_;

    bool fullscreen_ = false;
    bool slideshow_ = false;
    rap::subscription slideshow_timer_;
    std::vector<int> slideshow_order_;
    size_t slideshow_pos_ = 0;
    struct Slide
    {
        bool active = false;
        double z0 = 1, z1 = 1;
        double fx0 = 0.5, fy0 = 0.5, fx1 = 0.5, fy1 = 0.5;
        int64_t start_us = 0;
        int duration_ms = 0;
    } slide_;
    void apply_slide_motion();
    void slideshow_start_slide();
    void slideshow_advance();
    void stop_slideshow();
    rap::subscription animation_timer_;
    size_t animation_frame_ = 0;
    bool animation_playing_ = false;
    void start_animation();
    void stop_animation();
    void show_animation_frame(size_t index);
    void schedule_animation_frame();
    uint64_t load_generation_ = 0;
    std::shared_ptr<Image> image_;
    std::string image_path_;  // the file shown
    // Which of its pages and mip levels: the key it's loaded and cached by
    // (part_key), and the part asked for (`part_`, of the file `part_file_`;
    // another file starts at its first).
    std::string image_key_;
    ImagePart part_;
    std::string part_file_;
    std::string current_key() const { return part_key(folder_.current(), part_); }
    uint64_t file_size_ = 0;
    std::string status_message_;
    // A status saying work is under way (loading, developing, saving): the
    // view shows it's busy while the status is still this message.
    std::string busy_message_;
    rap::subscription refit_timer_;
    std::shared_ptr<SaveDialog> save_dialog_;
    std::shared_ptr<Image> preview_image_;  // the latest preview, compared while its toggle is on
    std::shared_ptr<const ImageDifference> preview_difference_;  // between image_ and preview_image_
    std::map<std::string, EncodeOptions> save_options_;  // per format, between saves
    // Shared with the preview workers, which may outlive a request: the
    // latest generation wins, and the app waits for running ones on exit.
    struct PreviewState
    {
        std::atomic<uint64_t> generation{0};
        std::atomic<int> running{0};
    };
    std::shared_ptr<PreviewState> preview_ = std::make_shared<PreviewState>();
    bool refit_after_layout_ = false;
    bool refit_when_laid_out_ = false;  // a fit guessed the chrome: fit again once laid out
    double fitted_scale_ = 0;           // the device scale the window was last fitted at
    bool first_presented_ = false;
    std::string deferred_scan_;  // the folder of the first image, listed once it's painted
    void take_first_image(class FirstImage& first, const std::string& file, uint64_t generation);
    void refit_after_pane_toggle();
    float pane_extra_ = 0;         // what the info pane adds to the window's width (learnt)
    float pane_toggled_from_ = 0;  // the width before the last toggle
};
