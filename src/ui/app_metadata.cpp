// The info pane and the fullscreen overlay: what's known about the image. Part of App (app.h).
#include "i18n.h"
#include "app.h"
#include "app_util.h"
#include "platform.h"
#include "plugins.h"
#include "save.h"
#include "settings.h"
#include "decoders/decoder.h"
#include "first_image.h"
#include "integration.h"
#include "jpeg_lossless.h"
#include "encoders/encoder.h"
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>

// -- metadata -----------------------------------------------------------------
void App::remember_pane_width()
{
    const std::vector<float> sizes = splitter_->sizes();
    if (!metadata_visible_ || sizes.size() < 2 || sizes[1] <= 0)
        return;
    const int width = std::max(240, int(std::lround(sizes[1])));
    if (width == cfg_.metadata_pane_width)
        return;
    if (pane_extra_ > 0)
        pane_extra_ += float(width - cfg_.metadata_pane_width);  // the divider's share is unchanged
    cfg_.metadata_pane_width = width;
    cfg_.save();
}

void App::toggle_metadata()
{
    if (metadata_visible_ && pane_histogram_)
    {
        pane_histogram_ = false;  // the pane stays: it shows the info instead
        show_pane_content();
        return;
    }
    pane_histogram_ = false;
    toggle_pane();
}

void App::toggle_histogram()
{
    if (fullscreen_)
    {
        cfg_.histogram = !cfg_.histogram;
        cfg_.save();
        update_histogram();
        return;
    }
    if (metadata_visible_ && !pane_histogram_)
    {
        pane_histogram_ = true;
        show_pane_content();
        return;
    }
    pane_histogram_ = true;
    toggle_pane();
}

namespace
{
struct LegendChip
{
    const char* letter;
    int bit;
    rap::color colour;
};
constexpr LegendChip kLegend[] = {
    {"R", 1, {235, 70, 70}}, {"G", 2, {80, 200, 90}}, {"B", 4, {90, 130, 245}}, {"L", 8, {235, 235, 235}}};
constexpr float kChip = 18, kChipGap = 4, kChipInset = 6;  // logical px

rap::rect_f chip_rect(size_t i)
{
    return {kChipInset + float(i) * (kChip + kChipGap), kChipInset, kChip, kChip};
}
}  // namespace

void App::paint_histogram_legend(rap::painter& p, rap::size_f)
{
    for (size_t i = 0; i < std::size(kLegend); ++i)
    {
        const LegendChip& c = kLegend[i];
        const bool on = (cfg_.histogram_channels & c.bit) != 0;
        const rap::rect_f r = chip_rect(i);
        p.rect(r, 3, rap::color{0, 0, 0, uint8_t(on ? 170 : 110)},
               rap::color{c.colour.r, c.colour.g, c.colour.b, uint8_t(on ? 255 : 90)});
        rap::text_style style;
        style.fg = rap::color{c.colour.r, c.colour.g, c.colour.b, uint8_t(on ? 255 : 90)};
        const rap::size_f t = p.measure(c.letter, style);
        p.text({r.x + (r.w - t.w) / 2, r.y + (r.h - t.h) / 2}, c.letter, style);
    }
}

bool App::histogram_legend_click(const rap::pointer_event& e)
{
    if (e.type != rap::pointer_event_type::press || e.button != 1)
        return false;
    for (size_t i = 0; i < std::size(kLegend); ++i)
    {
        const rap::rect_f r = chip_rect(i);
        if (e.pos.x >= r.x && e.pos.x < r.x + r.w && e.pos.y >= r.y && e.pos.y < r.y + r.h)
        {
            cfg_.histogram_channels ^= kLegend[i].bit;
            cfg_.save();
            view_->config_changed();           // the overlay
            view_->histogram_panel_changed();  // the docked panel
            histogram_canvas_->invalidate();   // the chips
            return true;
        }
    }
    return false;
}

void App::show_pane_content()
{
    show_pane_box();
    fill_pane();
}

void App::show_pane_box()
{
    info_box_->set_visible(!pane_histogram_);
    histogram_box_->set_visible(pane_histogram_);
    if (pane_histogram_ || !metadata_visible_)
    {
        map_view_->stop_fetching();
        scan_pins(false);  // which stops, being hidden
    }
}

void App::fill_pane()
{
    refresh_metadata();
    update_histogram();
}

void App::update_histogram()
{
    view_->set_histogram(fullscreen_ && cfg_.histogram, metadata_visible_ && pane_histogram_ && !fullscreen_ ? histogram_canvas_ : nullptr);
}

void App::toggle_pane()
{
    const rap::size_f before = window_->size();
    metadata_visible_ = !metadata_visible_;
    metadata_pane_->set_visible(metadata_visible_);
    if (metadata_visible_)
        splitter_->set_sizes({std::max(100.f, before.w), float(cfg_.metadata_pane_width)});
    show_pane_box();
    // The window takes (or gives back) what the pane adds in the same step,
    // so the first frame is laid out at the final width, the pane's controls
    // in it: laying the pane into the old width first showed it over the
    // image until the window grew. Before the pane's filled in: that asks
    // the histogram canvas its size, which lays the window out as it is.
    pane_toggled_from_ = 0;
    if (pane_extra_ <= 0)
        pane_extra_ = float(cfg_.metadata_pane_width) + splitter_->divider();  // until measured: the pane and its divider
    if (image_ && !fullscreen_ && cfg_.fit_window_to_image)
    {
        float w = before.w + (metadata_visible_ ? pane_extra_ : -pane_extra_);
        const rap::rect_f work = window_->work_area();
        if (work.w > 0)
            w = std::min(w, work.w - 24);
        window_->resize({std::max(w, float(cfg_.min_window_width)), before.h});
        pane_toggled_from_ = before.w;
    }
    fill_pane();
    // Then the exact fit, measured once the toolkit has laid the pane in or
    // out (it does that later, not here), or shortly if the view's size
    // didn't change.
    refit_after_layout_ = true;
    refit_timer_ = ui_.timer(150, false,
                             [this]
                             {
                                 if (refit_after_layout_)
                                     refit_after_pane_toggle();
                             });
}

void App::refit_after_pane_toggle()
{
    refit_after_layout_ = false;
    fit_window_to_image();
    // Learn exactly what the pane adds (its width and the divider), for next time.
    const float now = window_->size().w;
    const rap::rect_f work = window_->work_area();
    const bool limited = work.w > 0 && now >= work.w - 25;
    if (pane_toggled_from_ > 0 && !limited)
    {
        const float extra = metadata_visible_ ? now - pane_toggled_from_ : pane_toggled_from_ - now;
        if (extra > 0)
            pane_extra_ = extra;
    }
    pane_toggled_from_ = 0;
}

std::vector<MetadataGroup> App::collect_metadata() const
{
    std::vector<MetadataGroup> groups;
    if (!image_)
        return groups;
    MetadataGroup file{N_("File"), {}};
    file.items.push_back({N_("Name"), shown_name()});
    if (!pasted_)
        file.items.push_back({N_("Folder"), folder_.directory()});
    if (file_size_)
        file.items.push_back({N_("Size"), rap::i18n::format(P_("{0} ({1} byte)", "{0} ({1} bytes)", file_size_), human_size(file_size_), file_size_)});
    if (const std::string t = file_time(image_path_, "%Y-%m-%d %H:%M:%S"); !t.empty())
        file.items.push_back({N_("Modified"), t});
    groups.push_back(std::move(file));
    MetadataGroup img{N_("Image"), {}};
    img.items.push_back({N_("Dimensions"), std::to_string(image_->width) + " × " + std::to_string(image_->height)});
    if (!image_->format.empty())  // a pasted image has none
        img.items.push_back({N_("Format"), image_->format});
    if (!image_->pixel_format.empty())
        img.items.push_back({N_("Pixel format"), image_->pixel_format.describe(image_->width, image_->height)});
    if (!image_->color_space.empty())
        img.items.push_back({N_("Colour space"), image_->color_space});
    img.items.push_back({N_("Source depth"), rap::i18n::format(P_("{0} bit per sample", "{0} bits per sample", image_->bits_per_sample), image_->bits_per_sample)});
    img.items.push_back({N_("Alpha"), image_->has_alpha ? _("yes") : _("no")});
    if (!image_->info.empty())
        img.items.push_back({N_("Notes"), image_->info});
    groups.push_back(std::move(img));
    for (const MetadataGroup& g : image_->metadata)
        groups.push_back(g);
    if (image_->has_location)
    {
        MetadataGroup* gps = nullptr;
        for (MetadataGroup& g : groups)
            if (g.name == "GPS")
                gps = &g;
        if (!gps)
        {
            groups.push_back({"GPS", {}});
            gps = &groups.back();
        }
        if (!image_->place.empty() && image_->place != " ")
            gps->items.insert(gps->items.begin(), {N_("Place"), image_->place});
    }
    return groups;
}

// The fullscreen HUD, assembled by category from what the decoders found.
View::OverlaySections App::overlay_sections() const
{
    View::OverlaySections sections;
    if (!image_)
        return sections;
    std::vector<std::string> lines;
    auto flush = [&]
    {
        if (!lines.empty())
            sections.push_back(std::move(lines));
        lines.clear();
    };
    auto find = [&](std::initializer_list<const char*> groups, std::initializer_list<const char*> keys) -> std::string
    {
        for (const MetadataGroup& g : image_->metadata)
            for (const char* gn : groups)
                if (g.name == gn)
                    for (const char* k : keys)
                        for (const MetadataItem& m : g.items)
                            if (m.key == k)
                                return m.value;
        return "";
    };
    if (cfg_.overlay_file)
    {
        lines.push_back(folder_.current_name() + "  (" + std::to_string(folder_.index() + 1) + "/" + std::to_string(folder_.count()) + ")");
        std::string line = file_size_ ? human_size(file_size_) : "";
        if (const std::string t = file_time(image_path_, "%Y-%m-%d %H:%M"); !t.empty())
            line += std::string(line.empty() ? "" : "  ·  ") + t;
        if (!line.empty())
            lines.push_back(line);
    }
    flush();
    if (cfg_.overlay_format)
    {
        lines.push_back(std::to_string(image_->width) + " × " + std::to_string(image_->height) + "  ·  " + image_->format);
        if (!image_->pixel_format.empty())
            lines.push_back(image_->pixel_format.describe(image_->width, image_->height) + (image_->color_space.empty() ? "" : "  ·  " + image_->color_space));
        const std::string compression = find({"TIFF"}, {"Compression"});
        if (!compression.empty())
            lines.push_back(rap::i18n::format(_("compression {0}"), compression));
    }
    flush();
    if (cfg_.overlay_shot)
    {
        const std::string make = find({"Camera", "EXIF"}, {"Make", "Manufacturer"});
        const std::string model = find({"Camera", "EXIF"}, {"Model"});
        std::string camera = model.rfind(make, 0) == 0 || make.empty() ? model : make + " " + model;
        const std::string lens = find({"Camera", "EXIF"}, {"Lens", "Lens Model"});
        if (!camera.empty())
            lines.push_back(camera + (lens.empty() ? "" : "  ·  " + lens));
        std::string exp;
        auto append = [&](const std::string& v)
        {
            if (!v.empty())
                exp += (exp.empty() ? "" : "  ·  ") + v;
        };
        append(find({"Camera", "EXIF"}, {"Focal length", "Focal Length"}));
        append(find({"Camera", "EXIF"}, {"F-Number", "Aperture"}));
        append(find({"Camera", "EXIF"}, {"Shutter", "Exposure Time"}));
        const std::string iso = find({"Camera", "EXIF"}, {"ISO", "ISO Speed Ratings"});
        if (!iso.empty())
            append("ISO " + iso);
        const std::string bias = find({"Camera", "EXIF"}, {"Exposure compensation", "Exposure Bias"});
        if (!bias.empty() && bias != "0.00 EV")
            append(bias + (bias.find("EV") == std::string::npos ? " EV" : ""));
        if (!exp.empty())
            lines.push_back(exp);
        std::string taken = find({"Camera", "EXIF"}, {"Taken", "Date and Time (Original)", "Date/time original"});
        if (taken.size() >= 10 && taken[4] == ':' && taken[7] == ':')  // EXIF writes the date with colons
            taken[4] = taken[7] = '-';
        if (!taken.empty())
            lines.push_back(taken);
    }
    flush();
    if (cfg_.overlay_location && image_->has_location)
    {
        lines.push_back(image_->location_text());
        if (!image_->place.empty() && image_->place != " ")
            lines.push_back(image_->place);
    }
    flush();
    if (cfg_.overlay_view)
    {
        const double shown = (image_->final_width > 0 ? view_->zoom() * image_->width / image_->final_width : view_->zoom()) / view_->natural_zoom();
        std::string line = std::to_string(int(std::lround(shown * 100))) + "%";
        if (image_->page_count > 1)
            line += "  ·  " + rap::i18n::format(_("page {0} / {1}"), image_->part.page + 1, image_->page_count) +
                    (image_->page_label.empty() ? "" : " " + image_->page_label);
        if (image_->level_count > 1)
            line += "  ·  " + rap::i18n::format(_("mip {0} / {1}"), image_->part.level, image_->level_count - 1);
        if (image_->is_preview())
            line += std::string("  ·  ") + (image_->is_raw ? _("embedded preview") : _("preview"));
        else if (image_->tonable())
        {
            char ev[32];
            std::snprintf(ev, sizeof ev, "%+.2f EV", exposure_->value());
            line += std::string("  ·  ") + (image_->linear ? _("developed") : "HDR") + "  ·  " + ev;
        }
        lines.push_back(line);
    }
    flush();
    return sections;
}

void App::refresh_metadata()
{
    if (!metadata_visible_ || pane_histogram_)
    {
        refresh_map();
        return;
    }
    const std::string url = image_ && image_->has_location ? cfg_.map_link(image_->latitude, image_->longitude) : "";
    metadata_model_.set(collect_metadata(), image_.get(), url);
    refresh_map();
}

// The info pane's height: the details' and the map's, as laid out.
static float pane_height(const std::vector<float>& sizes)
{
    return sizes.size() == 2 ? sizes[0] + sizes[1] : 0;
}

// Until the divider's been dragged: a third of the pane, the details the other
// two, but no taller than 4:3, the details taking the rest.
float App::auto_map_height(float pane) const
{
    const float width = map_width_ > 0 ? map_width_ : float(cfg_.metadata_pane_width);
    return std::round(std::min(std::max(100.f, std::min(pane / 3, width * 3 / 4)), std::max(100.f, pane - 80)));
}

// The height the map was last dragged to, or the automatic one. As far as the
// pane allows: the details keep 80 px, the map 100.
void App::restore_map_height()
{
    const std::vector<float> sizes = map_split_->sizes();
    const float total = pane_height(sizes);
    if (total <= 0)
        return;
    const float height = cfg_.map_height > 0 ? std::round(std::min(float(cfg_.map_height), std::max(100.f, total - 80)))
                                              : auto_map_height(total);
    if (std::abs(sizes[1] - height) < 1)
        return;  // as it is (and a window resize leaves a dragged height to the layout)
    map_split_->set_sizes({total - height, height});
}

// The window changing the pane's height leaves the map the height it was
// dragged to, the details taking the difference; not yet dragged, the map's
// shaped again to the pane's new size, or its new width. (A drag of the map's
// divider changes neither: that's not this.)
void App::map_resized()
{
    if (!map_split_bound_)
        refresh_map();
    if (!map_shown_ || map_split_->sizes().size() != 2)
        return;
    const float total = pane_height(map_split_->sizes());
    const bool reshape = cfg_.map_height <= 0 && std::abs(map_width_ - map_shaped_width_) >= 1;
    if (std::abs(total - map_split_total_) < 1 && !reshape)
        return;
    map_split_total_ = total;
    map_shaped_width_ = map_width_;
    ui_.post([this] { restore_map_height(); });
}

// Only a drag of the divider keeps a height: from then on it's the user's.
// on_moved is only that, never layout or set_sizes().
void App::map_divider_moved()
{
    const std::vector<float> sizes = map_split_->sizes();
    if (sizes.size() == 2)
        save_map_height(sizes[1]);
}

void App::save_map_height(float height)
{
    if (!map_shown_ || height < 100)
        return;
    cfg_.map_height = int(std::round(height));
    cfg_.save();
}

// The map's divider dragged down by `by` px and let go, as on_moved sees it:
// the height it's laid out at is saved at the script's next tick, once
// layout has settled (GTK lays it out after set_sizes, and Windows' splitter
// reports the sizes asked for until it's placed its panes), whether or not
// the map moved.
void App::script_drag_map(float by)
{
    const std::vector<float> sizes = map_split_ ? map_split_->sizes() : std::vector<float>{};
    if (sizes.size() != 2)
        return;
    map_split_->set_sizes({sizes[0] + by, sizes[1] - by});
    script_map_dragged_ = true;
}

void App::script_settle()
{
    if (!script_map_dragged_)
        return;
    script_map_dragged_ = false;
    if (map_canvas_)
        save_map_height(map_canvas_->size().h);  // the map as laid out, not the splitter's sizes
}

void App::refresh_map()
{
    const bool located = image_ && image_->has_location && map_view_->enabled();
    const bool was_overlay = map_overlay_on_;
    map_overlay_on_ = fullscreen_ && cfg_.map_overlay && located;
    const bool pane = map_canvas_ && !fullscreen_ && located;

    if (map_canvas_ && !map_split_bound_)
    {
        map_split_bound_ = true;
        map_split_->on_moved([this] { map_divider_moved(); });
    }
    if (map_overlay_on_)
        map_view_->attach([this] { view_->redraw(); });
    else if (map_canvas_)
        map_view_->attach([canvas = map_canvas_] { canvas->invalidate(); });

    if (map_overlay_on_ || pane)
        map_view_->set_location(image_->latitude, image_->longitude, folder_.current());
    else
        map_view_->stop_fetching();
    if (map_canvas_)
    {
        map_canvas_->set_visible(pane);
        if (pane && !map_shown_)
            restore_map_height();
        map_shown_ = pane;
    }
    if (was_overlay != map_overlay_on_)
    {
        map_view_->cancel_gesture();
        view_->redraw();
    }
    scan_pins(false);
}

rap::rect_f App::map_overlay_area(rap::size_f view) const
{
    const float w = std::min(480.f, std::max(200.f, view.w * 0.3f)), h = w * 0.68f;
    return {view.w - w - 24, view.h - h - 24, w, h};
}

// M: in fullscreen the map over the image; otherwise the pane it's in.
void App::toggle_map()
{
    if (!fullscreen_)
    {
        toggle_metadata();
        return;
    }
    cfg_.map_overlay = !cfg_.map_overlay;
    cfg_.save();
    refresh_map();
}
