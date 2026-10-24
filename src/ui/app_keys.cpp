// The keyboard. Part of App (app.h).
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

bool App::on_key(const rap::key_event& e)
{
    if (!e.press)
        return false;
    // The exposure slider keeps its keys while it has focus: arrows step it
    // (Shift refines, as on the wheel), Home is the camera's own exposure,
    // and Esc or Enter hand the keys back to the image.
    if (exposure_->has_focus())
    {
        const double step = e.modifiers & rap::mod_shift ? 0.01 : 0.1;
        switch (e.key)
        {
            case rap::key::left:
            case rap::key::down:
                set_exposure(exposure_->value() - step);
                return true;
            case rap::key::right:
            case rap::key::up:
                set_exposure(exposure_->value() + step);
                return true;
            case rap::key::home:
                set_exposure(image_ ? image_->exposure_bias : 0.0);
                return true;
            case rap::key::escape:
            case rap::key::enter:
                view_->canvas()->focus();
                return true;
            default:
                break;  // everything else keeps its usual meaning
        }
    }
    const bool ctrl = e.modifiers & rap::mod_ctrl;
    const char32_t c = e.key == rap::key::character ? e.codepoint : 0;
    switch (e.key)
    {
        case rap::key::f1:
            open_settings_page(4);  // About
            return true;
        case rap::key::page_up:
        case rap::key::page_down:
        {
            // Within the file: its pages, or with Shift its mip levels.
            const int d = e.key == rap::key::page_up ? -1 : +1;
            if (e.modifiers & rap::mod_shift)
                step_level(d);
            else
                step_page(d);
            return true;
        }
        case rap::key::left:
        case rap::key::backspace:
            navigate(-1);
            return true;
        case rap::key::space:
            if (image_ && image_->is_video)
            {
                toggle_video();  // on a video, Space is play/pause
                return true;
            }
            navigate(+1);
            return true;
        case rap::key::right:
            navigate(+1);
            return true;
        case rap::key::home:
            go_first();
            return true;
        case rap::key::end:
            go_last();
            return true;
        case rap::key::enter:
        case rap::key::f11:
            toggle_fullscreen();
            return true;
        case rap::key::escape:
            if (slideshow_)
                stop_slideshow();
            if (fullscreen_)
                toggle_fullscreen();
            else
                quit();
            return true;
        case rap::key::f5:
            reload();
            return true;
        case rap::key::tab:
            if (!view_->comparing())
                return false;
            set_compare(view_->compare_mode() == View::Compare::A ? View::Compare::B : View::Compare::A);
            return true;
        default:
            break;
    }
    switch (c)
    {
        case '+':
        case '=':
            zoom_in();
            return true;
        case '-':
            zoom_out();
            return true;
        case '1':
            zoom_original();
            return true;
        case 'f':
            zoom_fit();
            return true;
        case 'm':
            if (image_ && image_->is_video && image_->has_audio)
                toggle_mute();
            return true;
        case 'l':
            transform(8);
            return true;  // rotate left
        case 'r':
            transform(6);
            return true;  // rotate right
        case 'h':
            transform(2);
            return true;  // mirror horizontally
        case 'v':
            transform(4);
            return true;  // mirror vertically
        case 'o':
            if (ctrl)
            {
                open_dialog();
                return true;
            }
            break;
        case 'p':
            if (ctrl)
            {
                print_image();
                return true;
            }
            break;
        case ',':
            if (ctrl)
            {
                open_settings();
                return true;
            }
            break;
        case 'q':
            if (ctrl)
            {
                quit();
                return true;
            }
            break;
        case 'y':
            if (ctrl)
            {
                crop_to_selection();
                return true;
            }
            break;
        case 's':
            if (ctrl)
                save_as();
            else
                toggle_slideshow();
            return true;
        case 'i':
            if (fullscreen_)
            {
                // In fullscreen, I is the overlay switch; the choice is kept.
                cfg_.overlay_enabled = !cfg_.overlay_enabled;
                cfg_.save();
                view_->config_changed();
            }
            else
                toggle_metadata();
            return true;
        case 'a':
            toggle_animation();
            return true;
        case 'c':
            if (ctrl)
            {
                copy_to_clipboard();
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}
