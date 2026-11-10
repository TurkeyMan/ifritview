#pragma once
// What's known of a folder's files without opening them properly: a small
// record each, read from the file's metadata once and kept on disk (one
// database in the cache folder, a packed block per directory), so a folder
// seen before is known at once. It's only a cache: damaged, or from another
// version, it's thrown away and the files read again.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FileRecord;

// --check-forget: forgetting refused while another connection holds an old
// snapshot, finished once it lets go and on closing, and nothing written
// back by what was queued; with a database in `dir`. 0 if so.
int check_index_forget(const std::string& dir);

// From the file's first bytes (for HEIF, the file): false if it can't be read.
bool read_file_record(const std::string& path, FileRecord& record);
// The preview stored in the file's metadata; empty if it has none.
std::vector<uint8_t> embedded_thumbnail(const std::string& path);

class FolderIndex
{
public:
    struct Located
    {
        std::string path;
        double latitude = 0, longitude = 0;
        int orientation = 1;
        int64_t mtime = 0;
    };
    // On the index's thread: more of what's been found, and whether that's all.
    using Report = std::function<void(std::vector<Located> found, bool done)>;

    // `database`: where it's kept, if not in the cache folder.
    explicit FolderIndex(std::string database = database_path());
    ~FolderIndex();  // waits for its thread
    FolderIndex(const FolderIndex&) = delete;
    FolderIndex& operator=(const FolderIndex&) = delete;

    // The geotags of `paths`. Replaces a scan still going, whose progress is
    // kept for next time; only files that have changed are read.
    void scan(std::vector<std::string> paths, Report report);
    // Stops what's going (what it's read is kept), nothing more reported.
    void cancel();
    // Stops what's going, scans and updates alike, and empties the database,
    // on the index's thread and before anything asked for after this; `done`
    // then runs there, told whether it was emptied. Not if another connection
    // is still reading what was there, which keeps it in the journal: then
    // it's tried again whenever the index has nothing else to do, as it
    // closes, and by the next to open the database. Done before closing,
    // too, if it hadn't been reached.
    void forget(std::function<void(bool emptied)> done = {});

    // Where it's kept; there's nothing to forget while there's no such file.
    static std::string database_path();

    // Some files changed or went: just those looked at again. `removed` has
    // every path the map should drop (gone, and changed ones, which come back
    // in `found` if they still have a position). Queued behind a scan, and
    // dropped by the next scan or cancel, which cover them.
    using Changes = std::function<void(std::vector<std::string> removed, std::vector<Located> found)>;
    void update(std::vector<std::string> changed, std::vector<std::string> gone, Changes report);

private:
    struct Job
    {
        std::vector<std::string> paths;
        Report report;
        uint64_t serial = 0;
    };
    struct Update
    {
        std::vector<std::string> changed, gone;
        Changes report;
        uint64_t serial = 0;  // the scan it follows: another scan, or a cancel, ends it
    };
    struct Store;

    void run();
    void work(Store& store, const Job& job);
    void work(Store& store, const Update& update);
    bool stale(const Job& job) const { return stop_ || serial_ != job.serial; }

    std::string database_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool have_job_ = false;
    Job job_;
    std::deque<Update> updates_;
    bool forget_ = false;
    std::vector<std::function<void(bool)>> forgotten_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> serial_{0};
    std::thread thread_;
};
