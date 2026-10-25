#include "folder.h"

#include "config.h"
#include "decoders/decoder.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <utility>

namespace
{

std::string join(const std::string& a, const std::string& b)
{
    gchar* full = g_build_filename(a.c_str(), b.c_str(), nullptr);
    std::string s = full;
    g_free(full);
    return s;
}

// Whether `name` (relative to `dir`) is one the list shows.
bool qualifies(const std::string& dir, const std::string& name, const Config& cfg)
{
    const size_t base = name.find_last_of(G_DIR_SEPARATOR_S "/");
    if (!cfg.show_hidden && name[base == std::string::npos ? 0 : base + 1] == '.')
        return false;
    const std::string ext = file_extension(name);
    if (!cfg.accepts_extension(ext) || !extension_supported(ext))  // e.g. videos without GStreamer
        return false;
    return g_file_test(join(dir, name).c_str(), G_FILE_TEST_IS_REGULAR);
}

std::string collate_key(const std::string& name)
{
    gchar* key = g_utf8_collate_key_for_filename(name.c_str(), -1);
    std::string k = key;
    g_free(key);
    return k;
}

bool is_separator(char c)
{
    return c == '/' || c == G_DIR_SEPARATOR;
}

// The list's order, for a name relative to the directory: its path from
// there, a folder at a time, each by name as a file manager sorts them;
// with `files_first`, a folder's own files before the folders in it, else
// the two alike.
std::string sort_key(const std::string& name, bool files_first)
{
    std::string key;
    size_t from = 0;
    for (size_t i = 0; i <= name.size(); ++i)
    {
        const bool file = i == name.size();
        if (!file && !is_separator(name[i]))
            continue;
        if (files_first)
            key += file ? '\x01' : '\x02';
        key += collate_key(name.substr(from, i - from)) + '\0';  // a shorter name first: "a" before "ab"
        from = i + 1;
    }
    return key;
}

// Under `dir`, from its folder `rel` (relative; empty: `dir` itself), the
// files the list shows, and (cfg.recursive) those of the folders inside,
// not following links (no loops) or hidden folders unless hidden files show.
void collect(const std::string& dir, const std::string& rel, const Config& cfg, std::vector<std::string>& out, int depth, const std::function<bool()>& stop)
{
    if (stop && stop())
        return;
    const std::string here = rel.empty() ? dir : join(dir, rel);
    GDir* d = g_dir_open(here.c_str(), 0, nullptr);
    if (!d)
        return;
    while (const gchar* entry = g_dir_read_name(d))
    {
        const std::string name = rel.empty() ? std::string(entry) : join(rel, entry);
        if (qualifies(dir, name, cfg))
            out.push_back(name);
        else if (cfg.recursive && depth < 64 && (cfg.show_hidden || entry[0] != '.'))
        {
            const std::string full = join(dir, name);
            if (g_file_test(full.c_str(), G_FILE_TEST_IS_DIR) && !g_file_test(full.c_str(), G_FILE_TEST_IS_SYMLINK))
                collect(dir, name, cfg, out, depth + 1, stop);
        }
    }
    g_dir_close(d);
}

std::vector<std::string> list_images(const std::string& dir, const Config& cfg, const std::function<bool()>& stop)
{
    std::vector<std::string> names;
    collect(dir, "", cfg, names, 0, stop);
    std::vector<std::pair<std::string, std::string>> keyed;  // sort key, name
    keyed.reserve(names.size());
    for (std::string& name : names)
        keyed.emplace_back(sort_key(name, cfg.files_first), std::move(name));
    std::sort(keyed.begin(), keyed.end());
    names.clear();
    for (auto& [key, name] : keyed)
        names.push_back(std::move(name));
    return names;
}

}  // namespace

Folder::Listing Folder::list(const std::string& path, const Config& cfg, const std::function<bool()>& stop)
{
    Listing l;
    if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR))
        l.dir = path;
    else
    {
        gchar* d = g_path_get_dirname(path.c_str());
        gchar* b = g_path_get_basename(path.c_str());
        l.dir = d;
        l.want = b;
        g_free(d);
        g_free(b);
        l.opened = g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR);
    }
    l.files = list_images(l.dir, cfg, stop);
    return l;
}

void Folder::adopt(Listing l)
{
    const bool same = l.dir == dir_;
    const int was = index_;
    dir_ = std::move(l.dir);
    files_ = std::move(l.files);
    gone_ = false;
    if (auto it = std::find(files_.begin(), files_.end(), l.want); !l.want.empty() && it != files_.end())
        index_ = int(it - files_.begin());
    else if (l.opened)
    {
        // Opened explicitly, so show it even though it isn't in the filter.
        files_.insert(files_.begin(), l.want);
        index_ = 0;
    }
    else
        index_ = files_.empty() ? -1 : same ? std::clamp(was, 0, count() - 1) : 0;
}

void Folder::scan(const std::string& path, const Config& cfg)
{
    adopt(list(path, cfg));
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
    Listing l = list(dir_, cfg);
    l.want = current_name();
    adopt(std::move(l));
}

std::string Folder::current() const
{
    return index_ < 0 ? std::string() : path_at(index_);
}

std::string Folder::path_at(int index) const
{
    return join(dir_, files_[index]);
}

std::string Folder::current_name() const
{
    return index_ < 0 ? std::string() : files_[index_];
}

bool Folder::contains(const std::string& name) const
{
    return std::find(files_.begin(), files_.end(), name) != files_.end();
}

bool Folder::contains_under(const std::string& folder) const
{
    return std::any_of(files_.begin(), files_.end(), [&](const std::string& f)
                       { return f.size() > folder.size() && f.compare(0, folder.size(), folder) == 0 && is_separator(f[folder.size()]); });
}

bool Folder::add(const std::string& name, const Config& cfg)
{
    if (dir_.empty() || contains(name) || !qualifies(dir_, name, cfg))
        return false;
    const std::string key = sort_key(name, cfg.files_first);
    auto at = std::find_if(files_.begin(), files_.end(), [&](const std::string& f) { return sort_key(f, cfg.files_first) > key; });
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
    const std::string key = sort_key(to, cfg.files_first);
    auto at = std::find_if(files_.begin(), files_.end(), [&](const std::string& f) { return sort_key(f, cfg.files_first) > key; });
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
