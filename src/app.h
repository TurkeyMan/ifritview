#pragma once

#include "config.h"
#include "folder.h"
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
    };
    using Callback = std::function<void(Result&)>;   // runs on the GTK main thread

    explicit Loader(Callback callback);
    ~Loader();
    void request(uint64_t generation, const std::string& path);

private:
    void run();

    Callback callback_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<std::pair<uint64_t, std::string>> pending_;
    bool stop_ = false;
    std::thread thread_;
};

class App {
public:
    App();
    void open_path(const std::string& path);
    void run();
    GtkWidget* window() const { return window_; }

private:
    void build_ui();
    GtkWidget* tool_button(const char* icon, const char* tooltip, void (App::*action)());

    void load_current();
    void navigate(int delta);
    void go_first();
    void go_last();
    void go_prev_action();
    void go_next_action();
    void reload();
    void on_loaded(Loader::Result& r);
    void fit_window_to_image();
    void toggle_fullscreen();
    void zoom_in();
    void zoom_out();
    void zoom_original();
    void zoom_fit();
    void open_dialog();
    void open_settings();
    void quit();
    void update_status();
    void update_title();
    gboolean on_key(GdkEventKey* e);
    void on_window_state(GdkEventWindowState* e);
    void on_drop(GtkSelectionData* data, guint time, GdkDragContext* ctx);

    Config cfg_;
    Folder folder_;
    std::unique_ptr<ImageView> view_;
    Loader loader_;

    GtkWidget* window_ = nullptr;
    GtkWidget* toolbar_ = nullptr;
    GtkWidget* footer_ = nullptr;
    GtkWidget* status_left_ = nullptr;
    GtkWidget* status_center_ = nullptr;
    GtkWidget* status_right_ = nullptr;

    bool fullscreen_ = false;
    uint64_t load_generation_ = 0;
    std::shared_ptr<Image> image_;
    std::string image_path_;
    uint64_t file_size_ = 0;
    std::string status_message_;    // transient: "Loading…", errors
};
