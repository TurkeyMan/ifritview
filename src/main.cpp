#include "app.h"
#include "decoders/decoder.h"

#include <gtk/gtk.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Development aid: IFRIT_SCRIPT="next;next;fullscreen;shot:a.png;quit"
// drives the app without a human, one action every 400 ms. "shot:<file>"
// draws the frontmost window to a PNG, which works on Wayland too where a
// real screen capture would not.
struct Script {
    App* app;
    std::vector<std::string> steps;
    size_t next = 0;
    std::string page;
};

// The dialog or tool window when one is open, else the main window.
GtkWindow* frontmost_window() {
    GtkWindow* found = nullptr;
    GList* list = gtk_window_list_toplevels();
    for (GList* l = list; l; l = l->next) {
        auto* w = GTK_WINDOW(l->data);
        if (!gtk_widget_get_visible(GTK_WIDGET(w))) continue;
        if (!found || gtk_window_get_transient_for(w)) found = w;
    }
    g_list_free(list);
    return found;
}

void screenshot(GtkWindow* win, const std::string& path) {
    const int scale = gtk_widget_get_scale_factor(GTK_WIDGET(win));
    const int w = gtk_widget_get_allocated_width(GTK_WIDGET(win)), h = gtk_widget_get_allocated_height(GTK_WIDGET(win));
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * scale, h * scale);
    cairo_surface_set_device_scale(surface, scale, scale);
    cairo_t* cr = cairo_create(surface);
    gtk_widget_draw(GTK_WIDGET(win), cr);
    cairo_destroy(cr);
    cairo_surface_write_to_png(surface, path.c_str());
    cairo_surface_destroy(surface);
    int ww = 0, wh = 0;
    gtk_window_get_size(win, &ww, &wh);
    std::fprintf(stderr, "shot %s: window %dx%d, allocation %dx%d, scale %d\n", path.c_str(), ww, wh, w, h, scale);
}

gboolean run_script_step(gpointer p) {
    auto* s = static_cast<Script*>(p);
    if (s->next >= s->steps.size()) return G_SOURCE_CONTINUE;
    const std::string step = s->steps[s->next++];
    App& app = *s->app;
    std::fprintf(stderr, "script: %s (zoom %.3f)\n", step.c_str(), app.view().zoom());
    if (step == "next") app.navigate(+1);
    else if (step == "prev") app.navigate(-1);
    else if (step == "first") app.go_first();
    else if (step == "last") app.go_last();
    else if (step == "fullscreen") app.toggle_fullscreen();
    else if (step == "zoomin") app.zoom_in();
    else if (step == "zoomout") app.zoom_out();
    else if (step == "fit") app.zoom_fit();
    else if (step == "orig") app.zoom_original();
    else if (step == "reload") app.reload();
    else if (step.rfind("settings", 0) == 0) {
        // The dialog nests a main loop; open it from an idle so this timer
        // source finishes dispatching and keeps firing inside the dialog.
        // "settings:controls" opens on that page.
        s->page = step.size() > 9 ? step.substr(9) : "";
        g_idle_add(+[](gpointer p) -> gboolean {
            auto* s = static_cast<Script*>(p);
            s->app->open_settings(s->page.empty() ? nullptr : s->page.c_str());
            return G_SOURCE_REMOVE;
        }, s);
    }
    else if (step == "close-dialog") {
        if (GtkWindow* w = frontmost_window(); w && GTK_IS_DIALOG(w)) gtk_dialog_response(GTK_DIALOG(w), GTK_RESPONSE_CANCEL);
    } else if (step == "accept-dialog") {
        if (GtkWindow* w = frontmost_window(); w && GTK_IS_DIALOG(w)) gtk_dialog_response(GTK_DIALOG(w), GTK_RESPONSE_OK);
    } else if (step.rfind("shot:", 0) == 0) screenshot(frontmost_window(), step.substr(5));
    else if (step == "wait") {}
    else if (step == "metadata") app.toggle_metadata();
    else if (step == "copy") app.copy_to_clipboard();
    else if (step.rfind("ev:", 0) == 0) app.set_exposure(std::stod(step.substr(3)));
    else if (step == "quit") app.quit();
    else std::fprintf(stderr, "script: unknown step '%s'\n", step.c_str());
    return G_SOURCE_CONTINUE;
}

int probe(int argc, char** argv) {
    int failures = 0;
    for (int i = 0; i < argc; ++i) {
        Image img;
        std::string error;
        const gint64 t0 = g_get_monotonic_time();
        const bool ok = decode_image(argv[i], img, error);
        const double ms = (g_get_monotonic_time() - t0) / 1000.0;
        if (ok) {
            std::printf("ok    %-40s %5dx%-5d %s%s %s (%.0f ms)\n", argv[i], img.width, img.height,
                        img.format.c_str(), img.has_alpha ? " +alpha" : "", img.info.c_str(), ms);
        } else {
            std::printf("FAIL  %-40s %s\n", argv[i], error.c_str());
            ++failures;
        }
    }
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    g_set_prgname("ifritview");
    g_set_application_name("IfritView");
    gtk_init(&argc, &argv);

    if (argc > 1 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
        std::printf("usage: ifritview [image-file | directory]\n"
                    "       ifritview --probe file...   decode files and report, no window\n");
        return 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "--probe") == 0) return probe(argc - 2, argv + 2);

    App app;
    if (argc > 1) app.open_path(argv[1]);

    if (const char* script = g_getenv("IFRIT_SCRIPT")) {
        auto* s = new Script{&app, {}, 0, {}};
        gchar** parts = g_strsplit(script, ";", -1);
        for (gchar** p = parts; *p; ++p)
            if (**p) s->steps.push_back(*p);
        g_strfreev(parts);
        g_timeout_add(400, run_script_step, s);
    }
    app.run();
    return 0;
}
