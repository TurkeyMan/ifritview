#include "loader.h"

#include "platform.h"

#include "decoders/decoder.h"

#include <algorithm>

// -- Loader -------------------------------------------------------------------
Loader::Loader(Callback callback, Dispatch dispatch) : callback_(std::move(callback)), dispatch_(std::move(dispatch))
{
    foreground_ = std::thread([this] { run_foreground(); });
    const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    const unsigned workers = std::clamp(cores - 2, 1u, 4u);
    for (unsigned i = 0; i < workers; ++i)
        background_.emplace_back([this] { run_background(); });
}

Loader::~Loader()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    fg_wake_.notify_all();
    bg_wake_.notify_all();
    foreground_.join();
    for (std::thread& t : background_)
        t.join();
}

void Loader::request(uint64_t generation, const std::string& path)
{
    {
        std::lock_guard lock(mutex_);
        develop_.reset();  // a develop for an image we've left, not yet started, is dropped
        pending_ = Job{generation, path};
    }
    fg_wake_.notify_one();
}

void Loader::request_develop(uint64_t generation, const std::string& path)
{
    {
        std::lock_guard lock(mutex_);
        if (std::find(developing_.begin(), developing_.end(), path) != developing_.end())
        {  // already developing (left and come back to): its result is adopted
            develop_.reset();
            return;
        }
        develop_ = Job{generation, path};
    }
    bg_wake_.notify_one();
}

void Loader::prefetch(std::vector<std::string> paths)
{
    {
        std::lock_guard lock(mutex_);
        prefetch_ = std::move(paths);
    }
    bg_wake_.notify_all();
}

// Results for the image on screen jump the main loop's queue; prefetches
// wait until the main thread is otherwise idle.
void Loader::post(Result* r, Lane lane)
{
    if (lane == Lane::Foreground)
        platform::trace("loader: the image on screen is decoded, posted to the UI");
    const int priority = lane == Lane::Foreground ? 0 : lane == Lane::Develop ? 1 : 2;
    dispatch_(
        [this, r]
        {
            callback_(*r);
            delete r;
        },
        priority);
}

void Loader::decode_into(Result& r)
{
    r.picked_us = g_get_monotonic_time();
    if (!decode_image_part(key_file(r.path), key_part(r.path), *r.image, r.error))  // r.path: a part's key
        r.image.reset();
    else
        r.image->build_mips();
    r.decoded_us = g_get_monotonic_time();
}

void Loader::run_foreground()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock lock(mutex_);
            fg_wake_.wait(lock, [this] { return stop_ || pending_; });
            if (stop_)
                return;
            job = *pending_;
            pending_.reset();
        }
        // A quick stand-in first (a raw's embedded JPEG); its develop goes to
        // the background, ahead of any prefetching.
        auto preview = std::make_shared<Image>();
        if (key_part(job.path) == ImagePart{} && decode_preview(job.path, *preview))  // only a file's first image has one
        {
            preview->build_mips();
            post(new Result{job.generation, job.path, preview, {}, true}, Lane::Foreground);
            request_develop(job.generation, job.path);
            continue;
        }
        auto* r = new Result{job.generation, job.path, std::make_shared<Image>(), {}, false};
        decode_into(*r);
        post(r, Lane::Foreground);
    }
}

void Loader::run_background()
{
    // Below the foreground worker and the UI when cores are contended.
    platform::lower_thread_priority();
    for (;;)
    {
        Job job{};
        bool is_develop = false;
        {
            std::unique_lock lock(mutex_);
            bg_wake_.wait(lock,
                          [this]
                          {
                              if (stop_ || develop_)
                                  return true;
                              for (const std::string& p : prefetch_)
                                  if (std::find(in_flight_.begin(), in_flight_.end(), p) == in_flight_.end())
                                      return true;
                              return false;
                          });
            if (stop_)
                return;
            if (develop_)
            {
                job = *develop_;
                develop_.reset();
                is_develop = true;
            }
            else
            {
                for (auto it = prefetch_.begin(); it != prefetch_.end(); ++it)
                    if (std::find(in_flight_.begin(), in_flight_.end(), *it) == in_flight_.end())
                    {
                        job.path = *it;
                        prefetch_.erase(it);
                        break;
                    }
            }
            in_flight_.push_back(job.path);
            if (is_develop)
                developing_.push_back(job.path);
        }
        auto* r = new Result{job.generation, job.path, std::make_shared<Image>(), {}, false, !is_develop};
        if (is_develop)
            decode_into(*r);
        else
        {
            // A neighbour is prepared the way a normal load would be: its
            // preview when it has one (cheap), otherwise the image itself.
            if (key_part(job.path) == ImagePart{} && decode_preview(job.path, *r->image))
            {
                r->preview = true;
                r->image->build_mips();
            }
            else
                decode_into(*r);
        }
        {
            std::lock_guard lock(mutex_);
            in_flight_.erase(std::find(in_flight_.begin(), in_flight_.end(), job.path));
            if (is_develop)
                developing_.erase(std::find(developing_.begin(), developing_.end(), job.path));
        }
        bg_wake_.notify_one();
        post(r, is_develop ? Lane::Develop : Lane::Prefetch);
    }
}
