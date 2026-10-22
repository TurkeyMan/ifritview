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

#include <anyui/anyui.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// IfritView on anyui: the same app as the GTK front end, with the toolkit
// picked at runtime (GTK 3 here, Win32 on Windows).
class App
{
public:
    explicit App(anyui::Ui& ui);
    ~App();
    void open_path(const std::string& path);
    int run();
    anyui::Ui& ui() { return ui_; }
    anyui::Window& window() { return *window_; }
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
    void choose_save_path(const Encoder& e, const EncodeOptions& o);
    void toggle_metadata();
    void toggle_xmp();
    void copy_to_clipboard();
    void transform(int exif_orientation);
    void toggle_animation();
    void toggle_slideshow();
    void set_exposure(double ev);
    void script_toggle_video() { toggle_video(); }
    void script_poke_controls() { view_->poke_controls(); }

private:
    void build_ui();
    anyui::Node tool_button(const char* icon, const char* tooltip, void (App::*action)());
    bool on_key(const anyui::KeyEvent& e);

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

    anyui::Ui& ui_;
    Config cfg_;
    Folder folder_;
    std::unique_ptr<View> view_;
    MetadataModel metadata_model_;
    VideoPlayer video_;  // after view_: stops (and its frame callbacks) before the view goes
    anyui::Subscription video_timer_;
    bool volume_dirty_ = false;
    VideoPlayer outgoing_video_;
    anyui::Subscription audio_fade_timer_;
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

    std::unique_ptr<anyui::Window> window_;
    anyui::Widget* toolbar_ = nullptr;
    anyui::Widget* footer_ = nullptr;
    anyui::Widget* exposure_box_ = nullptr;
    anyui::Slider* exposure_ = nullptr;
    anyui::ToolButton* smooth_toggle_ = nullptr;
    anyui::ToolButton* xmp_toggle_ = nullptr;
    anyui::ToolButton* crop_button_ = nullptr;
    anyui::Label* status_left_ = nullptr;
    anyui::Label* status_center_ = nullptr;
    anyui::Label* status_right_ = nullptr;
    anyui::Button* status_location_ = nullptr;
    anyui::Splitter* splitter_ = nullptr;
    anyui::Widget* metadata_pane_ = nullptr;
    anyui::List* metadata_list_ = nullptr;
    bool metadata_visible_ = false;
    bool pane_before_fullscreen_ = false;
    std::string location_url_;

    bool fullscreen_ = false;
    bool slideshow_ = false;
    anyui::Subscription slideshow_timer_;
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
    anyui::Subscription animation_timer_;
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
    anyui::Subscription refit_timer_;
    std::shared_ptr<SaveDialog> save_dialog_;
    std::shared_ptr<Image> preview_image_;  // the latest preview, shown while its toggle is on
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
    void refit_after_pane_toggle();
    float pane_extra_ = 0;         // what the info pane adds to the window's width (learnt)
    float pane_toggled_from_ = 0;  // the width before the last toggle
};
