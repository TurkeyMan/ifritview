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
    struct Entry
    {
        std::shared_ptr<Image> image;
        int64_t mtime = 0;  // file modification time when decoded
    };

    // Budget in bytes; 0 picks a share of the memory available right now.
    explicit ImageCache(size_t budget = 0);

    void put(const std::string& path, Entry entry);
    bool get(const std::string& path, Entry& entry);  // refreshes recency, checks mtime
    bool contains(const std::string& path) const;
    void erase(const std::string& path);
    void clear();
    size_t bytes() const { return bytes_; }
    size_t budget() const { return budget_; }
    void set_budget(size_t budget);

    static size_t image_bytes(const Image& image);
    static int64_t file_mtime(const std::string& path);
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
