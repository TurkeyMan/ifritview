#include "app.h"

#include "decoders/decoder.h"
#include "encoders/encoder.h"
#include "settings.h"

#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>

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

void Loader::request(uint64_t generation, const std::string& path, bool develop) {
    {
        std::lock_guard lock(mutex_);
        pending_ = Job{generation, path, develop};
    }
    wake_.notify_one();
}

void Loader::prefetch(std::vector<std::string> paths) {
    {
        std::lock_guard lock(mutex_);
        prefetch_ = std::move(paths);
    }
    wake_.notify_one();
}

bool Loader::superseded(uint64_t generation) {
    std::lock_guard lock(mutex_);
    return stop_ || (pending_ && pending_->generation != generation);
}

void Loader::post(Result* r) {
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

void Loader::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || pending_ || !prefetch_.empty(); });
            if (stop_) return;
            if (pending_) {
                job = *pending_;
                pending_.reset();
            } else {
                // Idle: decode the next neighbour the same way a normal load
                // would, and hand it to the cache.
                const std::string path = prefetch_.front();
                prefetch_.erase(prefetch_.begin());
                lock.unlock();
                auto* r = new Result{0, path, std::make_shared<Image>(), {}, false, true};
                if (decode_preview(path, *r->image)) r->preview = true;
                else if (!decode_image(path, *r->image, r->error)) r->image.reset();
                post(r);
                continue;
            }
        }
        // Quick stand-in first (a raw's embedded JPEG), so flicking through a
        // folder shows something at once. The real decode follows only when
        // asked for and only if the user is still on this file.
        static uint64_t previewed = 0;
        if (previewed != job.generation) {
            auto preview = std::make_shared<Image>();
            if (decode_preview(job.path, *preview)) {
                previewed = job.generation;
                post(new Result{job.generation, job.path, preview, {}, true});
                if (!job.develop || superseded(job.generation)) continue;
            }
        }
        auto* r = new Result{job.generation, job.path, std::make_shared<Image>(), {}, false};
        if (!decode_image(r->path, *r->image, r->error)) r->image.reset();
        post(r);
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

App::App() : loader_([this](Loader::Result& r) { on_loaded(r); }), cache_(0) {
    cfg_.load();
    cache_.set_budget(size_t(cfg_.cache_megabytes) << 20);
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
    // Installed builds find the icon by name; a build tree uses the SVG directly.
    gtk_window_set_default_icon_name("ifritview");
    if (!gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), "ifritview"))
        gtk_window_set_default_icon_from_file(IFRIT_SOURCE_DIR "/data/ifritview.svg", nullptr);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window_), vbox);

    toolbar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar_), "ifrit-toolbar");
    gtk_box_pack_start(GTK_BOX(vbox), toolbar_, FALSE, FALSE, 0);
    tool_button("document-open-symbolic", "Open… (Ctrl+O)", &App::open_dialog);
    tool_button("media-floppy-symbolic", "Save as… (Ctrl+S)", &App::save_as);
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
    gtk_box_pack_start(GTK_BOX(toolbar_), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    crop_button_ = tool_button("edit-cut-symbolic", "Crop to selection (Ctrl+Y)", &App::crop_to_selection);
    gtk_widget_set_sensitive(crop_button_, FALSE);

    smooth_toggle_ = gtk_toggle_button_new();
    gtk_container_add(GTK_CONTAINER(smooth_toggle_), gtk_image_new_from_icon_name("view-grid-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_button_set_relief(GTK_BUTTON(smooth_toggle_), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(smooth_toggle_, "Smooth when zoomed in (off shows crisp pixels)");
    gtk_widget_set_can_focus(smooth_toggle_, FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(smooth_toggle_), cfg_.smooth_upscale);
    g_signal_connect(smooth_toggle_, "toggled", G_CALLBACK(+[](GtkToggleButton*, gpointer self) {
                         static_cast<App*>(self)->on_smooth_toggled();
                     }), this);
    gtk_box_pack_start(GTK_BOX(toolbar_), smooth_toggle_, FALSE, FALSE, 0);

    xmp_toggle_ = gtk_toggle_button_new();
    gtk_container_add(GTK_CONTAINER(xmp_toggle_), gtk_image_new_from_icon_name("document-edit-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_button_set_relief(GTK_BUTTON(xmp_toggle_), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(xmp_toggle_, "Apply edits from XMP sidecars (exposure, crop, rotation)");
    gtk_widget_set_can_focus(xmp_toggle_, FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(xmp_toggle_), cfg_.apply_xmp_edits);
    g_signal_connect(xmp_toggle_, "toggled", G_CALLBACK(+[](GtkToggleButton*, gpointer self) {
                         static_cast<App*>(self)->on_xmp_toggled();
                     }), this);
    gtk_box_pack_start(GTK_BOX(toolbar_), xmp_toggle_, FALSE, FALSE, 0);

    GtkWidget* settings = tool_button("preferences-system-symbolic", "Settings (Ctrl+P)", &App::open_settings_action);
    gtk_box_set_child_packing(GTK_BOX(toolbar_), settings, FALSE, FALSE, 0, GTK_PACK_END);

    // Exposure, in stops around the decoder's automatic brightness; raw only.
    exposure_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(exposure_box_), gtk_image_new_from_icon_name("display-brightness-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR), FALSE, FALSE, 4);
    exposure_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -3.0, 3.0, 0.01);
    gtk_range_set_round_digits(GTK_RANGE(exposure_), -1);   // continuous: every screen pixel is a value
    gtk_scale_set_value_pos(GTK_SCALE(exposure_), GTK_POS_RIGHT);
    g_signal_connect(exposure_, "format-value", G_CALLBACK(+[](GtkScale*, gdouble v, gpointer) -> gchar* {
                         return g_strdup_printf("%+.2f", v);
                     }), nullptr);
    gtk_scale_add_mark(GTK_SCALE(exposure_), 0.0, GTK_POS_BOTTOM, nullptr);
    gtk_widget_set_size_request(exposure_, 200, -1);
    gtk_widget_set_tooltip_text(exposure_, "Exposure (EV). Rests at the camera's compensation, i.e. as shot.\nWheel or arrows: 0.1 per step, hold Shift for 0.01; Home resets");
    gtk_widget_set_can_focus(exposure_, TRUE);
    // Wheel: 0.1 per notch, Shift refines to 0.01.
    gtk_widget_add_events(exposure_, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(exposure_, "scroll-event", G_CALLBACK(+[](GtkWidget*, GdkEventScroll* e, gpointer self) -> gboolean {
                         auto* app = static_cast<App*>(self);
                         double dir = 0;
                         if (e->direction == GDK_SCROLL_UP) dir = -1;
                         else if (e->direction == GDK_SCROLL_DOWN) dir = 1;
                         else if (e->direction == GDK_SCROLL_SMOOTH && !gdk_event_get_pointer_emulated(reinterpret_cast<GdkEvent*>(e))) dir = e->delta_y;
                         else return TRUE;
                         const double step = (e->state & GDK_SHIFT_MASK) ? 0.01 : 0.1;
                         app->set_exposure(gtk_range_get_value(GTK_RANGE(app->exposure_)) - dir * step);   // wheel up brightens
                         gtk_widget_grab_focus(app->exposure_);
                         return TRUE;
                     }), this);
    g_signal_connect(exposure_, "value-changed", G_CALLBACK(+[](GtkRange*, gpointer self) {
                         static_cast<App*>(self)->on_exposure_changed();
                     }), this);
    gtk_box_pack_start(GTK_BOX(exposure_box_), exposure_, FALSE, FALSE, 0);

    gtk_box_pack_end(GTK_BOX(toolbar_), exposure_box_, FALSE, FALSE, 8);   // sits left of the settings button
    // Children are shown now; the box itself is toggled per image.
    gtk_widget_show_all(exposure_box_);
    gtk_widget_hide(exposure_box_);
    gtk_widget_set_no_show_all(exposure_box_, TRUE);
    GtkWidget* metadata = tool_button("dialog-information-symbolic", "Metadata / EXIF (I)", &App::toggle_metadata);
    gtk_box_set_child_packing(GTK_BOX(toolbar_), metadata, FALSE, FALSE, 0, GTK_PACK_END);


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
    view_->on_selection_changed = [this] {
        int x, y, w, h;
        gtk_widget_set_sensitive(crop_button_, view_->selection(x, y, w, h));
    };
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
    status_location_ = gtk_label_new("");
    gtk_label_set_track_visited_links(GTK_LABEL(status_location_), FALSE);
    gtk_widget_set_no_show_all(status_location_, TRUE);
    gtk_box_pack_start(GTK_BOX(footer_), status_location_, FALSE, FALSE, 0);
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

    metadata_ = std::make_unique<MetadataWindow>(GTK_WINDOW(window_));
    update_status();
    gtk_widget_show_all(window_);
}

// Ctrl+C: the selection if there is one, otherwise the whole image. The
// selection is dropped afterwards as the visible sign that it was copied.
void App::copy_to_clipboard() {
    if (!image_) return;
    int x, y, w, h;
    const Image region = view_->selection(x, y, w, h) ? image_->crop(x, y, w, h) : *image_;
    std::vector<uint8_t> rgba = to_rgba8(region);
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_data(rgba.data(), GDK_COLORSPACE_RGB, TRUE, 8, region.width, region.height,
                                                 region.width * 4, nullptr, nullptr);
    GdkPixbuf* copy = gdk_pixbuf_copy(pixbuf);   // owns its pixels, outlives `rgba`
    g_object_unref(pixbuf);
    gtk_clipboard_set_image(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), copy);
    g_object_unref(copy);
    view_->clear_selection();
    status_message_ = "Copied " + std::to_string(region.width) + " × " + std::to_string(region.height) + " to clipboard";
    update_status();
}

void App::transform(int exif_orientation) {
    if (!image_) return;
    image_modified_ = true;
    if (showing_preview_) preview_edits_.push_back({exif_orientation, 0, 0, 1, 1});
    image_->apply_orientation(exif_orientation);
    view_->set_image(image_);   // dimensions may have swapped
    fit_window_to_image();
    update_status();
    refresh_metadata();
}

void App::toggle_metadata() {
    metadata_->toggle();
    if (metadata_->visible()) refresh_metadata();
}

std::vector<MetadataGroup> App::collect_metadata() const {
    std::vector<MetadataGroup> groups;
    if (!image_) return groups;
    MetadataGroup file{"File", {}};
    file.items.push_back({"Name", folder_.current_name()});
    file.items.push_back({"Folder", folder_.directory()});
    if (file_size_) file.items.push_back({"Size", human_size(file_size_) + " (" + std::to_string(file_size_) + " bytes)"});
    GStatBuf st;
    if (g_stat(image_path_.c_str(), &st) == 0) {
        if (GDateTime* t = g_date_time_new_from_unix_local(st.st_mtime)) {
            gchar* text = g_date_time_format(t, "%Y-%m-%d %H:%M:%S");
            file.items.push_back({"Modified", text});
            g_free(text);
            g_date_time_unref(t);
        }
    }
    groups.push_back(std::move(file));
    MetadataGroup img{"Image", {}};
    img.items.push_back({"Dimensions", std::to_string(image_->width) + " × " + std::to_string(image_->height)});
    img.items.push_back({"Format", image_->format});
    img.items.push_back({"Source depth", std::to_string(image_->bits_per_sample) + " bits per sample"});
    img.items.push_back({"Alpha", image_->has_alpha ? "yes" : "no"});
    if (!image_->info.empty()) img.items.push_back({"Notes", image_->info});
    groups.push_back(std::move(img));
    for (const MetadataGroup& g : image_->metadata) groups.push_back(g);
    if (image_->has_location) {
        bool has_gps = false;
        for (const MetadataGroup& g : groups) has_gps |= g.name == "GPS";
        if (!has_gps) groups.push_back({"GPS", {}});
    }
    return groups;
}

void App::refresh_metadata() {
    if (!metadata_ || !metadata_->visible()) return;
    const std::string url = image_ && image_->has_location ? cfg_.map_link(image_->latitude, image_->longitude) : "";
    metadata_->set(folder_.current_name(), collect_metadata(), image_.get(), url);
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
    // Leaving an image: keep it (edits included) and remember the slider so
    // coming back shows exactly what was left, which is what flipping between
    // the shots of a burst wants.
    if (image_ && !image_path_.empty()) {
        ImageCache::Entry e{image_, showing_preview_, ImageCache::file_mtime(image_path_)};
        if (gtk_widget_get_visible(exposure_box_)) {
            e.has_exposure = true;
            e.exposure = gtk_range_get_value(GTK_RANGE(exposure_));
        }
        cache_.put(image_path_, e);
    }
    image_modified_ = false;
    showing_preview_ = false;
    develop_requested_ = !cfg_.raw_develop_on_demand;
    preview_edits_.clear();
    ++load_generation_;
    ImageCache::Entry hit;
    if (cache_.get(path, hit)) {
        Loader::Result r{load_generation_, path, hit.image, {}, hit.preview, false};
        on_loaded(r);
        if (hit.has_exposure && gtk_widget_get_visible(exposure_box_)) {   // pixels already match this value
            g_signal_handlers_block_matched(exposure_, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
            gtk_range_set_value(GTK_RANGE(exposure_), hit.exposure);
            g_signal_handlers_unblock_matched(exposure_, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
        }
        return;
    }
    status_message_ = "Loading " + folder_.current_name() + "…";
    update_status();
    loader_.request(load_generation_, path, develop_requested_);
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
    if (r.prefetch) {
        if (r.image && r.path != image_path_) cache_.put(r.path, {r.image, r.preview, ImageCache::file_mtime(r.path)});
        if (r.image && r.path == folder_.current() && !image_) {   // user moved onto it while it was decoding
            r.generation = load_generation_;
            r.prefetch = false;
        } else {
            return;
        }
    }
    if (r.generation != load_generation_) return;  // superseded
    const bool from_cache = cache_.contains(r.path);
    schedule_prefetch();
    showing_preview_ = r.preview;
    if (g_getenv("IFRIT_TRACE") && r.image) std::fprintf(stderr, "loaded %s %dx%d preview=%d cache=%zuMB\n", r.path.c_str(), r.image->width, r.image->height, int(r.preview), cache_.bytes() >> 20);
    status_message_ = r.preview && develop_requested_ ? "Developing…" : "";
    if (r.preview && !from_cache) {
        // Edits that need real pixels mean the preview can't stand for long.
        const XmpDevelop& d = r.image->xmp;
        if (cfg_.apply_xmp_edits && d.present && (d.has_crop || d.exposure != 0 || d.orientation != 1)) ensure_developed();
    } else if (!preview_edits_.empty() && r.image) {
        for (const Edit& e : preview_edits_) {   // redo what was done to the preview
            if (e.orientation != 1) r.image->apply_orientation(e.orientation);
            else *r.image = r.image->crop(int(e.x * r.image->width), int(e.y * r.image->height),
                                          int(e.w * r.image->width), int(e.h * r.image->height));
        }
        preview_edits_.clear();
    }
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
    refresh_metadata();
    const double slider_before = gtk_range_get_value(GTK_RANGE(exposure_));
    const bool slider_moved = !r.preview && gtk_widget_get_visible(exposure_box_) && std::fabs(slider_before - image_->exposure_bias) > 1e-6;
    if (image_->linear || showing_preview_) {
        // The slider reads in the shooter's units: it rests at the camera's
        // exposure compensation, which is already in the pixels, and the
        // render uses the difference from that.
        const double bias = image_->exposure_bias;
        g_signal_handlers_block_matched(exposure_, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
        gtk_range_set_range(GTK_RANGE(exposure_), bias - 3.0, bias + 3.0);
        gtk_scale_clear_marks(GTK_SCALE(exposure_));
        gtk_scale_add_mark(GTK_SCALE(exposure_), bias, GTK_POS_BOTTOM, nullptr);   // the unmodified image
        gtk_range_set_value(GTK_RANGE(exposure_), bias);
        g_signal_handlers_unblock_matched(exposure_, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);

        gtk_widget_show(exposure_box_);
        if (!from_cache && !showing_preview_ && cfg_.apply_xmp_edits && image_->xmp.present) apply_xmp_edits();
        else if (slider_moved) gtk_range_set_value(GTK_RANGE(exposure_), slider_before);
    } else if (!from_cache && cfg_.apply_xmp_edits && image_->xmp.present) {
        apply_xmp_edits();
    } else {
        gtk_widget_hide(exposure_box_);
    }
}

void App::set_exposure(double ev) {
    gtk_range_set_value(GTK_RANGE(exposure_), ev);
}

// Replaces the displayed image with the selected region. Nothing is written
// to disk; R reloads the original.
void App::crop_to_selection() {
    int x, y, w, h;
    if (!image_ || !view_->selection(x, y, w, h)) return;
    image_modified_ = true;
    if (showing_preview_) preview_edits_.push_back({1, double(x) / image_->width, double(y) / image_->height, double(w) / image_->width, double(h) / image_->height});
    auto cropped = std::make_shared<Image>(image_->crop(x, y, w, h));
    if (cropped->linear) cropped->retone(gtk_range_get_value(GTK_RANGE(exposure_)) - cropped->exposure_bias);
    cropped->info = (image_->info.empty() ? "" : image_->info + "  ·  ") + "cropped";
    image_ = cropped;
    view_->set_image(image_);
    fit_window_to_image();
    update_status();
    refresh_metadata();
}

// Save As with a format chooser; picking a format rewrites the extension of
// the proposed file name. Quality applies to the lossy formats, 100 = lossless
// where the format can do it.
void App::save_as() {
    if (!image_) return;
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Save Image As", GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileChooser* chooser = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
    if (!folder_.directory().empty()) gtk_file_chooser_set_current_folder(chooser, folder_.directory().c_str());

    // Format row + quality row below the file list.
    GtkWidget* extra = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(extra), gtk_label_new("Format"), FALSE, FALSE, 0);
    GtkWidget* format = gtk_combo_box_text_new();
    const auto& list = encoders();
    int initial = 0;
    const std::string current_ext = [&] {
        auto dot = image_path_.find_last_of('.');
        std::string e = dot == std::string::npos ? "" : image_path_.substr(dot + 1);
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
        return e == "jpeg" ? std::string("jpg") : e;
    }();
    for (size_t i = 0; i < list.size(); ++i) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format), list[i]->extension, list[i]->name);
        if (current_ext == list[i]->extension) initial = int(i);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(format), initial);
    gtk_box_pack_start(GTK_BOX(extra), format, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(extra), gtk_label_new("Quality"), FALSE, FALSE, 8);
    GtkWidget* quality = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(quality), 90);
    gtk_widget_set_tooltip_text(quality, "Lossy formats only; 100 is lossless for WebP and JPEG XL");
    gtk_box_pack_start(GTK_BOX(extra), quality, FALSE, FALSE, 0);
    gtk_widget_show_all(extra);
    gtk_file_chooser_set_extra_widget(chooser, extra);

    struct State { GtkFileChooser* chooser; GtkWidget* quality; };
    State state{chooser, quality};
    auto on_format = +[](GtkComboBox* combo, gpointer p) {
        auto* st = static_cast<State*>(p);
        const gchar* ext = gtk_combo_box_get_active_id(combo);
        if (!ext) return;
        for (const Encoder* e : encoders())
            if (std::string(e->extension) == ext) gtk_widget_set_sensitive(st->quality, e->lossy);
        gchar* name = gtk_file_chooser_get_current_name(st->chooser);
        std::string n = name ? name : "";
        g_free(name);
        auto dot = n.find_last_of('.');
        n = (dot == std::string::npos ? n : n.substr(0, dot)) + "." + ext;
        gtk_file_chooser_set_current_name(st->chooser, n.c_str());
    };
    g_signal_connect(format, "changed", G_CALLBACK(on_format), &state);
    gtk_file_chooser_set_current_name(chooser, folder_.current_name().c_str());
    on_format(GTK_COMBO_BOX(format), &state);   // apply the initial format's extension

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar* path = gtk_file_chooser_get_filename(chooser);
        const gchar* ext = gtk_combo_box_get_active_id(GTK_COMBO_BOX(format));
        EncodeOptions options;
        options.quality = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(quality));
        std::string error;
        bool ok = false;
        for (const Encoder* e : list)
            if (ext && std::string(e->extension) == ext) ok = e->encode(*image_, path, options, error);
        if (ok) {
            gchar* base = g_path_get_basename(path);
            status_message_ = std::string("Saved ") + base;
            g_free(base);
            folder_.rescan(cfg_);   // the new file may now be part of the folder
        } else {
            status_message_ = "Save failed: " + error;
        }
        g_free(path);
        update_status();
        update_title();
    }
    gtk_widget_destroy(dialog);
}

void App::on_exposure_changed() {
    retone();
}

// Neighbours in the browsing direction first, then behind; skips what the
// cache already has.
void App::schedule_prefetch() {
    std::vector<std::string> paths;
    const int n = folder_.count();
    if (n <= 1) {
        loader_.prefetch({});
        return;
    }
    auto add = [&](int offset) {
        int i = folder_.index() + offset;
        if (cfg_.wrap_around) i = ((i % n) + n) % n;
        else if (i < 0 || i >= n) return;
        if (i == folder_.index()) return;
        gchar* full = g_build_filename(folder_.directory().c_str(), "", nullptr);
        g_free(full);
        const std::string path = folder_.directory() + "/" + folder_.name_at(i);
        if (!cache_.contains(path) && std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
    };
    for (int k = 1; k <= cfg_.prefetch_ahead || k <= cfg_.prefetch_behind; ++k) {
        if (k <= cfg_.prefetch_ahead) add(k);
        if (k <= cfg_.prefetch_behind) add(-k);
    }
    loader_.prefetch(std::move(paths));
}

void App::ensure_developed() {
    if (!showing_preview_ || develop_requested_) return;
    develop_requested_ = true;
    status_message_ = "Developing…";
    update_status();
    loader_.request(load_generation_, folder_.current(), true);
}

void App::retone() {
    if (showing_preview_) {
        ensure_developed();   // the slider value is picked up once the real pixels arrive
        return;
    }
    if (!image_ || !image_->linear) return;
    image_modified_ = true;
    image_->retone(gtk_range_get_value(GTK_RANGE(exposure_)) - image_->exposure_bias);
    view_->image_changed();
}

// Orientation and crop change the pixels; exposure just moves the slider,
// which re-renders. White balance from XMP is parsed but not applied yet.
void App::apply_xmp_edits() {
    const XmpDevelop d = image_->xmp;
    if (d.has_crop) {
        const int x = int(d.crop_left * image_->width), y = int(d.crop_top * image_->height);
        const int w = int((d.crop_right - d.crop_left) * image_->width), h = int((d.crop_bottom - d.crop_top) * image_->height);
        if (w > 1 && h > 1) *image_ = image_->crop(x, y, w, h);
    }
    if (d.orientation != 1) image_->apply_orientation(d.orientation);
    if (d.has_crop || d.orientation != 1) {
        view_->set_image(image_);
        fit_window_to_image();
    }
    if (image_->linear) {
        if (d.exposure != 0) gtk_range_set_value(GTK_RANGE(exposure_), image_->exposure_bias + d.exposure);
        else retone();
    }
    status_message_ = "XMP edits applied";
    update_status();
}

void App::on_xmp_toggled() {
    cfg_.apply_xmp_edits = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(xmp_toggle_));
    cfg_.save();
    if (image_ && image_->xmp.present) load_current();   // re-read with or without the edits
}

void App::on_smooth_toggled() {
    cfg_.smooth_upscale = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(smooth_toggle_));
    cfg_.save();
    view_->config_changed();
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
    cache_.set_budget(size_t(cfg_.cache_megabytes) << 20);
    folder_.rescan(cfg_);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(smooth_toggle_), cfg_.smooth_upscale);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(xmp_toggle_), cfg_.apply_xmp_edits);
    view_->config_changed();
    refresh_metadata();
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
        if (showing_preview_ && status_message_.empty()) center += "  ·  embedded preview";
        const int percent = int(std::lround(view_->zoom() * 100.0));
        right = std::to_string(percent) + "%";
        if (view_->zoom_mode() == ImageView::ZoomMode::Fit && view_->zoom() < 1.0) right += " (fit)";
    }
    if (image_ && image_->has_location) {
        gchar* markup = g_markup_printf_escaped("<a href=\"%s\">%s</a>", cfg_.map_link(image_->latitude, image_->longitude).c_str(),
                                                image_->location_text().c_str());
        gtk_label_set_markup(GTK_LABEL(status_location_), markup);
        g_free(markup);
        gtk_widget_set_tooltip_text(status_location_, "Open in maps");
        gtk_widget_show(status_location_);
    } else {
        gtk_widget_hide(status_location_);
    }
    gtk_label_set_text(GTK_LABEL(status_left_), left.c_str());
    gtk_label_set_text(GTK_LABEL(status_center_), center.c_str());
    gtk_label_set_text(GTK_LABEL(status_right_), right.c_str());
}

gboolean App::on_key(GdkEventKey* e) {
    const bool ctrl = e->state & GDK_CONTROL_MASK;
    const bool shift = e->state & GDK_SHIFT_MASK;
    if (gtk_widget_has_focus(exposure_) && gtk_widget_get_visible(exposure_box_)) {
        const double step = shift ? 0.01 : 0.1;   // Shift refines, same as the wheel
        switch (e->keyval) {
            case GDK_KEY_Left: case GDK_KEY_Down:
                set_exposure(gtk_range_get_value(GTK_RANGE(exposure_)) - step); return TRUE;
            case GDK_KEY_Right: case GDK_KEY_Up:
                set_exposure(gtk_range_get_value(GTK_RANGE(exposure_)) + step); return TRUE;
            case GDK_KEY_Home: set_exposure(image_ ? image_->exposure_bias : 0.0); return TRUE;
            case GDK_KEY_Escape: case GDK_KEY_Return: case GDK_KEY_KP_Enter:
                gtk_widget_grab_focus(view_->widget()); return TRUE;
            default: break;   // everything else keeps its normal meaning
        }
    }
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
        case GDK_KEY_F5:
            reload(); return TRUE;
        case GDK_KEY_l: case GDK_KEY_L: transform(8); return TRUE;   // rotate left
        case GDK_KEY_r: case GDK_KEY_R: transform(6); return TRUE;   // rotate right
        case GDK_KEY_h: case GDK_KEY_H: transform(2); return TRUE;   // mirror horizontally
        case GDK_KEY_v: case GDK_KEY_V: transform(4); return TRUE;   // mirror vertically
        case GDK_KEY_o: case GDK_KEY_O:
            if (ctrl) { open_dialog(); return TRUE; }
            break;
        case GDK_KEY_p: case GDK_KEY_P:
            if (ctrl) { open_settings(); return TRUE; }
            break;
        case GDK_KEY_q: case GDK_KEY_Q:
            if (ctrl) { quit(); return TRUE; }
            break;
        case GDK_KEY_y: case GDK_KEY_Y:
            if (ctrl) { crop_to_selection(); return TRUE; }
            break;
        case GDK_KEY_s: case GDK_KEY_S:
            if (ctrl) { save_as(); return TRUE; }
            break;
        case GDK_KEY_i: case GDK_KEY_I:
            toggle_metadata(); return TRUE;
        case GDK_KEY_c: case GDK_KEY_C:
            if (ctrl) { copy_to_clipboard(); return TRUE; }
            break;
        default:
            break;
    }
    return FALSE;
}
