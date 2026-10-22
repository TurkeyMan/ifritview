#include "app.h"
#include "platform.h"
#include "probe.h"

#include <rapport/rapport.h>

#include <glib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Statically linked backends, registered here (a registration inside the
// static backend library would be dropped by the linker).
#ifdef IFRIT_BACKEND_gtk3
RAPPORT_BUILTIN_BACKEND(gtk3);
#endif
#ifdef IFRIT_BACKEND_gtk4
RAPPORT_BUILTIN_BACKEND(gtk4);
#endif
#ifdef IFRIT_BACKEND_qt6
RAPPORT_BUILTIN_BACKEND(qt6);
#endif
#ifdef IFRIT_BACKEND_win32
RAPPORT_BUILTIN_BACKEND(win32);
#endif
#ifdef IFRIT_BACKEND_null
RAPPORT_BUILTIN_BACKEND(null);
#endif

namespace
{

// Development aid: IFRIT_SCRIPT="next;next;fullscreen;quit" drives the app
// without a human, one step every IFRIT_SCRIPT_MS (default 400). Screenshots:
// run with RAPPORT_SNAPSHOT=file.png (and RAPPORT_SNAPSHOT_DELAY) set.
void run_step(App& app, const std::string& step)
{
    std::fprintf(stderr, "script: %s (zoom %.3f)\n", step.c_str(), app.view().zoom());
    if (step == "next")
        app.navigate(+1);
    else if (step == "prev")
        app.navigate(-1);
    else if (step == "first")
        app.go_first();
    else if (step == "last")
        app.go_last();
    else if (step == "fullscreen")
        app.toggle_fullscreen();
    else if (step == "zoomin")
        app.zoom_in();
    else if (step == "zoomout")
        app.zoom_out();
    else if (step == "fit")
        app.zoom_fit();
    else if (step == "orig")
        app.zoom_original();
    else if (step == "reload")
        app.reload();
    else if (step.rfind("settings", 0) == 0)
        app.open_settings_page(step.size() > 9 ? std::atoi(step.c_str() + 9) : 0);  // "settings:2" opens Controls
    else if (step == "wait")
    {
    }
    else if (step == "metadata")
        app.toggle_metadata();
    else if (step == "copy")
        app.copy_to_clipboard();
    else if (step == "xmp")
        app.script_toggle_xmp();
    else if (step == "volume")
        app.script_show_volume();
    else if (step == "close-dialog")
        app.script_end_dialog(false);
    else if (step == "accept-dialog")
        app.script_end_dialog(true);
    else if (step == "slideshow")
        app.toggle_slideshow();
    else if (step == "play")
        app.script_toggle_video();
    else if (step == "controls")
        app.script_poke_controls();
    else if (step == "rotl")
        app.transform(8);
    else if (step == "rotr")
        app.transform(6);
    else if (step.rfind("ev:", 0) == 0)
        app.set_exposure(std::stod(step.substr(3)));
    else if (step == "save")
        app.save_as();
    else if (step == "quit")
        app.quit();
    else
        std::fprintf(stderr, "script: unknown step '%s'\n", step.c_str());
}

}  // namespace

int run_app(rap::Ui& ui, int argc, char** argv, std::chrono::steady_clock::time_point t_start);

int main(int argc, char** argv)
{
    const auto t_start = std::chrono::steady_clock::now();
    g_set_prgname("ifritview");
    g_set_application_name("IfritView");
    if (argc > 1 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0))
    {
        std::printf(
            "usage: ifritview [image-file | directory]\n"
            "       ifritview --probe file...   decode files and report, no window\n"
            "       ifritview --convert in out [setting=value...]   save `in` as `out`, no window\n"
            "       ifritview --version\n");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0)
    {
        std::printf("IfritView %s\n", IFRIT_VERSION);
        return 0;
    }
    if (argc > 3 && std::strcmp(argv[1], "--convert") == 0)
        return convert(argc - 2, argv + 2);
    if (argc > 2 && std::strcmp(argv[1], "--probe") == 0)
        return probe(argc - 2, argv + 2);

    auto ui = rap::Ui::load(argc, argv);
    if (!ui)
    {
        std::fprintf(stderr, "%s\n", ui.error().c_str());
        return 1;
    }
    // Results from worker threads (lookups, video frames) come back through
    // the toolkit's loop; with Win32 there is no GLib loop to carry them.
    rap::Ui& toolkit = **ui;
    platform::set_main_dispatch([&toolkit](std::function<void()> fn) { toolkit.post(std::move(fn)); });
    const int rc = run_app(**ui, argc, argv, t_start);
    platform::set_main_dispatch([](std::function<void()>) {});  // late arrivals after the toolkit goes
    return rc;
}

int run_app(rap::Ui& ui, int argc, char** argv, std::chrono::steady_clock::time_point t_start)
{
    App app(ui);
    if (argc > 1)
        app.open_path(argv[1]);
    if (g_getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "startup: %.0f ms to window creation (%s)\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count(), std::string(ui.backendName()).c_str());

    rap::Subscription script;
    std::vector<std::string> steps;
    size_t next = 0;
    if (const char* text = g_getenv("IFRIT_SCRIPT"))
    {
        gchar** parts = g_strsplit(text, ";", -1);
        for (gchar** p = parts; *p; ++p)
            if (**p)
                steps.push_back(*p);
        g_strfreev(parts);
        const char* ms = g_getenv("IFRIT_SCRIPT_MS");
        script = ui.timer(ms ? std::atoi(ms) : 400, true,
                              [&]
                              {
                                  if (next < steps.size())
                                      run_step(app, steps[next++]);
                              });
    }
    return app.run();
}
