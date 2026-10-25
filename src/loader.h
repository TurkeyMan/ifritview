#pragma once

#include "image.h"
#include "decoders/decoder.h"

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

// Decoding off the main thread, with the image being navigated to always
// first. A dedicated foreground worker handles only that image (its preview,
// or its full decode when there is none) and never waits behind anything.
// Everything else goes through one reader, which reads files whole, one after
// another (a disk does one thing at a time), and hands each to a pool of
// workers at lower OS priority, about four in five of the cores, which decode
// them in parallel: the develop of the image on screen first, then its
// neighbours, nearest first, each as far as a visit would take it (a raw's
// mosaic, when the GPU develops it).
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
    // The develop asked for `path` isn't needed after all (shown in full already).
    void cancel_develop(const std::string& path);
    // A stand-in (a raw's embedded JPEG) need only cover this many pixels:
    // it's decoded smaller when it can be. 0: full size.
    void set_stand_in_fit(int width, int height);
    unsigned workers() const { return workers_; }

private:
    void run_foreground();
    void run_reader();
    void run_worker(unsigned index);
    enum class Lane
    {
        Foreground,
        Develop,
        Prefetch
    };
    void post(Result* r, Lane lane);
    void decode_into(Result& r);

    struct Job
    {
        uint64_t generation;
        std::string path;
        std::shared_ptr<const FileBlob> file;  // read, or null for the reader to read
    };
    // The file read for `path` and waiting for a worker, if any, taken from
    // the queue. Under the lock.
    std::shared_ptr<const FileBlob> take_read(const std::string& path);
    bool busy_with(const std::string& path) const;  // being read, or waiting or decoded in full. Under the lock.

    Callback callback_;
    Dispatch dispatch_;
    std::mutex mutex_;
    std::condition_variable fg_wake_, reader_wake_, worker_wake_;
    std::optional<Job> pending_;            // foreground
    std::optional<Job> develop_;            // the image on screen's, ahead of prefetches
    std::vector<std::string> prefetch_;     // still to read, nearest first
    std::vector<std::string> order_;        // the latest prefetch list: the order the read ones are decoded in
    std::vector<Job> read_;                 // prefetches read, waiting for a worker
    std::string reading_;                   // the reader's file now
    std::vector<std::string> developing_;   // full decodes running (develops, and prefetches that go as far)
    std::vector<std::string> in_flight_;    // every decode the workers are running
    unsigned workers_ = 1;
    int fit_w_ = 0, fit_h_ = 0;  // set_stand_in_fit
    bool stop_ = false;
    std::thread foreground_, reader_;
    std::vector<std::thread> pool_;
};
