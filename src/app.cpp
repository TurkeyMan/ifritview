#include "app.h"

#include "decoders/decoder.h"
#include "settings.h"

#include <gdk/gdkkeysyms.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

// ---------------------------------------------------------------- Loader

Loader::Loader(Callback callback) : callback_(std::move(callback)), thread_([this] { run(); }) {}

Loader::~Loader() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void Loader::request(uint64_t generation, const std::string& path) {
    {
        std::lock_guard lock(mutex_);
        pending_ = {generation, path};
    }
    wake_.notify_one();
}

void Loader::run() {
    for (;;) {
        std::pair<uint64_t, std::string> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || pending_; });
            if (stop_) return;
            job = *pending_;
            pending_.reset();
        }
        auto* r = new Result{job.first, job.second, std::make_shared<Image>(), {}};
        if (!decode_image(r->path, *r->image, r->error)) r->image.reset();
        struct Dispatch {
            Loader* loader;
            Result* result;
        };
        g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer p) -> gboolean {
            auto* d = static_cast<Dispatch*>(p);
            d->loader->callback_(*d->result);
            delete d->result;
            delete d;
            return G_SOURCE_REMOVE;
        }, new Dispatch{this, r}, nullptr);
    }
}

// ------------------------------------------------------------------- App

namespace {

const char* kCss = R"css(
.ifrit-toolbar { padding: 2px 4px; }
.ifrit-footer { padding: 3px 8px; border-top: 1px solid alpha(currentColor, 0.15); }
.ifrit-footer label { font-size: 90%; opacity: 0.85; }
)css";

DecodeOptions decode_options_for(const Config& cfg) {
    DecodeOptions o;
    using RawMode = DecodeOptions::RawMode;
    o.raw_mode = cfg.raw_mode == "full" ? RawMode::Full : cfg.raw_mode == "preview" ? RawMode::Preview : RawMode::Half;
    return o;
}

std::string human_size(uint64_t bytes) {
    gchar* s = g_format_size(bytes);
    std::string r = s;
    g_free(s);
    return r;
}

}  // namespace

App::App() : loader_([this](Loader::Result& r) { on_loaded(r); }) {
    cfg_.load();
    set_decode_options(decode_options_for(cfg_));
    build_ui();
}

GtkWidget* App::tool_button(const char* icon, const char* tooltip, void (App::*action)()) {
    GtkWidget* b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_set_can_focus(b, FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    // Member-function pointers can't cross a C callback, so box one per button.
    auto* slot = new (void (App::*)())(action);
    g_object_set_data_full(G_OBJECT(b), "ifrit-action", slot, +[](gpointer p) { delete static_cast<void (App::**)()>(p); });
    g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkWidget* w, gpointer self) {
                         auto* fn = static_cast<void (App::**)()>(g_object_get_data(G_OBJECT(w), "ifrit-action"));
                         (static_cast<App*>(self)->**fn)();
                     }), this);
    gtk_box_pack_start(GTK_BOX(toolbar_), b, FALSE, FALSE, 0);
    return b;
}

void App::build_ui() {
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, kCss, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window_), "IfritView");
    gtk_window_set_default_size(GTK_WINDOW(window_), cfg_.min_window_width, cfg_.min_window_height);
    gtk_window_set_icon_name(GTK_WINDOW(window_), "image-x-generic");

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window_), vbox);

    toolbar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar_), "ifrit-toolbar");
    gtk_box_pack_start(GTK_BOX(vbox), toolbar_, FALSE, FALSE, 0);
    tool_button("document-open-symbolic", "Open… (Ctrl+O)", &App::open_dialog);
    gtk_box_pack_start(GTK_BOX(toolbar_), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    tool_button("go-previous-symbolic", "Previous image (Left, wheel up)", &App::go_prev_action);
    tool_button("go-next-symbolic", "Next image (Right, wheel down)", &App::go_next_action);
    gtk_box_pack_start(GTK_BOX(toolbar_), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    tool_button("zoom-out-symbolic", "Zoom out (-, Ctrl+wheel)", &App::zoom_out);
    tool_button("zoom-in-symbolic", "Zoom in (+, Ctrl+wheel)", &App::zoom_in);
    tool_button("zoom-original-symbolic", "Actual pixels (1)", &App::zoom_original);
    tool_button("zoom-fit-best-symbolic", "Fit to window (F)", &App::zoom_fit);
    gtk_box_pack_start(GTK_BOX(toolbar_), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    tool_button("view-fullscreen-symbolic", "Fullscreen (Enter, double-click)", &App::toggle_fullscreen);
    GtkWidget* settings = tool_button("preferences-system-symbolic", "Settings (Ctrl+P)", &App::open_settings_action);
    gtk_box_set_child_packing(GTK_BOX(toolbar_), settings, FALSE, FALSE, 0, GTK_PACK_END);

    view_ = std::make_unique<ImageView>(cfg_);
    view_->on_action = [this](Action a) {
        switch (a) {
            case Action::NextImage: navigate(+1); break;
            case Action::PrevImage: navigate(-1); break;
            case Action::ToggleFullscreen: toggle_fullscreen(); break;
            case Action::OpenFile: open_dialog(); break;
            case Action::Settings: open_settings(); break;
            case Action::Quit: quit(); break;
            default: break;
        }
    };
    view_->on_zoom_changed = [this] { update_status(); };
    gtk_box_pack_start(GTK_BOX(vbox), view_->widget(), TRUE, TRUE, 0);

    footer_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(footer_), "ifrit-footer");
    gtk_box_pack_start(GTK_BOX(vbox), footer_, FALSE, FALSE, 0);
    status_left_ = gtk_label_new("");
    status_center_ = gtk_label_new("");
    status_right_ = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(status_left_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_ellipsize(GTK_LABEL(status_center_), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(status_left_), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(status_right_), 1.0f);
    gtk_box_pack_start(GTK_BOX(footer_), status_left_, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(footer_), status_center_, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(footer_), status_right_, FALSE, FALSE, 0);

    g_signal_connect(window_, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* e, gpointer self) {
                         return static_cast<App*>(self)->on_key(e);
                     }), this);
    g_signal_connect(window_, "window-state-event", G_CALLBACK(+[](GtkWidget*, GdkEventWindowState* e, gpointer self) -> gboolean {
                         static_cast<App*>(self)->on_window_state(e);
                         return FALSE;
                     }), this);
    g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) { gtk_main_quit(); }), nullptr);

    gtk_drag_dest_set(window_, GTK_DEST_DEFAULT_ALL, nullptr, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(window_);
    g_signal_connect(window_, "drag-data-received",
                     G_CALLBACK(+[](GtkWidget*, GdkDragContext* ctx, gint, gint, GtkSelectionData* data, guint, guint time, gpointer self) {
                         static_cast<App*>(self)->on_drop(data, time, ctx);
                     }), this);

    update_status();
    gtk_widget_show_all(window_);
}

void App::run() {
    gtk_main();
}

void App::quit() {
    gtk_widget_destroy(window_);
}

// ------------------------------------------------------------ navigation

void App::open_path(const std::string& path) {
    gchar* canonical = g_canonicalize_filename(path.c_str(), nullptr);
    folder_.scan(canonical, cfg_);
    g_free(canonical);
    load_current();
}

void App::load_current() {
    const std::string path = folder_.current();
    if (path.empty()) {
        status_message_ = folder_.directory().empty() ? "No image open" : "No images in " + folder_.directory();
        update_status();
        update_title();
        return;
    }
    status_message_ = "Loading " + folder_.current_name() + "…";
    update_status();
    loader_.request(++load_generation_, path);
}

void App::navigate(int delta) {
    if (folder_.step(delta, cfg_.wrap_around)) load_current();
}

void App::go_first() {
    if (folder_.go(0)) load_current();
}

void App::go_last() {
    if (folder_.go(folder_.count() - 1)) load_current();
}

void App::go_prev_action() { navigate(-1); }
void App::go_next_action() { navigate(+1); }

void App::reload() {
    folder_.rescan(cfg_);
    load_current();
}

void App::on_loaded(Loader::Result& r) {
    if (r.generation != load_generation_) return;  // superseded
    status_message_.clear();
    if (!r.image) {
        image_.reset();
        image_path_ = r.path;
        view_->set_image(nullptr);
        status_message_ = "Cannot load " + folder_.current_name() + ": " + r.error;
        update_status();
        update_title();
        return;
    }
    image_ = r.image;
    image_path_ = r.path;
    std::error_code ec;
    file_size_ = std::filesystem::file_size(r.path, ec);
    if (ec) file_size_ = 0;
    view_->set_image(image_);
    fit_window_to_image();
    update_title();
    update_status();
}

// --------------------------------------------------------------- window

// Size the window so the image shows 1:1 when it fits on the monitor,
// but never smaller than the configured minimum and never larger than
// the work area.
void App::fit_window_to_image() {
    if (!image_ || fullscreen_ || !cfg_.fit_window_to_image) return;
    GdkWindow* gw = gtk_widget_get_window(window_);
    if (!gw) return;
    const GdkWindowState state = gdk_window_get_state(gw);
    if (state & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_TILED)) return;

    GdkDisplay* display = gtk_widget_get_display(window_);
    GdkMonitor* monitor = gdk_display_get_monitor_at_window(display, gw);
    if (!monitor) monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle work{0, 0, 1920, 1080};
    if (monitor) gdk_monitor_get_workarea(monitor, &work);

    int chrome_h = 0, min = 0, nat = 0;
    gtk_widget_get_preferred_height(toolbar_, &min, &nat);
    chrome_h += nat;
    gtk_widget_get_preferred_height(footer_, &min, &nat);
    chrome_h += nat;

    // Leave room for the title bar and a little air around the window.
    const int margin_x = 24, margin_y = 72;
    const int max_w = work.width - margin_x, max_h = work.height - margin_y;
    const double scale = view_->scale_factor();
    const double iw = image_->width / scale, ih = image_->height / scale;   // logical pixels at 1:1
    const double s = std::min({1.0, max_w / iw, (max_h - chrome_h) / ih});
    const int cw = int(std::ceil(iw * s)), ch = int(std::ceil(ih * s));
    const int w = std::clamp(cw, cfg_.min_window_width, max_w);
    const int h = std::clamp(ch + chrome_h, cfg_.min_window_height, max_h);

    int cur_w = 0, cur_h = 0;
    gtk_window_get_size(GTK_WINDOW(window_), &cur_w, &cur_h);
    if (cur_w != w || cur_h != h) gtk_window_resize(GTK_WINDOW(window_), w, h);
}

void App::toggle_fullscreen() {
    if (fullscreen_) gtk_window_unfullscreen(GTK_WINDOW(window_));
    else gtk_window_fullscreen(GTK_WINDOW(window_));
}

void App::on_window_state(GdkEventWindowState* e) {
    const bool fs = e->new_window_state & GDK_WINDOW_STATE_FULLSCREEN;
    if (fs == fullscreen_) return;
    fullscreen_ = fs;
    gtk_widget_set_visible(toolbar_, !fs);
    gtk_widget_set_visible(footer_, !fs);
    view_->set_fullscreen(fs);
    if (!fs) fit_window_to_image();
}

void App::zoom_in() { view_->zoom_step(+1); }
void App::zoom_out() { view_->zoom_step(-1); }
void App::zoom_original() { view_->zoom_original(); }
void App::zoom_fit() { view_->zoom_fit(); }

void App::open_dialog() {
    GtkFileChooserNative* dialog = gtk_file_chooser_native_new("Open Image", GTK_WINDOW(window_),
                                                               GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
    GtkFileFilter* images = gtk_file_filter_new();
    gtk_file_filter_set_name(images, "Images");
    for (const std::string& e : cfg_.extensions) gtk_file_filter_add_pattern(images, ("*." + e).c_str());
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), images);
    GtkFileFilter* all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all);
    if (!folder_.directory().empty()) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), folder_.directory().c_str());

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) open_path(path);
        g_free(path);
    }
    g_object_unref(dialog);
}

void App::open_settings(const char* page) {
    const std::string old_raw_mode = cfg_.raw_mode;
    if (!run_settings_dialog(GTK_WINDOW(window_), cfg_, page)) return;
    cfg_.save();
    set_decode_options(decode_options_for(cfg_));
    folder_.rescan(cfg_);
    view_->config_changed();
    if (cfg_.raw_mode != old_raw_mode && image_ && image_->format.find("RAW") != std::string::npos) load_current();
    update_status();
}

void App::on_drop(GtkSelectionData* data, guint time, GdkDragContext* ctx) {
    gchar** uris = gtk_selection_data_get_uris(data);
    bool ok = false;
    if (uris && uris[0]) {
        if (gchar* path = g_filename_from_uri(uris[0], nullptr, nullptr)) {
            open_path(path);
            g_free(path);
            ok = true;
        }
    }
    g_strfreev(uris);
    gtk_drag_finish(ctx, ok, FALSE, time);
}

// --------------------------------------------------------------- status

void App::update_title() {
    std::string title = "IfritView";
    if (!folder_.empty()) {
        title = folder_.current_name() + " (" + std::to_string(folder_.index() + 1) + "/" + std::to_string(folder_.count()) + ") – IfritView";
    }
    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
}

void App::update_status() {
    std::string left, center, right;
    if (!folder_.empty()) {
        left = folder_.current_name() + "  (" + std::to_string(folder_.index() + 1) + " / " + std::to_string(folder_.count()) + ")";
    }
    if (!status_message_.empty()) {
        left = status_message_;
    }
    if (image_) {
        center = std::to_string(image_->width) + " × " + std::to_string(image_->height) + " × " +
                 (image_->has_alpha ? "32" : "24") + " bpp";
        if (image_->bits_per_sample != 8) center += " (" + std::to_string(image_->bits_per_sample) + "-bit)";
        if (!image_->format.empty()) center += "  ·  " + image_->format;
        if (file_size_) center += "  ·  " + human_size(file_size_);
        if (!image_->info.empty()) center += "  ·  " + image_->info;
        const int percent = int(std::lround(view_->zoom() * 100.0));
        right = std::to_string(percent) + "%";
        if (view_->zoom_mode() == ImageView::ZoomMode::Fit && view_->zoom() < 1.0) right += " (fit)";
    }
    gtk_label_set_text(GTK_LABEL(status_left_), left.c_str());
    gtk_label_set_text(GTK_LABEL(status_center_), center.c_str());
    gtk_label_set_text(GTK_LABEL(status_right_), right.c_str());
}

gboolean App::on_key(GdkEventKey* e) {
    const bool ctrl = e->state & GDK_CONTROL_MASK;
    switch (e->keyval) {
        case GDK_KEY_Left: case GDK_KEY_Page_Up: case GDK_KEY_BackSpace:
            navigate(-1); return TRUE;
        case GDK_KEY_Right: case GDK_KEY_Page_Down: case GDK_KEY_space:
            navigate(+1); return TRUE;
        case GDK_KEY_Home: go_first(); return TRUE;
        case GDK_KEY_End: go_last(); return TRUE;
        case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_F11:
            toggle_fullscreen(); return TRUE;
        case GDK_KEY_Escape:
            if (fullscreen_) toggle_fullscreen();
            else quit();
            return TRUE;
        case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
            zoom_in(); return TRUE;
        case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
            zoom_out(); return TRUE;
        case GDK_KEY_1: case GDK_KEY_KP_1:
            zoom_original(); return TRUE;
        case GDK_KEY_f: case GDK_KEY_F:
            zoom_fit(); return TRUE;
        case GDK_KEY_r: case GDK_KEY_F5:
            reload(); return TRUE;
        case GDK_KEY_o: case GDK_KEY_O:
            if (ctrl) { open_dialog(); return TRUE; }
            break;
        case GDK_KEY_p: case GDK_KEY_P:
            if (ctrl) { open_settings(); return TRUE; }
            break;
        case GDK_KEY_q: case GDK_KEY_Q:
            if (ctrl) { quit(); return TRUE; }
            break;
        default:
            break;
    }
    return FALSE;
}
