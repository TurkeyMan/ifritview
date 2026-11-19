#include "loader.h"

#include "platform.h"

#include <rapport/sys.h>

#include <glib/gstdio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace
{

std::string base_name(const std::string& key)
{
    const std::string f = key_file(key);
    const size_t slash = f.find_last_of("/\\");
    return slash == std::string::npos ? f : f.substr(slash + 1);
}

}  // namespace

// -- Loader -------------------------------------------------------------------
Loader::Loader(Callback callback, Dispatch dispatch) : callback_(std::move(callback)), dispatch_(std::move(dispatch))
{
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    workers_ = std::max(1u, unsigned(std::lround(cores * 0.8)));
    foreground_ = std::thread([this] { run_foreground(); });
    reader_ = std::thread([this] { run_reader(); });
    for (unsigned i = 0; i < workers_; ++i)
        pool_.emplace_back([this, i] { run_worker(i); });
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

bool Loader::take_read(const std::string& path, Job& job)
{
    for (auto it = read_.begin(); it != read_.end(); ++it)
        if (it->path == path)
        {
            job.file = std::move(it->file);
            job.changes = it->changes;
            read_.erase(it);
            return true;
        }
    return false;
}

uint64_t Loader::changes(const std::string& path) const
{
    const auto it = changes_.find(key_file(path));
    return it == changes_.end() ? 0 : it->second;
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
        develop_ = Job{generation, path, nullptr};
        take_read(path, *develop_);
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

void Loader::cancel_develop(const std::string& path)
{
    std::lock_guard lock(mutex_);
    if (develop_ && develop_->path == path)
        develop_.reset();
}

void Loader::file_changed(const std::string& file)
{
    {
        std::lock_guard lock(mutex_);
        ++changes_[file];
        const auto of_file = [&](const std::string& path) { return key_file(path) == file; };
        std::erase_if(read_, [&](const Job& j) { return of_file(j.path); });
        std::erase_if(developing_, of_file);  // the ones running finish stale
        if (develop_ && of_file(develop_->path))
            develop_->file.reset();  // read again
    }
    reader_wake_.notify_one();
}

void Loader::set_stand_in_fit(int width, int height)
{
    std::lock_guard lock(mutex_);
    fit_w_ = width;
    fit_h_ = height;
}

// Results for the image on screen jump the main loop's queue; prefetches
// wait until the main thread is otherwise idle. One whose file changed while
// it waited in the queue arrives stale, as if it had changed while decoding.
void Loader::post(Result* r, Lane lane, uint64_t changed)
{
    if (lane == Lane::Foreground)
        platform::trace("loader: the image on screen is decoded, posted to the UI");
    const int priority = lane == Lane::Foreground ? 0 : lane == Lane::Develop ? 1 : 2;
    dispatch_(
        [this, r, changed]
        {
            {
                std::lock_guard lock(mutex_);
                if (changes(r->path) != changed)
                    r->stale = true;
            }
            callback_(*r);
            delete r;
        },
        priority);
}

void Loader::decode_into(Result& r, const Job& job)
{
    r.picked_us = g_get_monotonic_time();
    r.stamp = job.file->stamp;
    if (!decode_image_part(key_file(r.path), key_part(r.path), *r.image, r.error))  // r.path: a part's key
    {
        r.incomplete = r.image->incomplete;
        r.image.reset();
    }
    else
    {
        r.image->stamp = job.file->stamp;
        r.image->build_mips();
    }
    r.decoded_us = g_get_monotonic_time();
    std::lock_guard lock(mutex_);
    r.stale = changes(job.path) != job.changes;
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
            if (!take_read(job.path, job))  // the reader got to it first
                job.changes = changes(job.path);
            set_preview_fit(fit_w_, fit_h_);
        }
        // Read here, not queued behind the reader: the image on screen always wins.
        const gint64 t0 = g_get_monotonic_time();
        const bool had = job.file != nullptr;
        if (!job.file)
            job.file = read_file(key_file(job.path));
        platform::trace("loader: foreground %s, %s in %.1f ms", base_name(job.path).c_str(), had ? "read already" : "read", (g_get_monotonic_time() - t0) / 1000.0);
        provide_file_bytes(key_file(job.path), job.file);
        // A quick stand-in first (a raw's embedded JPEG), unless the image
        // itself is as quick (a raw the GPU decodes: no flicker from one to
        // the other); its develop goes to the pool, ahead of any prefetching,
        // with the file already read.
        auto preview = std::make_shared<Image>();
        const bool first_image = key_part(job.path) == ImagePart{};  // only a file's first image has one
        if (first_image && !decode_is_quick(job.path) && decode_preview(job.path, *preview))
        {
            preview->stamp = job.file->stamp;
            preview->build_mips();
            auto* r = new Result{job.generation, job.path, preview, {}, true};
            r->stamp = job.file->stamp;
            const uint64_t changed = job.changes;
            {
                std::lock_guard lock(mutex_);
                r->stale = changes(job.path) != job.changes;
                if (!r->stale && std::find(developing_.begin(), developing_.end(), job.path) == developing_.end())
                    develop_ = std::move(job);
            }
            post(r, Lane::Foreground, changed);
            release_file_bytes();
            worker_wake_.notify_one();
            continue;
        }
        auto* r = new Result{job.generation, job.path, std::make_shared<Image>(), {}, false};
        decode_into(*r, job);
        release_file_bytes();
        post(r, Lane::Foreground, job.changes);
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
        uint64_t changed = 0;
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
            changed = changes(path);
        }
        const gint64 t0 = g_get_monotonic_time();
        std::shared_ptr<const FileBlob> file = read_file(key_file(path));
        const double ms = (g_get_monotonic_time() - t0) / 1000.0, mb = file->data.size() / 1048576.0;
        {
            std::lock_guard lock(mutex_);
            reading_.clear();
            const bool stale = changes(path) != changed;  // changed as it was read: read again, or not at all
            platform::trace("loader: read %s%s, %.1f MB in %.1f ms (%.0f MB/s); %zu read waiting, %zu decoding", base_name(path).c_str(),
                            for_develop ? " (develop)" : "", mb, ms, ms > 0 ? mb / ms * 1000 : 0.0, read_.size() + (for_develop ? 0 : 1),
                            in_flight_.size());
            if (stale)
                ;
            else if (for_develop)
            {
                if (develop_ && develop_->path == path && !develop_->file)
                {
                    develop_->file = std::move(file);
                    develop_->changes = changed;
                }
            }
            else if (std::find(order_.begin(), order_.end(), path) != order_.end())
                read_.push_back(Job{0, path, std::move(file), changed});
        }
        worker_wake_.notify_one();
    }
}

void Loader::run_worker(unsigned index)
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
        const gint64 started = g_get_monotonic_time();
        platform::trace("loader: worker %u starts %s (%s)", index, base_name(job.path).c_str(),
                        is_develop ? "develop" : full ? "prefetch, in full" : "prefetch, preview");
        provide_file_bytes(key_file(job.path), job.file);
        auto* r = new Result{job.generation, job.path, std::make_shared<Image>(), {}, false, !is_develop};
        if (full)
            decode_into(*r, job);
        else if (key_part(job.path) == ImagePart{} && decode_preview(job.path, *r->image))
        {
            r->preview = true;
            r->stamp = r->image->stamp = job.file->stamp;
            r->image->build_mips();
        }
        else
        {
            provide_file_bytes(key_file(job.path), job.file);  // a failed preview may have let them go
            decode_into(*r, job);
        }
        release_file_bytes();
        job.file.reset();
        platform::trace("loader: worker %u done %s in %.0f ms", index, base_name(job.path).c_str(), (g_get_monotonic_time() - started) / 1000.0);
        {
            std::lock_guard lock(mutex_);
            in_flight_.erase(std::find(in_flight_.begin(), in_flight_.end(), job.path));
            r->stale = changes(job.path) != job.changes;
            if (full && !r->stale)  // (file_changed took a stale one out)
                developing_.erase(std::find(developing_.begin(), developing_.end(), job.path));
        }
        post(r, is_develop ? Lane::Develop : Lane::Prefetch, job.changes);
    }
}

// -- FileSettle ---------------------------------------------------------------
bool FileSettle::look(FileStamp now)
{
    const bool still = now == seen_ && !stirred_;
    changed_ |= now != read_;
    seen_ = now;
    stirred_ = false;
    return still && changed_;
}

// -- --check-loader -----------------------------------------------------------
int check_loader(const std::string& dir)
{
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;
    const auto fail = [](const char* what)
    {
        std::fprintf(stderr, "check-loader: %s\n", what);
        return 1;
    };
    g_mkdir_with_parents(dir.c_str(), 0700);
    const std::string file = platform::join_path(dir, "check-loader.ppm");
    const fs::path p = rap::sys::path_from_utf8(file);
    std::ofstream(p, std::ios::binary) << "P6\n2 2\n255\n" << std::string(12, '\x80');

    // A file's time to the second can't tell a copy that sized it in full and
    // finished within the second from how it was read; finer, it can.
    std::error_code e;
    const auto second = std::chrono::floor<std::chrono::seconds>(fs::last_write_time(p, e));
    fs::last_write_time(p, second, e);
    const FileStamp before = FileStamp::of(file);
    fs::last_write_time(p, second + 500ms, e);
    const FileStamp after = FileStamp::of(file);
    if (e || !before.known())
        return fail("couldn't set the file's time");
    if (before == after)
        return fail("the file's time half a second on was taken for the same");

    // Settled only once it's changed and held still: by its size and time,
    // or, where they show nothing, by the watch's word, from which the
    // second's wait begins again.
    FileSettle quiet(before, before);
    if (quiet.look(before) || quiet.look(before))
        return fail("a file that never changed was taken as settled");
    quiet.stirred();
    if (quiet.look(before))
        return fail("a file was taken as settled the moment the watch said it changed");
    if (!quiet.look(before))
        return fail("a file the watch said changed, then held still, was never taken as settled");
    FileSettle grown(before, before);
    if (grown.look(after))
        return fail("a file was taken as settled as it changed");
    if (!grown.look(after))
        return fail("a file that changed and then held still was never taken as settled");

    // A decode finished, and waiting for the UI thread, when its file
    // changes arrives stale; one of the file as it is now doesn't.
    std::mutex m;
    std::vector<std::function<void()>> queued;
    std::vector<Loader::Result> got;
    Loader loader([&](Loader::Result& r) { got.push_back(r); },
                  [&](std::function<void()> fn, int)
                  {
                      std::lock_guard lock(m);
                      queued.push_back(std::move(fn));
                  });
    // Waits for a result to be posted, or a minute: a deadline for a slow
    // machine, not a guess at how long it takes.
    const auto posted = [&]
    {
        const auto deadline = std::chrono::steady_clock::now() + 60s;
        for (;;)
        {
            {
                std::lock_guard lock(m);
                if (!queued.empty())
                    return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(10ms);
        }
    };
    // What's posted, delivered as the UI thread would.
    const auto deliver = [&]
    {
        std::vector<std::function<void()>> fns;
        {
            std::lock_guard lock(m);
            fns.swap(queued);
        }
        got.clear();
        for (auto& fn : fns)
            fn();
    };
    loader.request(1, file);
    if (!posted())
        return fail("nothing was decoded");
    loader.file_changed(file);
    deliver();
    if (got.size() != 1 || !got[0].stale)
        return fail("a decode done before its file changed arrived as if current");
    loader.request(2, file);
    if (!posted())
        return fail("nothing was decoded the second time");
    deliver();
    if (got.size() != 1 || got[0].stale || !got[0].image)
        return fail("a decode of the file as it is arrived stale, or failed");
    std::printf("check-loader: ok\n");
    return 0;
}
