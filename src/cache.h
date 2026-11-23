#pragma once

#include "image.h"

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>

// Decoded images kept around so stepping back and forth through a folder
// is instant. Least recently used entries go first when over budget.
class ImageCache
{
public:
    // What a raw's entry holds: the camera's embedded JPEG (shown instead of
    // a develop), or a develop (or the stand-in waiting for one) by the GPU
    // or on the CPU (LibRaw). Anything that isn't a raw is Other.
    enum class Source : uint8_t
    {
        Other,
        CameraJpeg,
        GpuDevelop,
        CpuDevelop
    };
    struct Entry
    {
        std::shared_ptr<Image> image;
        FileStamp stamp;  // the file as read for it: a different size or time and it's out of date
        Source source = Source::Other;
    };

    // Budget in bytes; 0 picks a share of the memory available right now.
    explicit ImageCache(size_t budget = 0);

    // `soon` false: the first to go when over budget (an image left behind
    // while flicking ahead), not the last.
    void put(const std::string& path, Entry entry, bool soon = true);
    bool get(const std::string& path, Entry& entry);  // refreshes recency, checks the file's still as read
    bool contains(const std::string& path) const;
    bool peek(const std::string& path, Entry& entry) const;  // as get, but not counted as a use
    void erase(const std::string& path);
    // Every entry not usable for `wanted` (the view's mode changed); how many went.
    size_t erase_unusable(Source wanted);
    // Whether `entry` shows a raw as it's wanted now: the camera's JPEG
    // while that's chosen (or the develop of a raw with none), else the
    // develop that's chosen or its stand-in. Anything else always does.
    static bool usable(const Entry& entry, Source wanted);
    void clear();
    size_t bytes() const { return bytes_; }
    size_t budget() const { return budget_; }
    void set_budget(size_t budget);

    static size_t image_bytes(const Image& image);
    static size_t automatic_budget();

private:
    void trim();

    size_t budget_ = 0;
    size_t bytes_ = 0;
    std::list<std::string> order_;  // most recent first
    struct Slot
    {
        Entry entry;
        size_t bytes;
        std::list<std::string>::iterator where;
    };
    std::unordered_map<std::string, Slot> slots_;
};
