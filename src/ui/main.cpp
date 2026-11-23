#include "app.h"
#include "app_util.h"
#include "platform.h"
#include "first_image.h"
#include "folder_index.h"
#include "pin_thumbs.h"
#include "gpu_view.h"
#include "decoders/decoder.h"
#include "exposure.h"
#include "i18n.h"
#include "lens_store.h"
#include "pipeline.h"
#include "plugins.h"
#include "look.h"
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

int check_brightness();  // check_brightness.cpp

int run_app(rap::ui& ui, int argc, char** argv, std::chrono::steady_clock::time_point t_start, FirstImage* first,
            std::shared_future<Config> settings);

// --check-exposure: a raw's automatic exposure (shaders/exposure.hlsli,
// the shader's own code): the luminance at white from ISO, shutter and
// aperture; the display key at and past the mesopic limits and between; the
// gain from it, and without the EXIF to say how light the scene was.
static int check_exposure()
{
    using namespace exposure;
    int failed = 0;
    const auto expect = [&](const char* what, double got, double want)
    {
        const bool ok = std::abs(got - want) <= 1e-4 * std::max(1.0, std::abs(want));
        std::printf("%s %s: %.6g (want %.6g)\n", ok ? "ok  " : "FAIL", what, got, want);
        failed += !ok;
    };
    // ISO 12800, 1/40 s, f/4: 78 x 16 x 40 / (0.65 x 12800) = 6 cd/m^2.
    expect("white at ISO 12800, 1/40 s, f/4", saturation_luminance(12800, 1.0f / 40, 4), 6);
    expect("white without an ISO", saturation_luminance(0, 1.0f / 40, 4), 0);
    expect("white without a shutter", saturation_luminance(12800, 0, 4), 0);
    expect("white without an aperture", saturation_luminance(12800, 1.0f / 40, 0), 0);
    // Which rating the ISO is (SensitivityType, 0x8830), for the trace:
    // every one is the nominal ISO, an approximation of S_sat.
    const auto speed = [&](const char* what, Sensitivity s, Speed kind)
    {
        expect(what, s.iso, 12800);
        const bool ok = s.speed == kind;
        std::printf("%s %s: %s\n", ok ? "ok  " : "FAIL", what, speed_name(s.speed));
        failed += !ok;
    };
    speed("speed, ISO speed (type 3)", scene_sensitivity(3, 12800), Speed::IsoSpeed);
    speed("speed, ISO speed with SOS and REI (type 7)", scene_sensitivity(7, 12800), Speed::IsoSpeed);
    speed("speed, SOS (type 1)", scene_sensitivity(1, 12800), Speed::Sos);
    speed("speed, REI (type 2)", scene_sensitivity(2, 12800), Speed::Rei);
    speed("speed, SOS and REI (type 4)", scene_sensitivity(4, 12800), Speed::SosRei);
    speed("speed, not stated", scene_sensitivity(-1, 12800), Speed::Unstated);
    // No camera has a cited correction yet, whatever its rating: the nominal ISO stands.
    expect("white, REI, no correction", raw_white_luminance(scene_sensitivity(2, 12800), 1.0f / 40, 4, "Canon", "EOS 5D Mark III"), 6);
    expect("white, ISO speed, no correction", raw_white_luminance(scene_sensitivity(3, 12800), 1.0f / 40, 4, "Canon", "EOS 5D Mark III"), 6);
    // L_A on the stated white's scale (the develop scales its values by the
    // white it clips at): a raw whose only clue is its largest value at 0.8
    // of the stated white, log average 0.04 of that, 100 cd/m2 at the stated
    // white, is at 3.2 cd/m2, not 4: its gain 2.51, not 3.36.
    const float sparse = adapting_luminance(0.04f, 0.8f, 100);
    expect("L_A, develop white 0.8", sparse, 3.2);
    expect("gain, develop white 0.8", exposure_gain(2, 0.04f, 0.2f, display_key(sparse)), display_key(3.2f) / 0.04f);
    expect("gain, develop white 0.8, not its own", exposure_gain(2, 0.04f, 0.2f, display_key(adapting_luminance(0.04f, 1, 100))) > 3.3f, 1);
    // A treated raw (a Lightroom develop): the camera's exposure, with no
    // lift toward its key, nor any fitting of its highlights; untreated, the model.
    const float lifted = exposure_gain(2, 0.02f, 0.1f, display_key(adapting_luminance(0.02f, 1, 600)));
    const float fitted = exposure_gain(2, 0.07f, 0.8f, display_key(adapting_luminance(0.07f, 1, 600)));
    expect("treated: no lift toward the key", shown_gain(true, 2, lifted), 2);
    expect("treated: no fitting of the highlights", shown_gain(true, 2, fitted), 2);
    expect("treated: no departure without EXIF either", shown_gain(true, 2, exposure_gain(2, 0.0013f, 0.02f, display_key(0))), 2);
    expect("untreated: the model's lift", shown_gain(false, 2, lifted), 4);
    expect("untreated: the model's fitting", shown_gain(false, 2, fitted), 1.25);
    const double night = kMiddleGrey * std::exp2(-kNightDepth);
    expect("key, photopic limit (5 cd/m2)", display_key(kPhotopic), kMiddleGrey);
    expect("key, daylight (5000 cd/m2)", display_key(5000), kMiddleGrey);
    expect("key, scotopic limit (0.005 cd/m2)", display_key(kScotopic), night);
    expect("key, starlight (0.00001 cd/m2)", display_key(1e-5f), night);
    expect("key, unknown light", display_key(0), kMiddleGrey);
    const float middle = std::sqrt(kPhotopic * kScotopic);  // halfway down the range in log luminance
    expect("key, mesopic middle (0.158 cd/m2)", display_key(middle), kMiddleGrey * std::exp2(-kNightDepth / 2));
    // Smooth: no step at either limit, and darker all the way down.
    expect("key, just under the photopic limit", std::log2(display_key(kPhotopic * 0.999999f) / kMiddleGrey), 0);
    expect("key, just over the scotopic limit", std::log2(display_key(kScotopic * 1.000001f) / night), 0);
    bool falling = true;
    for (float l = 10; l > 0.001f; l *= 0.9f)
        falling &= display_key(l * 0.9f) <= display_key(l);
    expect("key falls with the light", falling, 1);
    // The gain (anchor 2): a dark scene (log average 0.13% of white, its 99th
    // percentile 2%) whose light is known to be night stays at the camera's
    // exposure; the same without the EXIF is lifted the stop the rule allows.
    const float anchor = 2, average = 0.0013f, p99 = 0.02f;
    expect("gain, night by its light (white 6 cd/m2)", exposure_gain(anchor, average, p99, display_key(adapting_luminance(average, 1, 6))), anchor);
    expect("gain, no EXIF: the camera's, a stop up", exposure_gain(anchor, average, p99, display_key(adapting_luminance(average, 1, 0))), 2 * anchor);
    // Daylight: lifted toward middle grey, a stop at most; held down to fit
    // its 99th percentile, a stop at most; never lowered for its key alone.
    expect("gain, daylight lifted to middle grey", exposure_gain(anchor, 0.07f, 0.3f, display_key(0.07f * 600)), kMiddleGrey / 0.07f);
    expect("gain, daylight lifted a stop at most", exposure_gain(anchor, 0.02f, 0.1f, display_key(0.02f * 600)), 2 * anchor);
    expect("gain, highlights fitted", exposure_gain(anchor, 0.07f, 0.8f, display_key(0.07f * 600)), 1 / 0.8f);
    expect("gain, highlights fitted a stop at most", exposure_gain(anchor, 0.07f, 4, display_key(0.07f * 600)), anchor / 2);
    expect("gain, a bright scene kept at the camera's", exposure_gain(anchor, 0.3f, 0.45f, display_key(0.3f * 600)), anchor);
    std::printf("%s\n", failed ? "exposure check FAILED" : "exposure check passed");
    return failed ? 1 : 0;
}

// --gpu-render IN OUT.ppm ZOOM [nearest|area|mitchell|catmullrom|lanczos3]:
// IN drawn as the window's GPU view draws it, at ZOOM, on a headless device
// (lavapipe will do), into OUT; with how much of it isn't the background.
// The view's GPU path checked without a window: a raw decoded as the
// window's default settings have it (full size, developed on the GPU),
// in the look IFRIT_LOOK names (look.h: neutral, camera, enhanced,
// monochrome), Neutral without.
static int gpu_render(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "--gpu-render IN OUT.ppm ZOOM [FILTER]\n");
        return 2;
    }
    set_decode_options(decode_options_for(Config{}));
    auto image = std::make_shared<Image>();
    std::string error;
    if (!decode_image(argv[0], *image, error))
    {
        std::fprintf(stderr, "%s: %s\n", argv[0], error.c_str());
        return 1;
    }
    if (const char* look = g_getenv("IFRIT_LOOK"))
    {
        if (const std::optional<Look> l = look_named(look); l && image->is_raw)
            image->adjust.look = *l;
        else
            std::fprintf(stderr, "IFRIT_LOOK: %s isn't a look, or %s isn't a raw\n", look, argv[0]);
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
    GpuView::edit_of(*image, f);  // its develop: the look, every control at rest
    if (argc > 3)
    {
        static const char* const names[] = {"nearest", "area", "mitchell", "catmullrom", "lanczos3"};
        for (int i = 0; i < 5; ++i)
            if (!std::strcmp(argv[3], names[i]))
                f.shrink = f.enlarge = ScaleFilter(i);
    }
    // A test hook: OUT ending .pfm is written linear (scRGB, 1 = SDR white),
    // little-endian float, so what's over SDR white (IFRIT_DISPLAY_PEAK)
    // survives.
    const std::string out_name = argv[1];
    const bool pfm = out_name.size() > 4 && out_name.compare(out_name.size() - 4, 4, ".pfm") == 0;
    if (pfm)
        f.front = FrontBuffer::ScRgb16;
    rap::gpu::texture_ptr out;
    rap::gpu::ticket ready;
    rap::frame_color colour;
    if (!view.render(image, 1, f, out, ready, colour, error))
    {
        std::fprintf(stderr, "render: %s\n", error.c_str());
        return 1;
    }
    // A test hook: IFRIT_REDRAW drawn again with a change, what's written the
    // second: "wb:R,G,B" (a raw's white balance multipliers) or "detail" (a
    // raw's default noise reduction and sharpening). With IFRIT_CURVE_LOG,
    // whether the brightness curve follows it.
    if (const char* redraw = std::getenv("IFRIT_REDRAW"))
    {
        const std::string change = redraw;
        if (change.rfind("wb:", 0) == 0)
        {
            float wb[3] = {};
            if (std::sscanf(change.c_str() + 3, "%f,%f,%f", &wb[0], &wb[1], &wb[2]) == 3)
                std::copy(wb, wb + 3, f.white_balance);
        }
        else if (change == "detail")
            f.detail = Detail::raw_defaults();
        ready = {};
        if (!view.render(image, 1, f, out, ready, colour, error))
        {
            std::fprintf(stderr, "render: %s\n", error.c_str());
            return 1;
        }
    }
    if (pfm)
    {
        const int pw = out->width();
        std::vector<uint16_t> half(size_t(pw) * out->height() * 4);
        device->readback(out, std::as_writable_bytes(std::span(half)), size_t(pw) * 8).wait();
        if (device->lost())  // before the output's opened: a failed readback leaves `half` empty
        {
            std::fprintf(stderr, "readback: the GPU was lost\n");
            return 1;
        }
        const auto to_float = [](uint16_t h)
        {
            const int e = h >> 10 & 31, m = h & 1023;
            const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 1024), e - 25);
            return h >> 15 ? -v : v;
        };
        std::FILE* file = std::fopen(argv[1], "wb");
        if (!file)
            return 1;
        std::fprintf(file, "PF\n%d %d\n-1.0\n", f.width, f.height);
        for (int y = f.height - 1; y >= 0; --y)  // PFM: bottom row first
            for (int x = 0; x < f.width; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    const float v = to_float(half[(size_t(y) * pw + x) * 4 + c]);
                    std::fwrite(&v, 4, 1, file);
                }
        std::fclose(file);
        std::printf("%s: %dx%d linear, peak %s\n", argv[1], f.width, f.height, std::getenv("IFRIT_DISPLAY_PEAK") ? std::getenv("IFRIT_DISPLAY_PEAK") : "1");
        return 0;
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
    if (argc > 1 && std::strcmp(argv[1], "--check-brightness") == 0)
        return check_brightness();
    if (argc > 1 && std::strcmp(argv[1], "--check-exposure") == 0)
        return check_exposure();
    if (argc > 2 && std::strcmp(argv[1], "--check-loader") == 0)
        return check_loader(argv[2]);
    if (argc > 2 && std::strcmp(argv[1], "--check-looks") == 0)
        return check_looks(argv[2]);
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
