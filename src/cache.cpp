#include "cache.h"

#include "platform.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

ImageCache::ImageCache(size_t budget) : budget_(budget ? budget : automatic_budget()) {}

size_t ImageCache::image_bytes(const Image& image)
{
    size_t n = image.pixels.size();
    if (image.linear)
        n += image.linear->rgb.size() * sizeof(uint16_t);
    if (image.mosaic && image.mosaic->data)
        n += image.mosaic->data->size() * sizeof(uint16_t);
    if (image.animation)
        for (const Animation::Frame& f : image.animation->frames)
            n += f.pixels.size();
    return n;
}

int64_t ImageCache::file_mtime(const std::string& path)
{
    GStatBuf st;
    return g_stat(path.c_str(), &st) == 0 ? int64_t(st.st_mtime) : 0;
}

// A quarter of what the system has free, but at least 1 GB (a developed
// 24 MP raw is ~240 MB) and never more than half of what's free, capped at 8 GB.
size_t ImageCache::automatic_budget()
{
    const size_t available = platform::available_memory();
    const size_t quarter = available / 4, half = available / 2;
    return std::min<size_t>(std::max<size_t>(quarter, std::min<size_t>(size_t(1) << 30, half)), size_t(8) << 30);
}

void ImageCache::set_budget(size_t budget)
{
    budget_ = budget ? budget : automatic_budget();
    trim();
}

void ImageCache::put(const std::string& path, Entry entry, bool soon)
{
    erase(path);
    if (!entry.image)
        return;
    const size_t n = image_bytes(*entry.image);
    if (n > budget_)
        return;
    const auto where = soon ? order_.insert(order_.begin(), path) : order_.insert(order_.end(), path);
    slots_[path] = Slot{std::move(entry), n, where};
    bytes_ += n;
    trim();
}

bool ImageCache::get(const std::string& path, Entry& entry)
{
    auto it = slots_.find(path);
    if (it == slots_.end())
        return false;
    if (it->second.entry.mtime != file_mtime(key_file(path)))  // `path` may be a part's key
    {  // changed on disk since
        erase(path);
        return false;
    }
    order_.erase(it->second.where);
    order_.push_front(path);
    it->second.where = order_.begin();
    entry = it->second.entry;
    return true;
}

bool ImageCache::peek(const std::string& path, Entry& entry) const
{
    auto it = slots_.find(path);
    if (it == slots_.end())
        return false;
    entry = it->second.entry;
    return true;
}

bool ImageCache::contains(const std::string& path) const
{
    return slots_.count(path) != 0;
}

void ImageCache::erase(const std::string& path)
{
    auto it = slots_.find(path);
    if (it == slots_.end())
        return;
    bytes_ -= it->second.bytes;
    order_.erase(it->second.where);
    slots_.erase(it);
}

void ImageCache::clear()
{
    slots_.clear();
    order_.clear();
    bytes_ = 0;
}

void ImageCache::trim()
{
    while (bytes_ > budget_ && !order_.empty())
        erase(order_.back());
}
