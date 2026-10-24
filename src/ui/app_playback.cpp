// What moves: the slideshow, video and audio, animated images. Part of App (app.h).
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

// -- slideshow ----------------------------------------------------------------
void App::toggle_slideshow()
{
    if (slideshow_)
    {
        stop_slideshow();
        return;
    }
    if (folder_.empty() || navigation_locked())
        return;
    slideshow_ = true;
    slideshow_order_.clear();
    if (cfg_.slideshow_shuffle)
    {
        for (int i = 0; i < folder_.count(); ++i)
            slideshow_order_.push_back(i);
        std::shuffle(slideshow_order_.begin(), slideshow_order_.end(), rng());
        slideshow_pos_ = 0;
        if (folder_.go(slideshow_order_[0]))
            load_current();
    }
    if (!fullscreen_)
        toggle_fullscreen();  // the first slide starts once fullscreen has landed
    else
        slideshow_start_slide();
}

void App::stop_slideshow()
{
    outgoing_video_.stop();
    audio_fade_timer_.reset();
    video_.set_volume(cfg_.video_volume);
    slideshow_ = false;
    slide_.active = false;
    slideshow_timer_.reset();
    view_->end_motion();
    view_->zoom_fit();
}

// One slide: a random gentle framing change over the slide's duration, then
// on to the next image.
void App::slideshow_start_slide()
{
    if (!slideshow_)
        return;
    int ms = int(cfg_.slideshow_seconds * 1000);
    if (image_ && image_->is_video && image_->duration > 0)
    {
        // A video plays to its end; the next slide fades in over its last moments.
        const double played = video_.active() ? video_.position() : 0.0;
        ms = std::max(500, int((image_->duration - played) * 1000) - cfg_.slideshow_crossfade_ms);
    }
    slide_.active = image_ && !image_->is_video && cfg_.slideshow_motion && cfg_.slideshow_zoom > 0;
    if (tracing())
        std::fprintf(stderr, "slide: %s for %d ms\n", folder_.current_name().c_str(), ms);
    slide_.start_us = now_us();
    slide_.duration_ms = ms;
    if (!slide_.active)
    {
        view_->end_motion();
        view_->zoom_fit();
    }
    else
    {
        const bool in = random01() < 0.5;
        const double lo = 1.0 + cfg_.slideshow_zoom * 0.15, hi = 1.0 + cfg_.slideshow_zoom;
        slide_.z0 = in ? lo : hi;
        slide_.z1 = in ? hi : lo;
        slide_.fx0 = random01();
        slide_.fy0 = random01();
        slide_.fx1 = random01();
        slide_.fy1 = random01();
        apply_slide_motion();
    }
    slideshow_timer_ = ui_.timer(ms, false, [this] { slideshow_advance(); });
}

// Translate the slide's relative motion into this image's coordinates. The
// viewport is kept inside the image.
void App::apply_slide_motion()
{
    if (!slide_.active || !image_)
        return;
    int vw, vh;
    view_->view_size(vw, vh);
    const double w = image_->width, h = image_->height;
    const double fit = std::min(vw / w, vh / h);
    auto centre = [&](double z, double fx, double fy, double& cx, double& cy)
    {
        const double half_w = vw / (2 * z), half_h = vh / (2 * z);
        const double lo_x = std::min(half_w, w - half_w), hi_x = std::max(half_w, w - half_w);
        const double lo_y = std::min(half_h, h - half_h), hi_y = std::max(half_h, h - half_h);
        cx = lo_x + (hi_x - lo_x) * fx;
        cy = lo_y + (hi_y - lo_y) * fy;
    };
    const double z0 = fit * slide_.z0, z1 = fit * slide_.z1;
    double cx0, cy0, cx1, cy1;
    centre(z0, slide_.fx0, slide_.fy0, cx0, cy0);
    centre(z1, slide_.fx1, slide_.fy1, cx1, cy1);
    view_->begin_motion(z0, z1, cx0, cy0, cx1, cy1, slide_.duration_ms, slide_.start_us);
}

void App::slideshow_advance()
{
    if (!slideshow_)
        return;
    if (tracing())
        std::fprintf(stderr, "slideshow: last frame cost %.1f ms\n", view_->frame_ms());
    view_->crossfade_next(cfg_.slideshow_crossfade_ms);
    if (cfg_.slideshow_crossfade_ms > 0 && image_ && image_->is_video && video_.active())
    {
        outgoing_video_.stop();
        outgoing_video_.swap(video_);  // it plays on under the fade
        view_->crossfade_live();
    }
    if (cfg_.slideshow_crossfade_ms > 0)
        start_audio_fade(cfg_.slideshow_crossfade_ms);
    if (!slideshow_order_.empty())
    {
        slideshow_pos_ = (slideshow_pos_ + 1) % slideshow_order_.size();
        if (folder_.go(slideshow_order_[slideshow_pos_]))
            load_current();
    }
    else
    {
        if (!folder_.step(+1, true))
            return;
        load_current();
    }
}

// -- video --------------------------------------------------------------------
void App::play_video()
{
    if (!image_ || !image_->is_video)
        return;
    video_.start(image_path_, image_, cfg_.video_muted, cfg_.video_loop && !slideshow_);
    video_.set_volume(audio_fade_timer_ ? 0.0 : cfg_.video_volume);  // during a cross-fade it fades in
    // Position for the knob and the status bar, a few times a second.
    video_timer_ = ui_.timer(250, true,
                             [this]
                             {
                                 if (!video_.active())
                                 {
                                     video_timer_.reset();
                                     return;
                                 }
                                 if (video_.duration() > 0)
                                     view_->set_video_progress(video_.position() / video_.duration(), video_.playing());
                                 if (volume_dirty_)
                                 {
                                     volume_dirty_ = false;
                                     cfg_.save();
                                 }
                                 update_status();
                             });
}

void App::toggle_video()
{
    if (!video_.active())
        play_video();
    else
        video_.toggle();
    if (video_.duration() > 0)
        view_->set_video_progress(video_.position() / video_.duration(), video_.playing());
    update_status();
}

// Over `ms`, the outgoing video's sound falls to silence and the incoming
// one's rises to the set volume; then the outgoing playback ends.
void App::start_audio_fade(int ms)
{
    outgoing_video_.on_frame = [this] { view_->outgoing_changed(); };
    audio_fade_start_ = now_us();
    audio_fade_ms_ = ms;
    if (audio_fade_timer_)
        return;
    audio_fade_timer_ = ui_.timer(16, true,
                                  [this]
                                  {
                                      const double t = std::clamp((now_us() - audio_fade_start_) / (audio_fade_ms_ * 1000.0), 0.0, 1.0);
                                      outgoing_video_.set_volume(cfg_.video_volume * (1 - t));
                                      video_.set_volume(cfg_.video_volume * t);
                                      if (t < 1.0)
                                          return;
                                      outgoing_video_.stop();
                                      audio_fade_timer_.reset();
                                  });
}

void App::toggle_mute()
{
    cfg_.video_muted = !cfg_.video_muted;
    cfg_.save();
    video_.set_muted(cfg_.video_muted);
    view_->set_video_muted(cfg_.video_muted);
    update_status();
}

void App::stop_video()
{
    video_.stop();
    video_timer_.reset();
}

// -- animation ----------------------------------------------------------------
// Frames are swapped into the image's pixel buffer in place, so everything
// else (zoom, selection, save, copy) sees the current frame.
void App::show_animation_frame(size_t index)
{
    if (!image_ || !image_->animation || image_->animation->frames.empty())
        return;
    const Animation::Frame& f = image_->animation->frames[index % image_->animation->frames.size()];
    if (f.pixels.size() == image_->pixels.size())
    {
        std::memcpy(image_->pixels.data(), f.pixels.data(), f.pixels.size());
        image_->mips.clear();
        view_->image_changed();
    }
}

void App::schedule_animation_frame()
{
    const int delay = std::max(10, image_->animation->frames[animation_frame_].delay_ms);
    animation_timer_ = ui_.timer(delay, false,
                                 [this]
                                 {
                                     if (!animation_playing_ || !image_ || !image_->animation)
                                         return;
                                     animation_frame_ = (animation_frame_ + 1) % image_->animation->frames.size();
                                     show_animation_frame(animation_frame_);
                                     schedule_animation_frame();
                                 });
}

void App::start_animation()
{
    stop_animation();
    if (!image_ || !image_->animation || image_->animation->frames.size() < 2)
        return;
    animation_playing_ = true;
    animation_frame_ = 0;
    show_animation_frame(0);
    schedule_animation_frame();
}

void App::stop_animation()
{
    animation_timer_.reset();
    animation_playing_ = false;
}

void App::toggle_animation()
{
    if (!image_ || !image_->animation)
        return;
    if (animation_playing_)
    {
        stop_animation();
        status_message_ = "Animation paused";
    }
    else
    {
        animation_playing_ = true;
        schedule_animation_frame();
        status_message_.clear();
    }
    update_status();
}
