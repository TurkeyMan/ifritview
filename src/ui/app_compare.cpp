// Comparing two images: A and B, swipe, difference, side by side, --diff. Part of App (app.h).
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

void App::set_compare(View::Compare mode)
{
    if (mode == View::Compare::Difference && !view_->compare_difference())
        mode = view_->compare_mode();  // not for images of different sizes
    view_->set_compare(mode);
    update_toolbar();  // the buttons act as one choice: unchecking the shown one keeps it
    update_status();
}

void App::swap_compare()
{
    view_->swap_compare();
    show_side();
    update_toolbar();
    update_status();
}

// Back to browsing: the view shows the image again, alone.
void App::end_comparing()
{
    set_side_by_side(false);
    view_->end_compare();
    if (diff_session_)
    {
        diff_session_ = false;
        status_message_.clear();
    }
    update_toolbar();
    update_status();
    update_title();
}

void App::show_side()
{
    if (!side_by_side_)
        return;
    side_view_->set_image(view_->compare_image(true));
    side_view_->set_label(view_->compare_label(true));
    syncing_framing_ = true;
    side_view_->set_framing(view_->framing());
    syncing_framing_ = false;
}

// The side pane opens to the right of the image, the window growing by as
// much (as for the info pane), so the image keeps its size.
void App::set_side_by_side(bool on)
{
    if (on && !view_->comparing())
        on = false;
    if (side_button_)
        side_button_->set_checked(on);
    if (on == side_by_side_)
        return;
    int vw = 0, vh = 0;
    view_->view_size(vw, vh);
    const rap::size_f before = window_->size();
    const bool sized = !fullscreen_ && !window_->maximized();
    side_by_side_ = on;
    side_pane_->set_visible(on);
    side_settle_.reset();
    if (on)
    {
        if (view_->compare_mode() == View::Compare::B)
            view_->set_compare(View::Compare::A);  // B is beside it now
        show_side();
        if (sized)
        {
            // Wider by a pane (and the gap, a guess until measured below), and
            // back to exactly this width when it closes.
            side_restore_w_ = before.w;
            float w = before.w + float(vw / view_->scale_factor()) + 6;
            const rap::rect_f work = window_->work_area();
            if (work.w > 0)
                w = std::min(w, work.w - 24);
            window_->resize({w, before.h});
            side_settle_ = SideSettle{vw, w};
        }
    }
    else
    {
        side_view_->set_image(nullptr);
        side_view_->set_label({});
        if (sized && side_restore_w_ > 0)
            window_->resize({side_restore_w_, before.h});
        side_restore_w_ = 0;
    }
    update_toolbar();
    update_status();
}

// Once the side pane is laid out at the new width: each pane as wide as the
// image area was, correcting the guessed gap between them.
void App::settle_side_by_side()
{
    if (!side_settle_ || std::fabs(window_->size().w - side_settle_->window_w) >= 1)
        return;  // not yet at the width asked for
    const SideSettle settle = *side_settle_;
    side_settle_.reset();
    int vw = 0, vh = 0;
    view_->view_size(vw, vh);
    const float short_by = float((settle.view_w - vw) / view_->scale_factor());
    const rap::rect_f work = window_->work_area();
    const float w = settle.window_w + 2 * short_by;  // the panes share the width equally
    if (std::fabs(short_by) >= 1 && (work.w <= 0 || w <= work.w - 24))
        window_->resize({w, window_->size().h});
}


void App::open_diff(const std::string& a_path, const std::string& b_path, const std::string& a_label, const std::string& b_label)
{
    if (save_dialog_)
        save_dialog_->close();
    if (slideshow_)
        stop_slideshow();
    stop_video();
    status_message_ = busy_message_ = _("Comparing…");
    update_status();
    const uint64_t generation = ++load_generation_;
    ++preview_->running;  // waited for on exit, as a save preview is
    std::thread(
        [this, generation, a_path, b_path, a_label, b_label, state = preview_]
        {
            struct Done
            {
                PreviewState& s;
                ~Done() { --s.running; }
            } done{*state};
            auto a = std::make_shared<Image>(), b = std::make_shared<Image>();
            std::string error;
            // Compared pixel for pixel, here: raws developed on the CPU, with
            // their pixels (for the GPU they'd have none until shown).
            develop_raws_on_cpu(true);
            const bool ok = decode_image(a_path, *a, error) && decode_image(b_path, *b, error);
            develop_raws_on_cpu(false);
            std::shared_ptr<const ImageDifference> difference;
            if (ok)
            {
                a->build_mips();
                b->build_mips();
                if (a->width == b->width && a->height == b->height)
                    difference = std::make_shared<ImageDifference>(compare_images(*a, *b));
            }
            platform::post_main(
                [this, generation, a, b, a_path, b_path, a_label, b_label, difference, ok, error]
                {
                    if (generation != load_generation_)
                        return;
                    if (!ok)
                    {
                        status_message_ = rap::i18n::format(_("Can't compare: {0}"), error);
                        update_status();
                        return;
                    }
                    // Named by file, or by folder and file when the names are the same.
                    const std::filesystem::path pa(a_path), pb(b_path);
                    const bool same = pa.filename() == pb.filename();
                    const auto name = [same](const std::filesystem::path& p)
                    { return same ? (p.parent_path().filename() / p.filename()).string() : p.filename().string(); };
                    end_comparing();
                    leave_image();
                    image_ = a;
                    image_path_ = a_path;
                    std::error_code ec;
                    file_size_ = std::filesystem::file_size(a_path, ec);
                    diff_session_ = true;
                    diff_name_ = (a_label.empty() ? name(pa) : a_label) + "  ↔  " + (b_label.empty() ? name(pb) : b_label);
                    status_message_ = diff_name_;
                    view_->begin_compare(a, b, a_label.empty() ? name(pa) : a_label, b_label.empty() ? name(pb) : b_label, difference,
                                         difference ? View::Compare::Difference : View::Compare::Swipe);
                    view_->set_compare_gain(int(gain_->value() + 0.5));
                    update_toolbar();
                    update_title();
                    update_status();
                    fit_window_to_image();
                });
        })
        .detach();
}
