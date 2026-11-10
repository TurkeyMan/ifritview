#pragma once
// Small square pictures of a folder's photos, for the map's pins, made on a
// thread of their own: from the preview stored in the file's metadata if it
// has one, else, for an image the platform can decode and not too big, from
// the file. Kept in the cache folder once made, up to 256 MB, so they're there
// at once the next time.

#include <rapport/rapport.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// --check-forget: a picture being made as they're forgotten isn't kept, nor
// handed back as current, with the pictures kept in `dir`. 0 if so.
int check_thumbs_forget(const std::string& dir);

class ThumbLoader
{
public:
    struct Request
    {
        std::string key, path;
        int orientation = 1;  // EXIF 1..8
    };
    struct Picture
    {
        int side = 0;  // pixels: it's square
        rap::pixel_format format{};
        std::vector<uint8_t> pixels;  // side * 4 bytes a row
    };
    // On the loader's thread, for each request; `picture` null if none could
    // be made. `generation`: forget()'s count when it was asked for.
    using Done = std::function<void(const std::string& key, std::shared_ptr<Picture> picture, uint64_t generation)>;

    explicit ThumbLoader(Done done);
    ~ThumbLoader();  // waits for its thread
    ThumbLoader(const ThumbLoader&) = delete;
    ThumbLoader& operator=(const ThumbLoader&) = delete;

    // What's wanted now, most wanted first, in place of what was.
    void want(std::vector<Request> requests);

    // Deletes the pictures kept in the cache folder between runs: on the
    // loader's thread, after the one it's making, so nothing it writes is
    // left. What was asked for is dropped, and what's on its way comes with
    // an older generation than the one returned. `done` runs on the loader's
    // thread (or as it closes), told whether they're all gone.
    uint64_t forget(std::function<void(bool deleted)> done = {});
    // The same with no loader, so nothing's being written.
    static bool forget_kept();

    // For --check-forget: kept in `dir`, not the cache folder. Before any loader.
    static void keep_in(std::string dir);

private:
    void run();

    Done done_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Request> queue_;
    std::string working_;  // the key being made
    bool stop_ = false;
    bool forget_ = false;
    std::vector<std::function<void(bool)>> forgotten_;
    uint64_t generation_ = 0;
    std::thread thread_;
};
