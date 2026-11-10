#include "folder_index.h"

#include "folder_block.h"
#include "image.h"
#include "platform.h"
#include "decoders/decoder.h"
#include "metadata/file_metadata.h"

#include <rapport/sys.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{

constexpr size_t kHead = size_t(1) << 20;  // where a file's metadata is, nearly always
constexpr int kSchema = 2;  // the blocks' times in milliseconds
constexpr int64_t kBudget = int64_t(64) << 20;
constexpr int64_t kDay = 86400;

// The file's first `limit` bytes; empty if it can't be read.
std::string read_bytes(const fs::path& path, size_t limit)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    if (limit)
    {
        std::string head(limit, '\0');
        in.read(head.data(), std::streamsize(limit));
        if (in.bad())
            return {};
        head.resize(size_t(in.gcount()));
        return head;
    }
    return {};
}

uint32_t big32(const uint8_t* d)
{
    return uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3];
}

// A JPEG's frame header, past its other segments.
void jpeg_size(const uint8_t* d, size_t n, uint32_t& width, uint32_t& height)
{
    size_t at = 2;
    while (at + 9 < n && d[at] == 0xFF)
    {
        const uint8_t marker = d[at + 1];
        if (marker == 0xFF)
        {
            ++at;
            continue;
        }
        const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (frame)
        {
            height = uint32_t(d[at + 5]) << 8 | d[at + 6];
            width = uint32_t(d[at + 7]) << 8 | d[at + 8];
            return;
        }
        if (marker == 0xDA)
            return;
        at += 2 + (size_t(d[at + 2]) << 8 | d[at + 3]);
    }
}

bool is_tiff(const uint8_t* d, size_t n)
{
    return n >= 8 && (std::memcmp(d, "II*\0", 4) == 0 || std::memcmp(d, "MM\0*", 4) == 0);
}

// The file type box of a HEIF; AVIF and HEIC alike, not a CR3 or an MP4.
bool is_heif(const uint8_t* d, size_t n)
{
    if (n < 12 || std::memcmp(d + 4, "ftyp", 4) != 0)
        return false;
    static const char* const brands[] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1", "avif", "avis"};
    return std::any_of(std::begin(brands), std::end(brands), [&](const char* b) { return std::memcmp(d + 8, b, 4) == 0; });
}

// Lower case on Windows, whose paths don't care, with one kind of slash.
std::string directory_key(const fs::path& dir)
{
    std::string key = rap::sys::path_to_utf8(dir.lexically_normal().generic_u8string());
#ifdef _WIN32
    gchar* lower = g_utf8_strdown(key.c_str(), -1);
    key = lower;
    g_free(lower);
#endif
    return key;
}

int64_t millis_of(fs::file_time_type t)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

}  // namespace

bool read_file_record(const std::string& path, FileRecord& record)
{
#ifdef HAVE_EXIF
    const fs::path p = rap::sys::path_from_utf8(path);
    const std::string bytes = read_bytes(p, kHead);
    if (bytes.empty())
        return false;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t n = bytes.size();

    std::vector<uint8_t> exif;
    if (is_tiff(d, n))
        exif.assign(d, d + n);
#ifdef HAVE_HEIF
    else if (is_heif(d, n))
    {
        int w = 0, h = 0;
        if (!heif_stored_exif(path, exif, w, h))  // its metadata can be anywhere in it
            return false;
        record.width = uint32_t(w);
        record.height = uint32_t(h);
    }
#endif
    else
    {
        exif = extract_metadata(d, n).exif;
        if (n > 3 && d[0] == 0xFF && d[1] == 0xD8)
            jpeg_size(d, n, record.width, record.height);
        else if (n >= 24 && std::memcmp(d, "\x89PNG", 4) == 0)
        {
            record.width = big32(d + 16);
            record.height = big32(d + 20);
        }
    }
    if (exif.empty())
        return true;
    Image parsed;
    const ExifSummary summary = read_exif_blob(exif.data(), exif.size(), parsed);
    record.orientation = uint8_t(summary.orientation);
    record.camera = summary.camera;
    record.taken = summary.taken;
    record.has_location = parsed.has_location;
    record.latitude = parsed.latitude;
    record.longitude = parsed.longitude;
    return true;
#else
    (void)path;
    (void)record;
    return false;
#endif
}

std::vector<uint8_t> embedded_thumbnail(const std::string& path)
{
#ifdef HAVE_EXIF
    const std::string bytes = read_bytes(rap::sys::path_from_utf8(path), kHead);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t n = bytes.size();
    std::vector<uint8_t> exif;
    if (is_tiff(d, n))
        exif.assign(d, d + n);
#ifdef HAVE_HEIF
    else if (is_heif(d, n))
    {
        int w = 0, h = 0;
        heif_stored_exif(path, exif, w, h);
    }
#endif
    else if (n)
        exif = extract_metadata(d, n).exif;
    return exif.empty() ? std::vector<uint8_t>{} : exif_thumbnail(exif.data(), exif.size());
#else
    (void)path;
    return {};
#endif
}

// The database: a block per directory, opened on the index's thread. Any
// trouble with it just means working without it.
struct FolderIndex::Store
{
    sqlite3* db = nullptr;
    sqlite3_stmt* get = nullptr;
    sqlite3_stmt* put = nullptr;
    sqlite3_stmt* touch = nullptr;
    int writes = 0;

    ~Store() { close(); }

    void close()
    {
        sqlite3_finalize(get);
        sqlite3_finalize(put);
        sqlite3_finalize(touch);
        sqlite3_close(db);
        get = put = touch = nullptr;
        db = nullptr;
    }

    bool exec(const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

    bool open_file(const std::string& path)
    {
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
            return false;
        sqlite3_busy_timeout(db, 1000);
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=NORMAL");
        int version = 0;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) != SQLITE_OK)
            return false;
        if (sqlite3_step(st) == SQLITE_ROW)
            version = sqlite3_column_int(st, 0);
        else
        {
            sqlite3_finalize(st);
            return false;
        }
        sqlite3_finalize(st);
        if (version != kSchema && !exec("DROP TABLE IF EXISTS dirs"))
            return false;
        char set[48];
        std::snprintf(set, sizeof set, "PRAGMA user_version=%d", kSchema);
        return exec("CREATE TABLE IF NOT EXISTS dirs(path TEXT PRIMARY KEY, used INTEGER NOT NULL, block BLOB NOT NULL) WITHOUT ROWID") && exec(set) &&
               sqlite3_prepare_v2(db, "SELECT used, block FROM dirs WHERE path=?", -1, &get, nullptr) == SQLITE_OK &&
               sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO dirs(path, used, block) VALUES(?,?,?)", -1, &put, nullptr) == SQLITE_OK &&
               sqlite3_prepare_v2(db, "UPDATE dirs SET used=? WHERE path=?", -1, &touch, nullptr) == SQLITE_OK;
    }

    // Emptied where it stands, SQLite's own locks keeping others out, rather
    // than the file removed from under another window.
    bool reset()
    {
        return db && sqlite3_db_config(db, SQLITE_DBCONFIG_RESET_DATABASE, 1, 0) == SQLITE_OK && exec("VACUUM") &&
               sqlite3_db_config(db, SQLITE_DBCONFIG_RESET_DATABASE, 0, 0) == SQLITE_OK;
    }

    void open()
    {
        const std::string dir = platform::cache_dir();
        g_mkdir_with_parents(dir.c_str(), 0700);
        const std::string path = platform::join_path(dir, "folder-index.db");
        if (open_file(path))
            return;
        // Busy or locked is another window at work: this session goes without.
        // Only a database that's really damaged is started again.
        const int code = db ? sqlite3_errcode(db) & 0xFF : SQLITE_CANTOPEN;
        if ((code == SQLITE_CORRUPT || code == SQLITE_NOTADB) && reset())
        {
            close();
            if (open_file(path))
                return;
        }
        platform::trace("folder index: no database (%d)", code);
        close();
    }

    // What's known of a directory, and when it was last used.
    std::vector<FileRecord> load(const std::string& key)
    {
        std::vector<FileRecord> records;
        if (!get)
            return records;
        sqlite3_bind_text(get, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(get) == SQLITE_ROW)
        {
            const int64_t used = sqlite3_column_int64(get, 0);
            const void* block = sqlite3_column_blob(get, 1);
            const int size = sqlite3_column_bytes(get, 1);
            if (!block || !decode_folder_block(static_cast<const uint8_t*>(block), size_t(size), records))
                records.clear();
            const int64_t now = int64_t(std::time(nullptr));
            if (used < now - kDay)
            {
                sqlite3_reset(get);
                sqlite3_bind_int64(touch, 1, now);
                sqlite3_bind_text(touch, 2, key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(touch);
                sqlite3_reset(touch);
            }
        }
        sqlite3_reset(get);
        return records;
    }

    void save(const std::string& key, std::vector<FileRecord>& records)
    {
        if (!put)
            return;
        const std::vector<uint8_t> block = encode_folder_block(records);
        sqlite3_bind_text(put, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(put, 2, int64_t(std::time(nullptr)));
        sqlite3_bind_blob(put, 3, block.data(), int(block.size()), SQLITE_STATIC);
        sqlite3_step(put);
        sqlite3_reset(put);
        if (++writes % 64 == 1)
            trim();
    }

    // The folders not seen for longest go until what's left fits.
    void trim()
    {
        exec(("DELETE FROM dirs WHERE path IN (SELECT path FROM (SELECT path, SUM(LENGTH(block)) OVER (ORDER BY used DESC, path ROWS UNBOUNDED PRECEDING) AS kept FROM dirs) WHERE kept > " +
              std::to_string(kBudget) + ")")
                 .c_str());
    }
};

FolderIndex::FolderIndex()
{
    thread_ = std::thread([this] { run(); });
}

FolderIndex::~FolderIndex()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void FolderIndex::scan(std::vector<std::string> paths, Report report)
{
    {
        std::lock_guard lock(mutex_);
        job_.paths = std::move(paths);
        job_.report = std::move(report);
        job_.serial = ++serial_;
        have_job_ = true;
        updates_.clear();  // the scan looks at everything
    }
    wake_.notify_all();
}

void FolderIndex::cancel()
{
    std::lock_guard lock(mutex_);
    have_job_ = false;
    job_ = {};
    updates_.clear();
    ++serial_;
}

void FolderIndex::run()
{
    platform::lower_thread_priority();
    Store store;
    bool opened = false;
    while (true)
    {
        Job job;
        Update update;
        bool scanning = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || have_job_ || !updates_.empty(); });
            if (stop_)
                return;
            if (have_job_)
            {
                job = std::move(job_);
                have_job_ = false;
                scanning = true;
            }
            else
            {
                update = std::move(updates_.front());
                updates_.pop_front();
            }
        }
        if (!opened)
        {
            store.open();
            opened = true;
        }
        if (scanning)
            work(store, job);
        else
            work(store, update);
    }
}

void FolderIndex::update(std::vector<std::string> changed, std::vector<std::string> gone, Changes report)
{
    {
        std::lock_guard lock(mutex_);
        updates_.push_back({std::move(changed), std::move(gone), std::move(report), serial_});
    }
    wake_.notify_all();
}

void FolderIndex::work(Store& store, const Update& update)
{
    // A directory at a time, as they're stored: its block changed where the
    // files did, the rest kept as it was.
    std::map<fs::path, std::pair<std::vector<const std::string*>, std::vector<const std::string*>>> folders;
    for (const std::string& path : update.changed)
        folders[rap::sys::path_from_utf8(path).parent_path()].first.push_back(&path);
    for (const std::string& path : update.gone)
        folders[rap::sys::path_from_utf8(path).parent_path()].second.push_back(&path);

    std::vector<std::string> removed = update.gone;
    std::vector<Located> found;
    const auto ended = [&] { return stop_ || serial_ != update.serial; };
    for (const auto& [dir, files] : folders)
    {
        if (ended())
            return;
        const std::string key = directory_key(dir);
        std::vector<FileRecord> records = store.load(key);
        bool dirty = false;
        const auto named = [&](const std::string& path)
        { return rap::sys::path_to_utf8(rap::sys::path_from_utf8(path).filename()); };
        for (const std::string* path : files.second)
        {
            const std::string name = named(*path);
            const auto before = records.size();
            records.erase(std::remove_if(records.begin(), records.end(), [&](const FileRecord& r) { return r.name == name; }), records.end());
            dirty |= records.size() != before;
        }
        bool stopped = false;
        for (const std::string* path : files.first)
        {
            if (ended())
            {
                stopped = true;  // what's been read is kept, nothing reported
                break;
            }
            removed.push_back(*path);
            const fs::path p = rap::sys::path_from_utf8(*path);
            std::error_code e1, e2;
            const auto size = fs::file_size(p, e1);
            const auto when = fs::last_write_time(p, e2);
            if (e1 || e2)
                continue;  // gone again already
            FileRecord record;
            record.name = named(*path);
            record.size = int64_t(size);
            record.mtime = millis_of(when);
            const auto at = std::find_if(records.begin(), records.end(), [&](const FileRecord& r) { return r.name == record.name; });
            if (at != records.end() && at->size == record.size && at->mtime == record.mtime)
                record = *at;  // as it was: nothing to read
            else if (!read_file_record(*path, record))
                continue;  // not readable yet: the next change, or scan, has it
            else
            {
                if (at != records.end())
                    *at = record;
                else
                    records.push_back(record);
                dirty = true;
            }
            if (record.has_location)
                found.push_back({*path, record.latitude, record.longitude, record.orientation, record.mtime});
        }
        if (dirty)
            store.save(key, records);
        if (stopped)
            return;
    }
    if (!ended())
        update.report(std::move(removed), std::move(found));
}

void FolderIndex::work(Store& store, const Job& job)
{
    // A directory at a time, as they're stored.
    std::map<fs::path, std::vector<const std::string*>> folders;
    for (const std::string& path : job.paths)
        folders[rap::sys::path_from_utf8(path).parent_path()].push_back(&path);

    std::vector<Located> found;
    auto last = std::chrono::steady_clock::now();

    for (const auto& [dir, files] : folders)
    {
        if (stale(job))
            return;
        const std::string key = directory_key(dir);
        std::vector<FileRecord> old = store.load(key);
        std::unordered_map<std::string, size_t> known;
        for (size_t i = 0; i < old.size(); ++i)
            known.emplace(old[i].name, i);
        std::vector<char> carried(old.size(), 0);

        std::vector<FileRecord> now;
        now.reserve(files.size());
        bool dirty = false;
        bool stopped = false;
        for (const std::string* path : files)
        {
            if (stale(job))
            {
                stopped = true;
                break;
            }
            const fs::path p = rap::sys::path_from_utf8(*path);
            std::error_code e1, e2;
            const auto size = fs::file_size(p, e1);
            const auto when = fs::last_write_time(p, e2);
            if (e1 || e2)
                continue;  // gone since the folder was listed
            const std::string name = rap::sys::path_to_utf8(p.filename());
            FileRecord record;
            const auto it = known.find(name);
            if (it != known.end())
                carried[it->second] = 1;
            if (it != known.end() && old[it->second].size == int64_t(size) && old[it->second].mtime == millis_of(when))
                record = old[it->second];
            else
            {
                record.name = name;
                record.size = int64_t(size);
                record.mtime = millis_of(when);
                if (!read_file_record(*path, record))
                {
                    // Not readable just now: tried again next time, not remembered as having nothing.
                    if (it != known.end())
                        now.push_back(std::move(old[it->second]));
                    continue;
                }
                dirty = true;
            }
            if (record.has_location)
                found.push_back({*path, record.latitude, record.longitude, record.orientation, record.mtime});
            now.push_back(std::move(record));

            const auto t = std::chrono::steady_clock::now();
            if (!found.empty() && t - last > std::chrono::milliseconds(200))
            {
                last = t;
                job.report(std::move(found), false);
                found.clear();
            }
        }
        if (stopped)
        {
            // What's been read is kept; what wasn't reached stays as it was.
            for (size_t i = 0; i < old.size(); ++i)
                if (!carried[i])
                    now.push_back(std::move(old[i]));
        }
        else
            dirty = dirty || std::count(carried.begin(), carried.end(), char(1)) != std::ptrdiff_t(old.size());
        if (dirty)
            store.save(key, now);
        if (stopped)
            return;
    }
    if (!stale(job))
        job.report(std::move(found), true);
}
