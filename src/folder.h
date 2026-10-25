#pragma once

#include <string>
#include <vector>

struct Config;

// The sorted list of viewable files in one directory (Config::recursive:
// and in the folders inside it, in order of their paths from it), with a
// cursor. Names are relative to the directory.
class Folder
{
public:
    // `path` may be a file (which becomes current if it qualifies) or a
    // directory (cursor goes to the first file).
    void scan(const std::string& path, const Config& cfg);
    // Just this one file, until it's scanned: for showing it before the
    // folder has been listed.
    void set_single(const std::string& file);
    // Re-list the current directory, keeping the cursor on the same file if possible.
    void rescan(const Config& cfg);

    // A change in the directory, by file name. Each returns whether the list
    // changed, the cursor staying on the same file. The current file, gone,
    // stays listed while it's shown, and goes on the next step, which counts
    // from where it was.
    bool add(const std::string& name, const Config& cfg);  // if it's one the list would have
    bool remove(const std::string& name);
    bool rename(const std::string& from, const std::string& to, const Config& cfg);
    bool contains(const std::string& name) const;
    bool contains_under(const std::string& folder) const;  // any file in `folder` (relative), or the folders inside it

    bool empty() const { return files_.empty(); }
    int count() const { return int(files_.size()); }
    int index() const { return index_; }
    const std::string& directory() const { return dir_; }
    std::string current() const;       // full path, or empty
    std::string current_name() const;  // its name, relative to directory()
    const std::string& name_at(int index) const { return files_[index]; }
    std::string path_at(int index) const;  // full path, as current() gives it (the cache's key)

    bool step(int delta, bool wrap);  // false when there is nowhere to go
    bool go(int index);

private:
    void drop_gone(int& target);  // the shown file, gone: out of the list, `target` kept on its file

    std::string dir_;
    std::vector<std::string> files_;  // names relative to dir_, sorted (sort_key)
    int index_ = -1;
    bool gone_ = false;  // the current file was removed while shown
};
