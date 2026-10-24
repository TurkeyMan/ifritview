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

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// IfritView on rapport, with the toolkit picked at runtime (GTK or Qt on
// Linux, Win32 on Windows).
class App
{
public:
    explicit App(rap::ui& ui);
    ~App();
    void open_path(const std::string& path);
    // Two images compared, standalone (--diff, or two files dropped at once).
    void open_diff(const std::string& a_path, const std::string& b_path);
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
    void preview_save(const Encoder& e, const EncodeOptions& o);
    void choose_save_path(const Encoder& e, const EncodeOptions& o, std::shared_ptr<Image> source, std::shared_ptr<const struct LosslessJpeg> lossless);
    void plan_lossless();
    void toggle_metadata();
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
    void script_compare(const std::string& how);  // a, b, swipe, diff, swap, side or gain:N

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

    void load_current();
    void on_loaded(Loader::Result& r);
    void sync_exposure_slider();
    void request_retone();
    bool retone_busy_ = false;
    int64_t nav_stamp_us_ = 0;
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
    rap::tool_button* smooth_toggle_ = nullptr;
    rap::tool_button* xmp_toggle_ = nullptr;
    std::function<void(bool)> end_settings_;  // while Settings is open
    rap::tool_button* crop_button_ = nullptr;
    std::array<rap::widget*, 6> browse_widgets_{};  // hidden while comparing
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
    std::shared_ptr<Image> save_source_;
    std::shared_ptr<const struct LosslessJpeg> lossless_plan_;  // for JPEG's Lossless, once worked out
    uint64_t lossless_generation_ = 0;  // what Save As saves: the image, or the difference shown
    rap::label* status_left_ = nullptr;
    rap::label* status_center_ = nullptr;
    rap::label* status_right_ = nullptr;
    rap::button* status_location_ = nullptr;
    rap::splitter* splitter_ = nullptr;
    rap::widget* metadata_pane_ = nullptr;
    rap::list* metadata_list_ = nullptr;
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
    std::string image_path_;
    uint64_t file_size_ = 0;
    std::string status_message_;
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
    bool first_presented_ = false;
    void refit_after_pane_toggle();
    float pane_extra_ = 0;         // what the info pane adds to the window's width (learnt)
    float pane_toggled_from_ = 0;  // the width before the last toggle
};
