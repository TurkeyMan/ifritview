// Save As: the dialog, the preview of what would be saved, lossless JPEG. Part of App (app.h).
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

// Save As: the format and its settings first (previewing lossy ones in the
// view), then the file dialog for that format's files.
void App::save_as()
{
    if (!image_ || save_dialog_)
        return;
    pixels_for_cpu();
    // Comparing two files, Save saves where they differ, as it's shown.
    save_source_ = diff_session_ ? view_->difference_shown() : image_;
    if (!save_source_)
    {
        status_message_ = _("No difference image to save: the images are different sizes");
        update_status();
        return;
    }
    SaveDialog::Callbacks cb;
    cb.changed = [this](const Encoder& e, const EncodeOptions& o) { preview_save(e, o); };
    cb.preview_shown = [this](bool on) { show_save_preview(on); };
    cb.chosen = [this](const Encoder& e, const EncodeOptions& chosen)
    {
        EncodeOptions o = chosen;
        const MetadataChoice metadata = save_dialog_->metadata();
        cfg_.save_metadata = metadata_choice_text(metadata);
        cfg_.save();
        // The file's metadata, made to describe what's being saved. A
        // difference image has none of its own.
        // XMP's develop settings, once applied, no longer describe the pixels.
        const bool developed = cfg_.apply_xmp_edits && image_->xmp.present;
        if (save_source_ == image_)
            o.metadata = metadata_for_save(*image_, metadata, e.metadata, developed);
        // Lossless: the planned transform, with the metadata made for its size.
        std::shared_ptr<const LosslessJpeg> lossless;
        if (o.lossless_transform && lossless_plan_ && lossless_plan_->why_not.empty())
        {
            lossless = lossless_plan_;
            o.metadata = metadata_for_save(*image_, metadata, e.metadata, developed, lossless->width, lossless->height);
        }
        // After the dialog has gone, so the file dialog has the main window as parent.
        ui_.post([this, e = &e, o, source = save_source_, lossless] { choose_save_path(*e, o, source, lossless); });
    };
    cb.closed = [this]
    {
        ++preview_->generation;
        preview_image_.reset();
        preview_difference_.reset();
        save_source_.reset();
        if (!diff_session_)
            end_comparing();
        save_dialog_.reset();
        status_message_.clear();
        update_toolbar();
        update_status();
    };
    if (slideshow_)
        stop_slideshow();
    MetadataChoice metadata;
    std::string ignored;
    parse_metadata_choice(cfg_.save_metadata, metadata, ignored);  // an unreadable setting keeps everything
    MetadataChoice present;  // nothing, unless the image's own
    present.keep = present.location = present.personal = present.author = present.description = present.capture = present.history = false;
    if (save_source_ == image_ && image_->stored)
        present = metadata_present(*image_->stored);
    // The file's own format and settings, as decoded (a .jpeg is "jpg"); a
    // difference image has none.
    const bool own = !diff_session_ && !image_->encoded_as.empty();
    const Encoder* by_name = encoder_for_extension(file_extension(image_path_));
    const std::string format = diff_session_ ? std::string("png") : own ? image_->encoded_as : by_name ? by_name->extension : "";
    save_dialog_ = SaveDialog::open(ui_, window_.get(), format, own ? image_->encoding : std::map<std::string, int>{}, save_options_, metadata,
                                    present, std::move(cb));
    plan_lossless();
    update_toolbar();
}

// The view compares the original (A) with the image as it would save (B),
// starting on B: what the save would look like.
void App::show_save_preview(bool on)
{
    if (diff_session_)
        return;  // saving the difference: the comparison stays as it is
    // The encode is already done: this only changes what the view shows.
    if (on && preview_image_ && image_)
    {
        if (view_->comparing())
        {
            view_->set_compare_b(preview_image_, preview_difference_);
            show_side();
        }
        else
        {
            view_->begin_compare(image_, preview_image_, _("Original"), _("Saved"), preview_difference_, View::Compare::B);
            view_->set_compare_gain(int(gain_->value() + 0.5));  // the gain stays as last set
        }
    }
    else
    {
        end_comparing();
        return;
    }
    update_toolbar();
    update_status();
}

// Encodes the image as it would save, decodes that back and shows it in the
// view, with the file size, a moment after the settings stop changing.
// JPEG's Lossless as it will save: the image as shown, with the edge strips
// the plan drops cleared (it's never more than the image).
static std::shared_ptr<Image> lossless_preview(const Image& image, const LosslessJpeg& plan)
{
    auto saved = std::make_shared<Image>();
    saved->allocate(image.width, image.height);
    std::fill(saved->pixels.begin(), saved->pixels.end(), 0);
    saved->has_alpha = true;
    const int right = std::min(image.width, plan.x + plan.width), bottom = std::min(image.height, plan.y + plan.height);
    for (int y = plan.y; y < bottom; ++y)
        std::memcpy(saved->row(y) + plan.x, image.row(y) + plan.x, size_t(right - plan.x) * 4);
    return saved;
}

void App::preview_lossless()
{
    const uint64_t generation = ++preview_->generation;
    preview_image_.reset();
    preview_difference_.reset();
    const std::shared_ptr<const LosslessJpeg> plan = lossless_plan_;
    // Also called while the dialog is still opening, before save_dialog_ is set.
    if (!plan || !plan->why_not.empty() || save_source_ != image_)
    {
        show_save_preview(false);
        if (save_dialog_)
            save_dialog_->set_info("");
        return;
    }
    if (save_dialog_)
        save_dialog_->set_info(human_size(plan->data.size()));  // the metadata comes on top
    if (plan->differs.empty())
    {
        show_save_preview(false);  // exactly the image as shown
        return;
    }
    std::thread(
        [this, generation, image = image_, plan, state = preview_, running = preview_->start()]
        {
            platform::lower_thread_priority();
            std::shared_ptr<Image> saved = lossless_preview(*image, *plan);
            const auto difference = std::make_shared<ImageDifference>(compare_images(*image, *saved));
            // Every difference is the strips dropped: say that, not a PSNR.
            difference->summary = rap::i18n::format(_("lossless, {0}"), plan->differs);
            platform::post_main(
                [this, generation, saved, difference, state]
                {
                    if (state->generation.load() != generation || !save_dialog_)
                        return;
                    preview_image_ = saved;
                    preview_difference_ = difference;
                    if (save_dialog_->preview_shown())
                        show_save_preview(true);
                });
        })
        .detach();
}

void App::preview_save(const Encoder& e, const EncodeOptions& o)
{
    // The original's own data, transformed: no encode to preview.
    if (o.lossless_transform)
    {
        preview_lossless();
        return;
    }
    const uint64_t generation = ++preview_->generation;
    // Kept exactly (PNG, WebP's Lossless): nothing to show, but its size is
    // still worth knowing, so it's encoded all the same.
    const bool lossy = e.lossy(o);
    if (!save_source_ || !lossy)
    {
        preview_image_.reset();
        preview_difference_.reset();
        show_save_preview(false);
        if (save_dialog_)
            save_dialog_->set_info("");
        if (!save_source_)
            return;
    }
    if (save_dialog_)
        save_dialog_->set_info("…");
    std::shared_ptr<Image> source = save_source_;
    const Encoder* encoder = &e;
    std::thread(
        [this, generation, source, encoder, o, lossy, state = preview_, running = preview_->start()]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));  // let a dragged slider settle
            if (state->generation.load() != generation)
                return;
            platform::lower_thread_priority();
            const std::string path = (std::filesystem::temp_directory_path() /
                                      ("ifritview-preview-" + std::to_string(generation) + "." + encoder->extension))
                                         .string();
            std::string error;
            auto result = std::make_shared<Image>();
            uintmax_t size = 0;
            bool ok = encoder->encode(*source, path, o, error);
            if (ok)
            {
                std::error_code ec;
                size = std::filesystem::file_size(path, ec);
                ok = !lossy || decode_image(path, *result, error);
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
            if (state->generation.load() != generation)
                return;
            std::shared_ptr<const ImageDifference> difference;
            if (ok && lossy)
                difference = std::make_shared<ImageDifference>(compare_images(*source, *result));
            platform::post_main(
                [this, generation, result, difference, ok, lossy, size, error, state]
                {
                    if (state->generation.load() != generation || !save_dialog_)
                        return;
                    if (!ok)
                    {
                        save_dialog_->set_info("failed");
                        status_message_ = rap::i18n::format(_("Preview failed: {0}"), error);
                        update_status();
                        return;
                    }
                    save_dialog_->set_info(human_size(size));
                    if (!lossy)
                        return;
                    preview_image_ = result;
                    preview_difference_ = difference;
                    if (save_dialog_->preview_shown())
                        show_save_preview(true);
                });
        })
        .detach();
}

// Whether JPEG's Lossless can be offered: worked out on a thread (it reads
// the original and does the transform, a fraction of a second for a large
// one), then told to the dialog.
void App::plan_lossless()
{
    lossless_plan_.reset();
    if (!save_dialog_)
        return;
    if (save_source_ != image_)
    {
        save_dialog_->set_lossless(false, _("Lossless: only the original image can be saved this way"));
        return;
    }
    const uint64_t generation = ++lossless_generation_;
    std::thread(
        [this, generation, image = image_, path = image_path_]
        {
            auto plan = std::make_shared<const LosslessJpeg>(plan_lossless_jpeg(*image, path));
            platform::post_main(
                [this, generation, plan]
                {
                    if (generation != lossless_generation_ || !save_dialog_)
                        return;
                    lossless_plan_ = plan;
                    if (!plan->why_not.empty())
                        save_dialog_->set_lossless(false, rap::i18n::format(_("Not possible: {0}"), plan->why_not));
                    else if (plan->differs.empty())
                        save_dialog_->set_lossless(true, _("Exactly the image as shown, not one pixel re-encoded"));
                    else
                        save_dialog_->set_lossless(true, rap::i18n::format(_("With JPEG's blocks: {0} than shown"), plan->differs));
                });
        })
        .detach();
}

void App::choose_save_path(const Encoder& e, const EncodeOptions& o, std::shared_ptr<Image> source, std::shared_ptr<const LosslessJpeg> lossless)
{
    if (!source)
        return;
    rap::file_dialog_spec spec;
    spec.mode = rap::file_dialog_spec::mode::save;
    spec.title = rap::i18n::format(_("Save as {0}"), e.name);
    std::string stem = std::filesystem::path(diff_session_ ? image_path_ : folder_.current_name()).stem().string();
    if (diff_session_)
        stem += "-difference";
    const std::string name = (stem.empty() ? std::string("image") : stem) + "." + e.extension;
    const std::string dir = diff_session_ ? std::filesystem::path(image_path_).parent_path().string() : folder_.directory();
    spec.initial_path = dir.empty() ? name : dir + "/" + name;
    spec.filters = {{e.name, std::string("*.") + e.extension}};
    ui_.choose_files(window_.get(), spec,
                    [this, enc = &e, o, source, lossless](std::vector<std::string> files)
                    {
                        if (files.empty())
                            return;
                        std::string path = files.front();
                        if (encoder_for_extension(file_extension(path)) != enc)
                            path += std::string(".") + enc->extension;  // not every toolkit's dialog adds it
                        // On a thread: some formats take seconds to encode (AVIF, BC7
                        // with its mipmaps), and the window keeps working meanwhile.
                        const std::string name = std::filesystem::path(path).filename().string();
                        status_message_ = busy_message_ = rap::i18n::format(_("Saving {0}…"), name);
                        update_status();
                        std::thread(
                            [this, enc, o, source, lossless, path, name, running = saves_->start()]
                            {
                                std::string error;
                                const bool ok = platform::write_replacing(
                                    path,
                                    [&](const std::string& temp, std::string& error)
                                    {
                                        return lossless ? write_lossless_jpeg(*lossless, o.metadata.get(), temp, error)
                                                        : enc->encode(*source, temp, o, error);
                                    },
                                    error);
                                platform::post_main(
                                    [this, ok, error, name]
                                    {
                                        status_message_ = ok ? rap::i18n::format(_("Saved {0}"), name) : rap::i18n::format(_("Save failed: {0}"), error);
                                        if (ok)
                                            list_folder({});  // the new file may now be part of the folder
                                        update_status();
                                        update_title();
                                    });
                            })
                            .detach();
                    });
}
