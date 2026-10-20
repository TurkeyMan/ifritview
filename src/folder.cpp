#include "folder.h"

#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <utility>

namespace {

std::string lowercase_extension(const std::string& name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

std::vector<std::string> list_images(const std::string& dir, const Config& cfg) {
    std::vector<std::pair<std::string, std::string>> keyed;  // collate key, name
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return {};
    while (const gchar* name = g_dir_read_name(d)) {
        if (!cfg.show_hidden && name[0] == '.') continue;
        if (!cfg.accepts_extension(lowercase_extension(name))) continue;
        gchar* full = g_build_filename(dir.c_str(), name, nullptr);
        const bool regular = g_file_test(full, G_FILE_TEST_IS_REGULAR);
        g_free(full);
        if (!regular) continue;
        gchar* key = g_utf8_collate_key_for_filename(name, -1);
        keyed.emplace_back(key, name);
        g_free(key);
    }
    g_dir_close(d);
    std::sort(keyed.begin(), keyed.end());
    std::vector<std::string> names;
    names.reserve(keyed.size());
    for (auto& [key, name] : keyed) names.push_back(std::move(name));
    return names;
}

}  // namespace

void Folder::scan(const std::string& path, const Config& cfg) {
    std::string want;
    if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) {
        dir_ = path;
    } else {
        gchar* d = g_path_get_dirname(path.c_str());
        gchar* b = g_path_get_basename(path.c_str());
        dir_ = d;
        want = b;
        g_free(d);
        g_free(b);
    }
    files_ = list_images(dir_, cfg);
    index_ = files_.empty() ? -1 : 0;
    if (!want.empty()) {
        auto it = std::find(files_.begin(), files_.end(), want);
        if (it != files_.end()) {
            index_ = int(it - files_.begin());
        } else if (g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) {
            // Opened explicitly, so show it even though it isn't in the filter.
            files_.insert(files_.begin(), want);
            index_ = 0;
        }
    }
}

void Folder::rescan(const Config& cfg) {
    if (dir_.empty()) return;
    const std::string keep = index_ >= 0 ? files_[index_] : "";
    files_ = list_images(dir_, cfg);
    auto it = std::find(files_.begin(), files_.end(), keep);
    if (it != files_.end()) index_ = int(it - files_.begin());
    else index_ = files_.empty() ? -1 : std::min(std::max(index_, 0), count() - 1);
}

std::string Folder::current() const {
    if (index_ < 0) return {};
    gchar* full = g_build_filename(dir_.c_str(), files_[index_].c_str(), nullptr);
    std::string s = full;
    g_free(full);
    return s;
}

std::string Folder::current_name() const {
    return index_ < 0 ? std::string() : files_[index_];
}

bool Folder::step(int delta, bool wrap) {
    if (files_.empty()) return false;
    int next = index_ + delta;
    if (wrap) {
        next = ((next % count()) + count()) % count();
    } else if (next < 0 || next >= count()) {
        return false;
    }
    if (next == index_) return false;
    index_ = next;
    return true;
}

bool Folder::go(int index) {
    if (index < 0 || index >= count() || index == index_) return false;
    index_ = index;
    return true;
}
