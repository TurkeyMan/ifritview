#include "folder.h"

#include "config.h"
#include "decoders/decoder.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <utility>

namespace
{

// Whether `name` in `dir` is one the list shows.
bool qualifies(const std::string& dir, const char* name, const Config& cfg)
{
    if (!cfg.show_hidden && name[0] == '.')
        return false;
    const std::string ext = file_extension(name);
    if (!cfg.accepts_extension(ext) || !extension_supported(ext))  // e.g. videos without GStreamer
        return false;
    gchar* full = g_build_filename(dir.c_str(), name, nullptr);
    const bool regular = g_file_test(full, G_FILE_TEST_IS_REGULAR);
    g_free(full);
    return regular;
}

std::string collate_key(const std::string& name)
{
    gchar* key = g_utf8_collate_key_for_filename(name.c_str(), -1);
    std::string k = key;
    g_free(key);
    return k;
}

std::vector<std::string> list_images(const std::string& dir, const Config& cfg)
{
    std::vector<std::pair<std::string, std::string>> keyed;  // collate key, name
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d)
        return {};
    while (const gchar* name = g_dir_read_name(d))
        if (qualifies(dir, name, cfg))
            keyed.emplace_back(collate_key(name), name);
    g_dir_close(d);
    std::sort(keyed.begin(), keyed.end());
    std::vector<std::string> names;
    names.reserve(keyed.size());
    for (auto& [key, name] : keyed)
        names.push_back(std::move(name));
    return names;
}

}  // namespace

void Folder::scan(const std::string& path, const Config& cfg)
{
    std::string want;
    if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR))
    {
        dir_ = path;
    }
    else
    {
        gchar* d = g_path_get_dirname(path.c_str());
        gchar* b = g_path_get_basename(path.c_str());
        dir_ = d;
        want = b;
        g_free(d);
        g_free(b);
    }
    files_ = list_images(dir_, cfg);
    index_ = files_.empty() ? -1 : 0;
    gone_ = false;
    if (!want.empty())
    {
        auto it = std::find(files_.begin(), files_.end(), want);
        if (it != files_.end())
        {
            index_ = int(it - files_.begin());
        }
        else if (g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR))
        {
            // Opened explicitly, so show it even though it isn't in the filter.
            files_.insert(files_.begin(), want);
            index_ = 0;
        }
    }
}

void Folder::set_single(const std::string& file)
{
    gchar* d = g_path_get_dirname(file.c_str());
    gchar* b = g_path_get_basename(file.c_str());
    dir_ = d;
    files_ = {b};
    index_ = 0;
    gone_ = false;
    g_free(d);
    g_free(b);
}

void Folder::rescan(const Config& cfg)
{
    if (dir_.empty())
        return;
    const std::string keep = index_ >= 0 ? files_[index_] : "";
    files_ = list_images(dir_, cfg);
    gone_ = false;
    auto it = std::find(files_.begin(), files_.end(), keep);
    if (it != files_.end())
        index_ = int(it - files_.begin());
    else
        index_ = files_.empty() ? -1 : std::min(std::max(index_, 0), count() - 1);
}

std::string Folder::current() const
{
    if (index_ < 0)
        return {};
    gchar* full = g_build_filename(dir_.c_str(), files_[index_].c_str(), nullptr);
    std::string s = full;
    g_free(full);
    return s;
}

std::string Folder::current_name() const
{
    return index_ < 0 ? std::string() : files_[index_];
}

bool Folder::contains(const std::string& name) const
{
    return std::find(files_.begin(), files_.end(), name) != files_.end();
}

bool Folder::add(const std::string& name, const Config& cfg)
{
    if (dir_.empty() || contains(name) || !qualifies(dir_, name.c_str(), cfg))
        return false;
    const std::string key = collate_key(name);
    auto at = std::find_if(files_.begin(), files_.end(), [&](const std::string& f) { return collate_key(f) > key; });
    const int i = int(at - files_.begin());
    files_.insert(at, name);
    if (index_ < 0)
        index_ = 0;
    else if (i <= index_)
        ++index_;
    return true;
}

bool Folder::remove(const std::string& name)
{
    auto it = std::find(files_.begin(), files_.end(), name);
    if (it == files_.end())
        return false;
    const int i = int(it - files_.begin());
    if (i == index_)
    {
        gone_ = true;  // still shown: it goes on the next step
        return true;
    }
    files_.erase(it);
    if (i < index_)
        --index_;
    return true;
}

bool Folder::rename(const std::string& from, const std::string& to, const Config& cfg)
{
    auto it = std::find(files_.begin(), files_.end(), from);
    if (it == files_.end())
        return add(to, cfg);
    if (int(it - files_.begin()) != index_)
        return remove(from) | add(to, cfg);
    // The current file: it stays current under its new name, in its new place.
    files_.erase(it);
    const std::string key = collate_key(to);
    auto at = std::find_if(files_.begin(), files_.end(), [&](const std::string& f) { return collate_key(f) > key; });
    index_ = int(at - files_.begin());
    files_.insert(at, to);
    return true;
}

void Folder::drop_gone(int& target)
{
    if (!gone_ || index_ < 0)
        return;
    gone_ = false;
    files_.erase(files_.begin() + index_);
    if (target > index_)
        --target;
    index_ = files_.empty() ? -1 : std::min(index_, count() - 1);
}

bool Folder::step(int delta, bool wrap)
{
    if (gone_)
    {
        // From where the gone file was: forward lands on the one after it.
        int next = index_ + delta;
        if (wrap && !files_.empty())
            next = ((next % count()) + count()) % count();
        else if (next < 0 || next >= count())
            return false;
        drop_gone(next);
        if (files_.empty())
            return false;
        index_ = next;
        return true;
    }
    if (files_.empty())
        return false;
    int next = index_ + delta;
    if (wrap)
    {
        next = ((next % count()) + count()) % count();
    }
    else if (next < 0 || next >= count())
    {
        return false;
    }
    if (next == index_)
        return false;
    index_ = next;
    return true;
}

bool Folder::go(int index)
{
    if (index < 0 || index >= count() || index == index_)
        return false;
    drop_gone(index);
    index_ = index;
    return true;
}
