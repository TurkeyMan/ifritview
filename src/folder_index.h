#pragma once
// What's known of a folder's files without opening them properly: a small
// record each, read from the file's metadata once and kept on disk (one
// database in the cache folder, a packed block per directory), so a folder
// seen before is known at once. It's only a cache: damaged, or from another
// version, it's thrown away and the files read again.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FileRecord;

// From the file's first bytes (for HEIF, the file): false if it can't be read.
bool read_file_record(const std::string& path, FileRecord& record);

class FolderIndex
{
public:
    struct Located
    {
        std::string path;
        double latitude = 0, longitude = 0;
    };
    // On the index's thread: more of what's been found, and whether that's all.
    using Report = std::function<void(std::vector<Located> found, bool done)>;

    FolderIndex();
    ~FolderIndex();  // waits for its thread
    FolderIndex(const FolderIndex&) = delete;
    FolderIndex& operator=(const FolderIndex&) = delete;

    // The geotags of `paths`. Replaces a scan still going, whose progress is
    // kept for next time; only files that have changed are read.
    void scan(std::vector<std::string> paths, Report report);
    // Stops what's going (what it's read is kept), nothing more reported.
    void cancel();

private:
    struct Job
    {
        std::vector<std::string> paths;
        Report report;
        uint64_t serial = 0;
    };
    struct Store;

    void run();
    void work(Store& store, const Job& job);
    bool stale(const Job& job) const { return stop_ || serial_ != job.serial; }

    std::mutex mutex_;
    std::condition_variable wake_;
    bool have_job_ = false;
    Job job_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> serial_{0};
    std::thread thread_;
};
