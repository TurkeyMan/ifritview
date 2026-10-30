// The G panel's develop controls: white balance, exposure, and Lightroom's
// (adjust.h) by its names and ranges, in its order; the image's settings
// they show and set; "As shot"; and its XMP sidecar written, on the button
// or (the setting) a moment after each change. Part of App (app.h).
#include "i18n.h"
#include "app.h"
#include "decoders/decoder.h"
#include "platform.h"
#include "view.h"

#include <algorithm>
#include <cstdio>

namespace
{

// A slider: its crs: name, label, range and what it does (after Adobe's
// descriptions of each).
struct KnobSpec
{
    const char* crs;
    const char* label;
    double lo, hi;
    const char* tip;
};

// In the panel's order. The labels and tips are marked (N_) and
// translated where they're shown.
const KnobSpec kKnobs[] = {
    // Basic, Process 2012 and later.
    {"Contrast2012", N_("Contrast"), -100, 100, N_("Contrast: an S-curve about mid-grey, the darks darker and the lights lighter (or, below 0, closer)")},
    {"Highlights2012", N_("Highlights"), -100, 100, N_("Highlights: the bright tones; below 0 brings back what's near or past white, the darks left as they are")},
    {"Shadows2012", N_("Shadows"), -100, 100, N_("Shadows: the dark tones; above 0 opens them up, the lights left as they are")},
    {"Whites2012", N_("Whites"), -100, 100, N_("Whites: where white clips; above 0 more of the brightest goes to white, below 0 less")},
    {"Blacks2012", N_("Blacks"), -100, 100, N_("Blacks: where black clips; below 0 more of the darkest goes to black, above 0 less")},
    // Basic, Process 2010 (Lightroom 3, and 2003's).
    {"Brightness", N_("Brightness"), -150, 150, N_("Brightness (Process 2010): the middle tones, white and black kept")},
    {"Contrast", N_("Contrast"), -50, 100, N_("Contrast (Process 2010): an S-curve about mid-grey")},
    {"HighlightRecovery", N_("Recovery"), 0, 100, N_("Recovery (Process 2010): the brightest brought back from white")},
    {"FillLight", N_("Fill Light"), 0, 100, N_("Fill Light (Process 2010): the shadows lifted")},
    {"Shadows", N_("Blacks"), 0, 100, N_("Blacks (Process 2010): how much of the darkest goes to black")},
    // Presence.
    {"Vibrance", N_("Vibrance"), -100, 100, N_("Vibrance: saturation, most for the least saturated colours, skin tones spared")},
    {"Saturation", N_("Saturation"), -100, 100, N_("Saturation: every colour alike; -100 is greyscale")},
    // The parametric tone curve.
    {"ParametricHighlights", N_("Highlights"), -100, 100, N_("The tone curve's brightest region")},
    {"ParametricLights", N_("Lights"), -100, 100, N_("The tone curve's region from the middle to the highlights")},
    {"ParametricDarks", N_("Darks"), -100, 100, N_("The tone curve's region from the shadows to the middle")},
    {"ParametricShadows", N_("Shadows"), -100, 100, N_("The tone curve's darkest region")},
    {"ParametricShadowSplit", N_("Shadow split"), 10, 70, N_("Where the curve's shadows end and its darks begin (percent)")},
    {"ParametricMidtoneSplit", N_("Midtone split"), 20, 80, N_("Where the curve's darks end and its lights begin (percent)")},
    {"ParametricHighlightSplit", N_("Highlight split"), 30, 90, N_("Where the curve's lights end and its highlights begin (percent)")},
};
constexpr int kBasic2012 = 0, kBasic2010 = 5, kPresence = 10, kParametric = 12, kKnobCount = int(std::size(kKnobs));
// HSL's: after them, hue then saturation then luminance, a band each.
const char* const kBands[8] = {N_("Red"), N_("Orange"), N_("Yellow"), N_("Green"), N_("Aqua"), N_("Blue"), N_("Purple"), N_("Magenta")};
const char* const kBandNames[8] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
const char* const kHslKinds[3] = {"HueAdjustment", "SaturationAdjustment", "LuminanceAdjustment"};

// The point curve's presets, as ToneCurveName(2012) names them.
const char* const kCurves[] = {"Linear", "Medium Contrast", "Strong Contrast"};
const char* const kCurveLabels[] = {N_("Linear"), N_("Medium Contrast"), N_("Strong Contrast"), N_("Custom")};

// The value a setting rests at: 0, the splits at their defaults.
double rest(const std::string& crs)
{
    return crs == "ParametricShadowSplit" ? 25 : crs == "ParametricMidtoneSplit" ? 50 : crs == "ParametricHighlightSplit" ? 75 : 0;
}

double* field(Adjustments& a, const std::string& crs)
{
    for (const CrsField& f : crs_fields(a))
        if (f.name == crs)
            return f.value;
    return nullptr;
}

}  // namespace

rap::node App::develop_panel()
{
    namespace L = rap::layout;
    knobs_.clear();
    knobs_.resize(size_t(kKnobCount) + 24);
    for (int i = 0; i < kKnobCount; ++i)
        knobs_[size_t(i)].crs = kKnobs[i].crs;
    for (int kind = 0; kind < 3; ++kind)
        for (int b = 0; b < 8; ++b)
            knobs_[size_t(kKnobCount + kind * 8 + b)].crs = std::string(kHslKinds[kind]) + kBandNames[b];
    const auto knob = [this](int i) -> L::form_row
    {
        const KnobSpec& k = kKnobs[i];
        const std::string crs = k.crs;
        return {_(k.label), L::slider{k.lo, k.hi, rest(crs)}
                                .step(1, 10)
                                .show_value(0)
                                .stretch()
                                .mark(rest(crs))
                                .on_changed([this, crs](double v) { knob_changed(crs, v); })
                                .bind(knobs_[size_t(i)].slider)
                                .tooltip(_(k.tip))};
    };
    const auto band = [this](int kind, int b) -> L::form_row
    {
        const size_t i = size_t(kKnobCount + kind * 8 + b);
        const std::string crs = knobs_[i].crs;
        return {_(kBands[b]), L::slider{-100.0, 100.0, 0.0}
                                  .step(1, 10)
                                  .show_value(0)
                                  .stretch()
                                  .mark(0.0)
                                  .on_changed([this, crs](double v) { knob_changed(crs, v); })
                                  .bind(knobs_[i].slider)};
    };
    const auto hsl = [&](int kind) -> rap::node
    {
        return L::form{band(kind, 0), band(kind, 1), band(kind, 2), band(kind, 3),
                       band(kind, 4), band(kind, 5), band(kind, 6), band(kind, 7)};
    };
    std::vector<std::string> curves;
    for (const char* c : kCurveLabels)
        curves.push_back(_(c));
    using L::vbox;
    return vbox{
        // White balance (raws the GPU develops), then exposure round the
        // camera's compensation (raw and HDR images), as Lightroom has them.
        vbox{
            L::form{
                {_("Temperature"), L::slider{2000.0, 12000.0, 5500.0}
                                       .step(50, 500)
                                       .show_value(0)
                                       .stretch()
                                       .on_changed([this](double) { white_balance_changed(); })
                                       .bind(wb_kelvin_)
                                       .tooltip(_("White balance: the colour temperature of the light (kelvin)"))},
                {_("Tint"), L::slider{-150.0, 150.0, 0.0}
                                .step(1, 10)
                                .show_value(0)
                                .stretch()
                                .mark(0.0)
                                .on_changed([this](double) { white_balance_changed(); })
                                .bind(wb_tint_)
                                .tooltip(_("White balance: towards green (-) or magenta (+)"))},
            },
        }
            .handle(wb_box_),
        L::form{
            {_("Exposure"), L::slider{-3.0, 3.0, 0.0}
                                .step(0.1, 1.0)
                                .show_value(2)
                                .stretch()
                                .mark(0.0)
                                .on_changed([this](double) { retone(); })
                                .bind(exposure_)
                                .tooltip(_("Exposure (EV). Rests at the camera's compensation, i.e. as shot.\n"
                                           "Greyed while the embedded preview stands in for the raw"))},
        }
            .handle(exposure_box_),
        vbox{
            L::tabs{
                {_("Basic"),
                 vbox{
                     L::form{knob(kBasic2012), knob(kBasic2012 + 1), knob(kBasic2012 + 2), knob(kBasic2012 + 3), knob(kBasic2012 + 4)}
                         .handle(basic2012_),
                     L::form{knob(kBasic2010), knob(kBasic2010 + 1), knob(kBasic2010 + 2), knob(kBasic2010 + 3), knob(kBasic2010 + 4)}
                         .tooltip(_("Lightroom 3's controls (Process 2010), as this image's settings have them"))
                         .handle(basic2010_),
                     L::form{knob(kPresence), knob(kPresence + 1)},
                 }},
                {_("Tone Curve"),
                 vbox{
                     L::form{knob(kParametric), knob(kParametric + 1), knob(kParametric + 2), knob(kParametric + 3)},
                     L::form{knob(kParametric + 4), knob(kParametric + 5), knob(kParametric + 6),
                             {_("Point curve"), L::drop_down{curves, 0}
                                                    .on_changed([this](int i) { curve_chosen(i); })
                                                    .bind(curve_preset_)
                                                    .tooltip(_("The point curve over the parametric one: a preset, or the image's own (Custom)"))}},
                 }},
                {_("HSL"), L::tabs{{_("Hue"), hsl(0)}, {_("Saturation"), hsl(1)}, {_("Luminance"), hsl(2)}}},
            },
            L::form{{"", L::hbox{L::button{_("As shot")}
                                     .on_click([this] { white_balance_as_shot(); })
                                     .tooltip(_("Back to how the camera took it: its white balance and exposure, every control at rest")),
                                 L::button{_("Save XMP")}
                                     .on_click([this] { save_xmp(false); })
                                     .tooltip(_("Write these settings to the image's XMP sidecar (photo.xmp beside photo.CR3), "
                                                "as Lightroom reads them; what else is in it is kept"))}},
                    {"", L::check{_("Auto-save XMP"), cfg_.auto_save_xmp}
                             .on_toggled([this](bool on) { set_auto_save_xmp(on); })
                             .tooltip(_("Write the image's XMP sidecar a moment after each change"))}},
        }
            .handle(develop_box_),
        L::form{{"", L::check{_("Camera's JPEG")}
                         .on_toggled([this](bool on) { show_instead(on ? Instead::CameraJpeg : Instead::Nothing); })
                         .bind(camera_jpeg_check_)
                         .tooltip(_("Show the camera's own rendering, the JPEG embedded in the raw, instead of the raw as developed here"))},
                {"", L::check{_("LibRaw's develop")}
                         .on_toggled([this](bool on) { show_instead(on ? Instead::LibRaw : Instead::Nothing); })
                         .bind(libraw_check_)
                         .tooltip(_("Show the raw as LibRaw develops it, on the CPU (slower), instead of as developed here on the GPU"))},
                {"", L::check{_("Lens correction"), cfg_.lens_correction}
                         .on_toggled([this](bool on) { set_lens_correction(on, true); })
                         .bind(lens_check_)
                         .tooltip(_("Take out the lens's distortion, colour fringes and vignetting, from lensfun's database, "
                                    "for raws that carry no corrections of their own"))}},
    };
}

// The sliders to the image's settings: its process's basic controls, the rest.
void App::sync_knobs()
{
    const bool shown = image_ && !image_->is_video && !view_->comparing();
    develop_box_->set_visible(shown);
    if (!shown)
        return;
    Adjustments& a = image_->adjust;
    syncing_knobs_ = true;
    basic2012_->set_visible(!a.legacy);
    basic2010_->set_visible(a.legacy);
    for (Knob& k : knobs_)
        if (const double* v = field(a, k.crs); v && k.slider)
            k.slider->set_value(*v);
    int curve = 3;  // custom: the image's own points
    if (a.curve.empty() || a.curve == Adjustments::Curve{{0, 0}, {255, 255}})
    {
        curve = 0;
        for (int i = 0; i < 3; ++i)
            if (a.curve_name == kCurves[i])
                curve = i;
    }
    curve_preset_->set_current(curve);
    syncing_knobs_ = false;
}

void App::knob_changed(const std::string& crs, double value)
{
    if (syncing_knobs_ || !image_)
        return;
    double* v = field(image_->adjust, crs);
    if (!v || *v == value)
        return;
    *v = value;
    adjustments_changed();
}

void App::curve_chosen(int i)
{
    if (syncing_knobs_ || !image_ || i < 0)
        return;
    Adjustments& a = image_->adjust;
    if (i >= 3)
        return;  // Custom: the image's own points, which only XMP brings
    a.curve.clear();  // the preset by name (adjust.cpp has their points)
    a.curve_name = kCurves[i];
    adjustments_changed();
}

// The view shows them; pixels made from the image's own data are made again
// when something needs them (pixels_for_cpu); the sidecar follows.
void App::adjustments_changed()
{
    if (!image_)
        return;
    if (image_->linear || image_->source || image_->mosaic)
    {
        if (view_->drawn_on_gpu() || image_->mosaic)
        {
            image_->pixels_stale = true;
            PixelBuffer().swap(image_->pixels);
            image_->mips.clear();
        }
        else
            request_retone();
    }
    view_->redraw();
    schedule_xmp();
}

// Auto-saving: the sidecar written once the settings have rested a moment.
void App::schedule_xmp()
{
    if (!cfg_.auto_save_xmp || !image_ || image_path_.empty())
        return;
    xmp_pending_ = image_;
    xmp_pending_path_ = image_path_;
    xmp_timer_ = ui_.timer(1500, false, [this] { flush_xmp(); });
}

void App::set_auto_save_xmp(bool on)
{
    cfg_.auto_save_xmp = on;
    cfg_.save();
    if (!on)
        flush_xmp();  // what was waiting still goes
}

// The image's settings, as XMP has them.
static XmpDevelop develop_of(const Image& image)
{
    XmpDevelop d = image.xmp;
    d.exposure = image.exposure_edit;
    d.temperature = image.wb_kelvin;
    d.tint = image.wb_tint;
    d.adjust = image.adjust;
    return d;
}

void App::save_xmp(bool quiet)
{
    if (!image_ || image_path_.empty())
        return;
    const XmpDevelop d = develop_of(*image_);
    std::string error;
    if (write_xmp_sidecar(image_path_, d, error))
    {
        image_->xmp = d;  // what coming back to it opens with
        image_->xmp.present = true;
        if (!quiet)
            status_message_ = rap::i18n::format(_("Saved the develop settings to {0}"), xmp_sidecar_path(image_path_));
    }
    else
        status_message_ = rap::i18n::format(_("Couldn't save the develop settings: {0}"), error);
    update_status();
}

// A change waiting to be written: now (the image is being left, or the
// app is closing), for the image it was made to.
void App::flush_xmp()
{
    xmp_timer_ = {};
    const std::shared_ptr<Image> image = xmp_pending_.lock();
    xmp_pending_.reset();
    if (!image)
        return;
    const XmpDevelop d = develop_of(*image);
    std::string error;
    if (write_xmp_sidecar(xmp_pending_path_, d, error))
    {
        image->xmp = d;
        image->xmp.present = true;
    }
    else if (!error.empty())
    {
        status_message_ = rap::i18n::format(_("Couldn't save the develop settings: {0}"), error);
        update_status();
    }
}

void App::script_crs(const std::string& setting)
{
    const size_t eq = setting.find('=');
    if (!image_ || eq == std::string::npos)
        return;
    const std::string name = setting.substr(0, eq);
    const double value = std::strtod(setting.c_str() + eq + 1, nullptr);
    if (name == "ProcessVersion")
        image_->adjust.legacy = value < 6;
    else if (double* v = field(image_->adjust, name))
        *v = value;
    else
    {
        std::fprintf(stderr, "script: no develop setting %s\n", name.c_str());
        return;
    }
    sync_knobs();
    adjustments_changed();
}
