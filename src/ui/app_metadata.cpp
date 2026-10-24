// The info pane and the fullscreen overlay: what's known about the image. Part of App (app.h).
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
    const rap::size_f before = window_->size();
    metadata_visible_ = !metadata_visible_;
    metadata_pane_->set_visible(metadata_visible_);
    if (metadata_visible_)
    {
        splitter_->set_sizes({std::max(100.f, before.w), float(cfg_.metadata_pane_width)});
        refresh_metadata();
    }
    // The window takes (or gives back) what the pane adds in the same step,
    // so the first frame is laid out at the final width: laying the pane into
    // the old width first would show its divider mid-image for a frame.
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
    MetadataGroup file{"File", {}};
    file.items.push_back({"Name", folder_.current_name()});
    file.items.push_back({"Folder", folder_.directory()});
    if (file_size_)
        file.items.push_back({"Size", human_size(file_size_) + " (" + std::to_string(file_size_) + " bytes)"});
    if (const std::string t = file_time(image_path_, "%Y-%m-%d %H:%M:%S"); !t.empty())
        file.items.push_back({"Modified", t});
    groups.push_back(std::move(file));
    MetadataGroup img{"Image", {}};
    img.items.push_back({"Dimensions", std::to_string(image_->width) + " × " + std::to_string(image_->height)});
    img.items.push_back({"Format", image_->format});
    if (!image_->pixel_format.empty())
        img.items.push_back({"Pixel format", image_->pixel_format.describe(image_->width, image_->height)});
    if (!image_->color_space.empty())
        img.items.push_back({"Colour space", image_->color_space});
    img.items.push_back({"Source depth", std::to_string(image_->bits_per_sample) + " bits per sample"});
    img.items.push_back({"Alpha", image_->has_alpha ? "yes" : "no"});
    if (!image_->info.empty())
        img.items.push_back({"Notes", image_->info});
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
            gps->items.insert(gps->items.begin(), {"Place", image_->place});
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
            lines.push_back("compression " + compression);
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
        const double shown = image_->final_width > 0 ? view_->zoom() * image_->width / image_->final_width : view_->zoom();
        std::string line = std::to_string(int(std::lround(shown * 100))) + "%";
        if (image_->is_preview())
            line += image_->is_raw ? "  ·  embedded preview" : "  ·  preview";
        else if (image_->tonable())
        {
            char ev[32];
            std::snprintf(ev, sizeof ev, "%+.2f EV", exposure_->value());
            line += std::string(image_->linear ? "  ·  developed  ·  " : "  ·  HDR  ·  ") + ev;
        }
        lines.push_back(line);
    }
    flush();
    return sections;
}

void App::refresh_metadata()
{
    if (!metadata_visible_)
        return;
    const std::string url = image_ && image_->has_location ? cfg_.map_link(image_->latitude, image_->longitude) : "";
    metadata_model_.set(collect_metadata(), image_.get(), url);
}
