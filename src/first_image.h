#pragma once
// The image named on the command line, decoded from the very start of the
// process, before the toolkit or the window exist, so the window can open
// with it already in. Decoded as the loader would: a raw's embedded preview
// first (its develop follows through the loader), anything else in full.

#include "image.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class FirstImage
{
public:
    // Starts decoding `path` on a thread of its own.
    explicit FirstImage(std::string path);
    ~FirstImage();  // waits for the thread
    FirstImage(const FirstImage&) = delete;
    FirstImage& operator=(const FirstImage&) = delete;

    const std::string& path() const { return path_; }

    // Waits up to `timeout` for the decode; true once it's done.
    bool wait_for(std::chrono::milliseconds timeout);

    // Once done: the image (null on failure, with error()), and whether it's
    // a stand-in (a raw's preview) whose develop is still to come.
    std::shared_ptr<Image> image() const { return image_; }
    const std::string& error() const { return error_; }
    bool preview() const { return preview_; }

    // When it isn't done yet: `done` runs on the decoding thread when it is
    // (at once if it already is). For handing the result to the UI thread.
    void on_done(std::function<void()> done);

private:
    std::string path_;
    std::shared_ptr<Image> image_;
    std::string error_;
    bool preview_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::function<void()> on_done_;
    std::thread thread_;
};
