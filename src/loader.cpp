#include "loader.h"

#include "platform.h"

#include <algorithm>
#include <cmath>

// -- Loader -------------------------------------------------------------------
Loader::Loader(Callback callback, Dispatch dispatch) : callback_(std::move(callback)), dispatch_(std::move(dispatch))
{
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    workers_ = std::max(1u, unsigned(std::lround(cores * 0.8)));
    foreground_ = std::thread([this] { run_foreground(); });
    reader_ = std::thread([this] { run_reader(); });
    for (unsigned i = 0; i < workers_; ++i)
        pool_.emplace_back([this] { run_worker(); });
}

Loader::~Loader()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    fg_wake_.notify_all();
    reader_wake_.notify_all();
    worker_wake_.notify_all();
    foreground_.join();
    reader_.join();
    for (std::thread& t : pool_)
        t.join();
}

std::shared_ptr<const FileBlob> Loader::take_read(const std::string& path)
{
    for (auto it = read_.begin(); it != read_.end(); ++it)
        if (it->path == path)
        {
            std::shared_ptr<const FileBlob> file = std::move(it->file);
            read_.erase(it);
            return file;
        }
    return nullptr;
}

bool Loader::busy_with(const std::string& path) const
{
    const auto has = [&](const std::vector<std::string>& v) { return std::find(v.begin(), v.end(), path) != v.end(); };
    return reading_ == path || has(developing_) || std::any_of(read_.begin(), read_.end(), [&](const Job& j) { return j.path == path; });
}

void Loader::request(uint64_t generation, const std::string& path)
{
    {
        std::lock_guard lock(mutex_);
        develop_.reset();  // a develop for an image we've left, not yet started, is dropped
        pending_ = Job{generation, path, nullptr};
    }
    fg_wake_.notify_one();
}

void Loader::request_develop(uint64_t generation, const std::string& path)
{
    {
        std::lock_guard lock(mutex_);
        if (std::find(developing_.begin(), developing_.end(), path) != developing_.end())
        {  // already decoding in full (left and come back to, or a prefetch): its result is adopted
            develop_.reset();
            return;
        }
        develop_ = Job{generation, path, take_read(path)};
    }
    reader_wake_.notify_one();
    worker_wake_.notify_one();
}

void Loader::prefetch(std::vector<std::string> paths)
{
    {
        std::lock_guard lock(mutex_);
        // What's read and no longer wanted goes; the rest is decoded in the new order.
        std::erase_if(read_, [&](const Job& j) { return std::find(paths.begin(), paths.end(), j.path) == paths.end(); });
        order_ = paths;
        prefetch_.clear();
        for (const std::string& p : paths)
            if (!busy_with(p) && std::find(in_flight_.begin(), in_flight_.end(), p) == in_flight_.end())
                prefetch_.push_back(p);
    }
    reader_wake_.notify_one();
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
            job.file = take_read(job.path);  // the reader got to it first
        }
        // Read here, not queued behind the reader: the image on screen always wins.
        if (!job.file)
            job.file = read_file(key_file(job.path));
        provide_file_bytes(key_file(job.path), job.file);
        // A quick stand-in first (a raw's embedded JPEG); its develop goes to
        // the pool, ahead of any prefetching, with the file already read.
        auto preview = std::make_shared<Image>();
        if (key_part(job.path) == ImagePart{} && decode_preview(job.path, *preview))  // only a file's first image has one
        {
            preview->build_mips();
            post(new Result{job.generation, job.path, preview, {}, true}, Lane::Foreground);
            {
                std::lock_guard lock(mutex_);
                if (std::find(developing_.begin(), developing_.end(), job.path) == developing_.end())
                    develop_ = Job{job.generation, job.path, std::move(job.file)};
            }
            release_file_bytes();
            worker_wake_.notify_one();
            continue;
        }
        auto* r = new Result{job.generation, job.path, std::make_shared<Image>(), {}, false};
        decode_into(*r);
        release_file_bytes();
        post(r, Lane::Foreground);
    }
}

// One file at a time, as fast as the disk goes: the develop's file if it
// needs one, then the prefetches in order, never more read ahead than there
// are workers to take them.
void Loader::run_reader()
{
    for (;;)
    {
        std::string path;
        bool for_develop = false;
        {
            std::unique_lock lock(mutex_);
            reader_wake_.wait(lock,
                              [this]
                              {
                                  return stop_ || (develop_ && !develop_->file) ||
                                         (!prefetch_.empty() && read_.size() < workers_);
                              });
            if (stop_)
                return;
            if (develop_ && !develop_->file)
            {
                path = develop_->path;
                for_develop = true;
            }
            else
            {
                path = prefetch_.front();
                prefetch_.erase(prefetch_.begin());
            }
            reading_ = path;
        }
        std::shared_ptr<const FileBlob> file = read_file(key_file(path));
        {
            std::lock_guard lock(mutex_);
            reading_.clear();
            if (for_develop)
            {
                if (develop_ && develop_->path == path && !develop_->file)
                    develop_->file = std::move(file);
            }
            else if (std::find(order_.begin(), order_.end(), path) != order_.end())
                read_.push_back(Job{0, path, std::move(file)});
        }
        worker_wake_.notify_one();
    }
}

void Loader::run_worker()
{
    // Below the foreground worker and the UI when cores are contended.
    platform::lower_thread_priority();
    for (;;)
    {
        Job job;
        bool is_develop = false, full = false;
        {
            std::unique_lock lock(mutex_);
            worker_wake_.wait(lock, [this] { return stop_ || (develop_ && develop_->file) || !read_.empty(); });
            if (stop_)
                return;
            if (develop_ && develop_->file)
            {
                job = std::move(*develop_);
                develop_.reset();
                is_develop = full = true;
            }
            else
            {
                // Nearest first, in the latest list's order.
                const auto rank = [this](const Job& j)
                { return std::find(order_.begin(), order_.end(), j.path) - order_.begin(); };
                auto it = std::min_element(read_.begin(), read_.end(), [&](const Job& a, const Job& b) { return rank(a) < rank(b); });
                job = std::move(*it);
                read_.erase(it);
                // As far as a visit would take it: the whole image when the
                // GPU develops raws (only unpacked here), else its preview.
                full = decode_options().gpu_develop;
            }
            in_flight_.push_back(job.path);
            if (full)
                developing_.push_back(job.path);
        }
        reader_wake_.notify_one();  // room for another read
        provide_file_bytes(key_file(job.path), job.file);
        auto* r = new Result{job.generation, job.path, std::make_shared<Image>(), {}, false, !is_develop};
        if (full)
            decode_into(*r);
        else if (key_part(job.path) == ImagePart{} && decode_preview(job.path, *r->image))
        {
            r->preview = true;
            r->image->build_mips();
        }
        else
        {
            provide_file_bytes(key_file(job.path), job.file);  // a failed preview may have let them go
            decode_into(*r);
        }
        release_file_bytes();
        job.file.reset();
        {
            std::lock_guard lock(mutex_);
            in_flight_.erase(std::find(in_flight_.begin(), in_flight_.end(), job.path));
            if (full)
                developing_.erase(std::find(developing_.begin(), developing_.end(), job.path));
        }
        post(r, is_develop ? Lane::Develop : Lane::Prefetch);
    }
}
