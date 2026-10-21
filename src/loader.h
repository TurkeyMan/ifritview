#pragma once

#include "image.h"

#include <glib.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Decodes on a worker thread. A newer request replaces a pending one, so
// scrolling quickly through a folder only decodes what ends up on screen.
// Decoding off the main thread, with the image being navigated to always
// first. A dedicated foreground worker handles only that image (its preview,
// or its full decode when there is none) and never waits behind anything.
// A pool of background workers, at lower OS priority, handles the develop of
// the image on screen and then prefetches of its neighbours, nearest first.
class Loader
{
public:
    struct Result
    {
        uint64_t generation;
        std::string path;
        std::shared_ptr<Image> image;  // null on failure
        std::string error;
        bool preview = false;                  // a quick stand-in; the real decode follows
        bool prefetch = false;                 // decoded ahead of time; cache it, don't show it
        bool cached = false;                   // served from the cache: pixels already carry any edits
        gint64 picked_us = 0, decoded_us = 0;  // trace: loader timeline
    };
    using Callback = std::function<void(Result&)>;  // runs on the UI thread
    // How results reach the UI thread. Priority: 0 = the image on screen
    // (jump the queue), 1 = its develop, 2 = prefetches (when idle).
    using Dispatch = std::function<void(std::function<void()>, int priority)>;

    Loader(Callback callback, Dispatch dispatch);
    ~Loader();
    // The image to show now; a preview, if it has one, is followed by its develop.
    void request(uint64_t generation, const std::string& path);
    // Only the develop: the preview is already on screen (from the cache).
    void request_develop(uint64_t generation, const std::string& path);
    // Files to decode when idle, nearest first; replaces the previous list.
    void prefetch(std::vector<std::string> paths);

private:
    void run_foreground();
    void run_background();
    enum class Lane
    {
        Foreground,
        Develop,
        Prefetch
    };
    void post(Result* r, Lane lane);
    void decode_into(Result& r);

    Callback callback_;
    Dispatch dispatch_;
    std::mutex mutex_;
    std::condition_variable fg_wake_, bg_wake_;
    struct Job
    {
        uint64_t generation;
        std::string path;
    };
    std::optional<Job> pending_;  // foreground
    std::optional<Job> develop_;  // background, ahead of prefetches
    std::vector<std::string> prefetch_;
    std::vector<std::string> in_flight_;  // background paths being decoded right now
    bool stop_ = false;
    std::thread foreground_;
    std::vector<std::thread> background_;
};
