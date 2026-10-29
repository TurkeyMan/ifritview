#include "app.h"
#include "platform.h"
#include "first_image.h"
#include "i18n.h"
#include "lens_store.h"
#include "pipeline.h"
#include "plugins.h"
#include "probe.h"

#include <rapport/rapport.h>
#include <rapport/sys.h>

#include <glib.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
// run with RAPPORT_SNAPSHOT=file.png (and RAPPORT_SNAPSHOT_DELAY) set; what a
// GPU drew isn't in them, but export:file.png writes the image as saving would
// (instead:jpeg, instead:libraw, instead:none, lens:on, lens:off first: the
// G panel's checks).
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
    else if (step == "page+" || step == "page-")
        app.step_page(step == "page+" ? 1 : -1);
    else if (step == "level+" || step == "level-")
        app.step_level(step == "level+" ? 1 : -1);
    else if (step == "print")
        app.print_image();  // with RAPPORT_PRINT_TO set, to a file without the dialog
    else if (step == "metadata")
        app.toggle_metadata();
    else if (step == "histogram")
        app.toggle_histogram();
    else if (step == "copy")
        app.copy_to_clipboard();
    else if (step.rfind("drop:", 0) == 0)
        app.on_dropped({step.substr(5)});
    else if (step == "recursive:on" || step == "recursive:off")
        app.set_recursive(step == "recursive:on");
    else if (step == "xmp")
        app.script_toggle_xmp();
    else if (step == "volume")
        app.script_show_volume();
    else if (step.rfind("shot:", 0) == 0)
    {
        // The window in front (a dialog over the main window, say), as drawn now.
        if (!app.ui().save_snapshot(step.substr(5)))
            std::fprintf(stderr, "script: no snapshot saved to %s\n", step.substr(5).c_str());
    }
    else if (step.rfind("press:", 0) == 0 || step.rfind("move:", 0) == 0 || step.rfind("release:", 0) == 0)
    {
        // The left button in the view, at x,y in its logical pixels (a
        // trailing ",shift" or ",ctrl" holds that key): the mouse untouched.
        using T = rap::pointer_event_type;
        const size_t colon = step.find(':');
        const std::string kind = step.substr(0, colon);
        rap::pointer_event e;
        e.type = kind == "press" ? T::press : kind == "release" ? T::release : T::move;
        e.button = 1;
        float x = 0, y = 0;
        std::sscanf(step.c_str() + colon + 1, "%f,%f", &x, &y);
        e.pos = {x, y};
        if (step.find(",shift") != std::string::npos)
            e.modifiers |= rap::mod_shift;
        if (step.find(",ctrl") != std::string::npos)
            e.modifiers |= rap::mod_ctrl;
        app.script_pointer(e);
    }
    else if (step == "lossless")
        app.script_toggle_lossless();
    else if (step == "crop")
        app.crop_to_selection();
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
    else if (step.rfind("instead:", 0) == 0)
        app.script_instead(step.substr(8));
    else if (step == "lens:on" || step == "lens:off")
        app.script_lens(step == "lens:on");
    else if (step.rfind("export:", 0) == 0)
        app.script_export(step.substr(7));
    else if (step.rfind("compare:", 0) == 0)
        app.script_compare(step.substr(8));
    else if (step == "quit")
        app.quit();
    else
        std::fprintf(stderr, "script: unknown step '%s'\n", step.c_str());
}

}  // namespace

int run_app(rap::ui& ui, int argc, char** argv, std::chrono::steady_clock::time_point t_start, FirstImage* first,
            std::shared_future<Config> settings);

int main(int argc, char** argv)
{
    const auto t_start = std::chrono::steady_clock::now();
    platform::trace("main");
#ifdef _WIN32
    // A GUI program has no console of its own: output already redirected
    // (to a file or pipe) stays there, and otherwise, run from a console
    // (--probe, --help, IFRIT_TRACE), it writes to that.
    const auto unset = [](DWORD which)
    {
        const HANDLE h = GetStdHandle(which);
        return h == nullptr || h == INVALID_HANDLE_VALUE;
    };
    if ((unset(STD_OUTPUT_HANDLE) || unset(STD_ERROR_HANDLE)) && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        if (unset(STD_OUTPUT_HANDLE))
            std::freopen("CONOUT$", "w", stdout);
        if (unset(STD_ERROR_HANDLE))
            std::freopen("CONOUT$", "w", stderr);
    }
#endif
    g_set_prgname("ifritview");
    g_set_application_name("IfritView");
    if (argc > 1 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0))
    {
        std::fputs(help_text(), stdout);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0)
    {
        std::printf("IfritView %s\n", IFRIT_VERSION);
        return 0;
    }
    if (argc > 3 && std::strcmp(argv[1], "--convert") == 0)
        return convert(argc - 2, argv + 2);
    if (argc > 1 && std::strcmp(argv[1], "--plugins") == 0)
        return plugins::print_report();
    if (argc > 2 && std::strcmp(argv[1], "--probe") == 0)
        return probe(argc - 2, argv + 2);
    if (argc > 2 && std::strcmp(argv[1], "--lenses") == 0)
        return lenses::report(argc - 2, argv + 2);
    if (argc > 1 && std::strcmp(argv[1], "--process") == 0)
        return process(argc - 2, argv + 2);
    if (argc > 1 && std::strcmp(argv[1], "--formats") == 0)
        return formats_report();

    // The language first: the toolkit takes it as it starts.
    start_translations(saved_language());
    platform::trace("translations loaded");

    rap::load_options options;
#ifdef IFRIT_PLUGIN_DIR
    // The toolkit plugins: beside the exe in a build tree, and in
    // ../lib/.../ifritview once installed.
    const std::filesystem::path exe_dir = rap::sys::executable_path().parent_path();
    options.search_paths = {rap::sys::path_to_utf8(exe_dir / "lib" / "ifritview"),
                            rap::sys::path_to_utf8(exe_dir / ".." / IFRIT_PLUGIN_DIR)};
#endif
    // The settings are read, and the image named on the command line
    // decoded, while the toolkit loads and the window is built.
    std::shared_future<Config> settings = App::read_settings();
    std::unique_ptr<FirstImage> first;
    if (argc == 2 && argv[1][0] != '-' && g_file_test(argv[1], G_FILE_TEST_IS_REGULAR))
        first = std::make_unique<FirstImage>(argv[1], settings);
    platform::trace("loading the toolkit");
    auto ui = rap::ui::load(argc, argv, options);
    platform::trace("toolkit loaded (%s)", ui ? std::string((*ui)->backend_name()).c_str() : "none");
    if (!ui)
    {
        std::fprintf(stderr, "%s\n", ui.error().c_str());
        return 1;
    }
    // Results from worker threads (lookups, video frames) come back through
    // the toolkit's loop; with Win32 there is no GLib loop to carry them.
    rap::ui& toolkit = **ui;
    platform::set_main_dispatch([&toolkit](std::function<void()> fn) { toolkit.post(std::move(fn)); });
    const int rc = run_app(**ui, argc, argv, t_start, first.get(), settings);
    platform::set_main_dispatch([](std::function<void()>) {});  // late arrivals after the toolkit goes
    return rc;
}

int run_app(rap::ui& ui, int argc, char** argv, std::chrono::steady_clock::time_point t_start, FirstImage* first,
            std::shared_future<Config> settings)
{
    App app(ui, std::move(settings));
    platform::trace("window built");
    if (argc > 3 && std::strcmp(argv[1], "--diff") == 0)
    {
        // --diff A B [--labels "A's name" "B's name"]
        const bool labelled = argc > 6 && std::strcmp(argv[4], "--labels") == 0;
        app.open_diff(argv[2], argv[3], labelled ? argv[5] : "", labelled ? argv[6] : "");
    }
    else if (argc > 1)
        app.open_path(argv[1], first);
    platform::trace("first image requested");
    app.show_window();
    if (g_getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "startup: %.0f ms to window creation (%s)\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count(), std::string(ui.backend_name()).c_str());

    rap::subscription script;
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
