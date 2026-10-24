#pragma once

#include <string>
#include <vector>

struct Config;

// The sorted list of viewable files in one directory, with a cursor.
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

    bool empty() const { return files_.empty(); }
    int count() const { return int(files_.size()); }
    int index() const { return index_; }
    const std::string& directory() const { return dir_; }
    std::string current() const;       // full path, or empty
    std::string current_name() const;  // file name only
    const std::string& name_at(int index) const { return files_[index]; }

    bool step(int delta, bool wrap);  // false when there is nowhere to go
    bool go(int index);

private:
    std::string dir_;
    std::vector<std::string> files_;  // names, sorted
    int index_ = -1;
};
