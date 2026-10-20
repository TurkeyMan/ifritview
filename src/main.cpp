#include "app.h"
#include "decoders/decoder.h"

#include <gtk/gtk.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    g_set_prgname("ifritview");
    g_set_application_name("IfritView");
    gtk_init(&argc, &argv);

    if (argc > 1 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
        std::printf("usage: ifritview [image-file | directory]\n"
                    "       ifritview --probe file...   decode files and report, no window\n");
        return 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "--probe") == 0) {
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            Image img;
            std::string error;
            const gint64 t0 = g_get_monotonic_time();
            const bool ok = decode_image(argv[i], img, error);
            const double ms = (g_get_monotonic_time() - t0) / 1000.0;
            if (ok)
                std::printf("ok    %-40s %5dx%-5d %s%s %s (%.0f ms)\n", argv[i], img.width, img.height,
                            img.format.c_str(), img.has_alpha ? " +alpha" : "", img.info.c_str(), ms);
            else
                std::printf("FAIL  %-40s %s\n", argv[i], error.c_str()), ++failures;
        }
        return failures ? 1 : 0;
    }

    App app;
    if (argc > 1) app.open_path(argv[1]);

    // Development aid: IFRIT_SCREENSHOT=out.png saves the window after a
    // moment and exits, which lets the UI be checked without a human.
    if (const char* shot = g_getenv("IFRIT_SCREENSHOT")) {
        struct Shot { App* app; std::string path; };
        g_timeout_add(2500, +[](gpointer p) -> gboolean {
            auto* s = static_cast<Shot*>(p);
            GdkWindow* gw = gtk_widget_get_window(s->app->window());
            if (GdkPixbuf* pb = gw ? gdk_pixbuf_get_from_window(gw, 0, 0, gdk_window_get_width(gw), gdk_window_get_height(gw)) : nullptr) {
                gdk_pixbuf_save(pb, s->path.c_str(), "png", nullptr, nullptr);
                g_object_unref(pb);
            }
            delete s;
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }, new Shot{&app, shot});
    }
    app.run();
    return 0;
}
