#include "app.h"
#include "platform.h"
#include "first_image.h"
#include "folder_index.h"
#include "pin_thumbs.h"
#include "gpu_view.h"
#include "decoders/decoder.h"
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

#include <algorithm>
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
// G panel's checks; crs:Contrast2012=40 sets a develop setting by its XMP
// name, as-shot puts them all back, save-xmp writes the sidecar).
// forget-locations does what the Maps settings' button does; maps-off turns
// both maps off as Settings would.
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
    else if (step == "paste")
        app.paste_from_clipboard();
    else if (step.rfind("drop:", 0) == 0)
        app.on_dropped({step.substr(5)});
    else if (step == "recursive:on" || step == "recursive:off")
        app.set_recursive(step == "recursive:on");
    else if (step.rfind("drag-pane-native-fast:", 0) == 0)
    {
        // "drag-pane-native-fast:-150,300,8": 150 px left over 300 ms, the pointer moving every 8 ms
        float by = 0;
        int ms = 300, period = 8;
        std::sscanf(step.c_str() + 22, "%f,%d,%d", &by, &ms, &period);
        app.script_drag_pane_native_fast(by, ms, std::max(1, period));
    }
    else if (step.rfind("drag-pane-native:", 0) == 0)
        app.script_drag_pane_native(std::stof(step.substr(17)));  // as drag-pane, through the splitter's mouse handling
    else if (step.rfind("drag-pane:", 0) == 0)
        app.script_drag_pane(std::stof(step.substr(10)));  // "drag-pane:-200": the divider 200 px left
    else if (step.rfind("drag-map:", 0) == 0)
        app.script_drag_map(std::stof(step.substr(9)));  // "drag-map:-100": the map's divider 100 px up, the map taller
    else if (step.rfind("resize:", 0) == 0)
    {
        float w = 0, h = 0;  // "resize:1200,900": the window's content, logical px
        if (std::sscanf(step.c_str() + 7, "%f,%f", &w, &h) == 2)
            app.script_resize({w, h});
    }
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
    else if (step == "forget-locations")
        app.script_forget_locations();
    else if (step == "maps-off")
        app.script_maps_off();
    else if (step.rfind("export:", 0) == 0)
        app.script_export(step.substr(7));
    else if (step.rfind("crs:", 0) == 0)
        app.script_crs(step.substr(4));
    else if (step == "save-xmp")
        app.script_save_xmp();
    else if (step == "as-shot")
        app.script_as_shot();
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

// --gpu-render IN OUT.ppm ZOOM [nearest|area|mitchell|catmullrom|lanczos3]:
// IN drawn as the window's GPU view draws it, at ZOOM, on a headless device
// (lavapipe will do), into OUT; with how much of it isn't the background.
// The view's GPU path checked without a window.
static int gpu_render(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "--gpu-render IN OUT.ppm ZOOM [FILTER]\n");
        return 2;
    }
    auto image = std::make_shared<Image>();
    std::string error;
    if (!decode_image(argv[0], *image, error))
    {
        std::fprintf(stderr, "%s: %s\n", argv[0], error.c_str());
        return 1;
    }
    auto device = rap::gpu::device::headless();
    if (!device)
    {
        std::fprintf(stderr, "no GPU device\n");
        return 1;
    }
    GpuView view(device);
    GpuView::Frame f;
    f.zoom = std::atof(argv[2]);
    f.width = std::max(1, int(std::ceil(image->width * f.zoom)));
    f.height = std::max(1, int(std::ceil(image->height * f.zoom)));
    f.background = rap::color{38, 38, 38};
    if (argc > 3)
    {
        static const char* const names[] = {"nearest", "area", "mitchell", "catmullrom", "lanczos3"};
        for (int i = 0; i < 5; ++i)
            if (!std::strcmp(argv[3], names[i]))
                f.shrink = f.enlarge = ScaleFilter(i);
    }
    rap::gpu::texture_ptr out;
    rap::gpu::ticket ready;
    rap::frame_color colour;
    if (!view.render(image, 1, f, out, ready, colour, error))
    {
        std::fprintf(stderr, "render: %s\n", error.c_str());
        return 1;
    }
    const int tw = out->width(), th = out->height();
    std::vector<std::byte> px(size_t(tw) * th * 4);
    device->readback(out, px, size_t(tw) * 4).wait();
    if (device->lost())  // before the output's opened: a failed readback leaves px empty
    {
        std::fprintf(stderr, "readback: the GPU was lost\n");
        return 1;
    }
    size_t drawn = 0;
    std::FILE* file = std::fopen(argv[1], "wb");
    if (file)
        std::fprintf(file, "P6\n%d %d\n255\n", f.width, f.height);
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x)
        {
            const std::byte* p = &px[(size_t(y) * tw + x) * 4];
            const int r = std::to_integer<int>(p[0]), g = std::to_integer<int>(p[1]), b = std::to_integer<int>(p[2]);
            drawn += std::abs(r - 38) + std::abs(g - 38) + std::abs(b - 38) > 6;
            if (file)
                std::fputc(r, file), std::fputc(g, file), std::fputc(b, file);
        }
    if (file)
        std::fclose(file);
    // The first pass's rows, where the filtered path drew them: their range.
    if (const rap::gpu::texture_ptr& rows = view.first_pass())
    {
        const int rw = rows->width(), rh = rows->height();
        std::vector<uint16_t> half(size_t(rw) * rh * 4);
        device->readback(rows, std::as_writable_bytes(std::span(half)), size_t(rw) * 8).wait();
        if (device->lost())
        {
            std::fprintf(stderr, "first pass rows: the GPU was lost reading them back\n");
            return 1;
        }
        const auto to_float = [](uint16_t h)
        {
            const int e = h >> 10 & 31, m = h & 1023;
            const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 1024), e - 25);
            return h >> 15 ? -v : v;
        };
        float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
        size_t nan = 0;
        for (size_t i = 0; i < half.size(); ++i)
        {
            const float v = to_float(half[i]);
            if (v != v)
                ++nan;
            else
                lo[i % 4] = std::min(lo[i % 4], v), hi[i % 4] = std::max(hi[i % 4], v);
        }
        std::printf("first pass rows %dx%d: r %g..%g g %g..%g b %g..%g a %g..%g, %zu NaN\n", rw, rh, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                    lo[3], hi[3], nan);
    }
    std::printf("%s on %s%s: %dx%d at zoom %g, %zu of %zu pixels not the background\n", argv[0], std::string(device->api()).c_str(),
                device->software() ? " (software)" : "", f.width, f.height, f.zoom, drawn, size_t(f.width) * f.height);
    return 0;
}

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
    if (argc > 1 && std::strcmp(argv[1], "--gpu-render") == 0)
        return gpu_render(argc - 2, argv + 2);
    if (argc > 2 && std::strcmp(argv[1], "--check-forget") == 0)
    {
        const int index = check_index_forget(argv[2]);
        const int thumbs = check_thumbs_forget(argv[2]);
        return index | thumbs;
    }

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
                                  app.script_settle();
                                  if (next < steps.size())
                                      run_step(app, steps[next++]);
                              });
    }
    return app.run();
}
