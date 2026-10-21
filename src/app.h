#pragma once

#include "config.h"
#include "folder.h"
#include "cache.h"
#include "metadata.h"
#include "image.h"
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
class Loader {
public:
    struct Result {
        uint64_t generation;
        std::string path;
        std::shared_ptr<Image> image;   // null on failure
        std::string error;
        bool preview = false;           // a quick stand-in; the real decode follows
        bool prefetch = false;          // decoded ahead of time; cache it, don't show it
    };
    using Callback = std::function<void(Result&)>;   // runs on the GTK main thread

    explicit Loader(Callback callback);
    ~Loader();
    // `develop` false: stop after the quick preview when one exists.
    void request(uint64_t generation, const std::string& path, bool develop);
    // Files to decode when idle, nearest first; replaces the previous list.
    void prefetch(std::vector<std::string> paths);

private:
    void run();
    bool superseded(uint64_t generation);
    bool preview_shown_ = false;   // this generation already posted its preview
    void post(Result* r);

    Callback callback_;
    std::mutex mutex_;
    std::condition_variable wake_;
    struct Job { uint64_t generation; std::string path; bool develop; };
    std::optional<Job> pending_;
    std::vector<std::string> prefetch_;
    bool stop_ = false;
    std::thread thread_;
};

class App {
public:
    App();
    void open_path(const std::string& path);
    void run();
    GtkWidget* window() const { return window_; }
    const ImageView& view() const { return *view_; }

    // Actions, bound to the toolbar and keys.
    void navigate(int delta);
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
    void copy_to_clipboard();
    void transform(int exif_orientation);   // rotate/mirror the displayed image
    void set_exposure(double ev);

private:
    void build_ui();
    GtkWidget* tool_button(const char* icon, const char* tooltip, void (App::*action)());

    void load_current();
    void go_prev_action();
    void go_next_action();
    void on_loaded(Loader::Result& r);
    void fit_window_to_image();
    void update_status();
    void update_title();
    gboolean on_key(GdkEventKey* e);
    void on_window_state(GdkEventWindowState* e);
    void on_drop(GtkSelectionData* data, guint time, GdkDragContext* ctx);
    void on_exposure_changed();
    void retone();                           // re-render the raw from both sliders
    void apply_xmp_edits();
    void ensure_developed();                 // kick off the real decode behind a preview
    void schedule_prefetch();
    void on_smooth_toggled();
    void on_xmp_toggled();
    void refresh_metadata();
    std::vector<MetadataGroup> collect_metadata() const;

    Config cfg_;
    Folder folder_;
    std::unique_ptr<ImageView> view_;
    std::unique_ptr<MetadataWindow> metadata_;
    Loader loader_;
    ImageCache cache_;
    bool image_modified_ = false;           // edited or re-toned since it was loaded

    GtkWidget* window_ = nullptr;
    GtkWidget* toolbar_ = nullptr;
    GtkWidget* footer_ = nullptr;
    GtkWidget* exposure_box_ = nullptr;   // EV slider, shown for raw files only
    GtkWidget* exposure_ = nullptr;
    GtkWidget* smooth_toggle_ = nullptr;
    GtkWidget* crop_button_ = nullptr;
    GtkWidget* status_left_ = nullptr;
    GtkWidget* status_center_ = nullptr;
    GtkWidget* status_right_ = nullptr;
    GtkWidget* status_location_ = nullptr;   // GPS link, hidden when absent

    bool fullscreen_ = false;
    uint64_t load_generation_ = 0;
    bool showing_preview_ = false;          // a raw's embedded JPEG stands in for the real pixels
    bool develop_requested_ = false;
    // Edits made while the preview is showing, replayed on the developed
    // image. Crops are fractions so they survive the size difference.
    struct Edit { int orientation; double x, y, w, h; };
    std::vector<Edit> preview_edits_;
    GtkWidget* xmp_toggle_ = nullptr;
    std::shared_ptr<Image> image_;
    std::string image_path_;
    uint64_t file_size_ = 0;
    std::string status_message_;    // transient: "Loading…", errors
};
