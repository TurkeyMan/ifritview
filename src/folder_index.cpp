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
#include <future>
#include <map>
#include <utility>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{

constexpr size_t kHead = size_t(1) << 20;  // where a file's metadata is, nearly always
constexpr int kSchema = 3;  // 2: the blocks' times in milliseconds; 3: CR3s read
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

// A Canon CR3, by its file type box (its brand past a 64-bit size, if it has one).
bool is_cr3(const uint8_t* d, size_t n)
{
    if (n < 12 || std::memcmp(d + 4, "ftyp", 4) != 0)
        return false;
    const size_t brand = big32(d) == 1 ? 16 : 8;
    return n >= brand + 4 && std::memcmp(d + brand, "crx ", 4) == 0;
}

// What a CR3 keeps of its metadata, in boxes found by seeking from one to the
// next, the file itself (raw data and all, tens of MB) never read: its EXIF
// as moov/uuid(Canon)/CMT1, CMT2 and CMT4, and its small preview, THMB,
// beside them (or the larger PRVW, in a box of its own, if there's no THMB).
struct Cr3
{
    std::vector<uint8_t> ifd0, exif, gps, thumbnail;
};

struct Box
{
    char type[4]{};
    uint64_t body = 0, end = 0;  // past the header (and a uuid's id); the box's end
    uint8_t id[16]{};            // a uuid box's
};

class BoxFile
{
public:
    explicit BoxFile(const fs::path& path) : in_(path, std::ios::binary)
    {
        if (in_ && in_.seekg(0, std::ios::end))
        {
            const std::streamoff end = in_.tellg();
            size_ = end > 0 ? uint64_t(end) : 0;
        }
    }
    uint64_t size() const { return size_; }
    // Whether a read of what's in the file failed: not the file's fault, so
    // nothing found is to be believed.
    bool failed() const { return failed_; }

    // False for a place past the file's end, as a box can claim, or if the
    // read itself fails (failed() says which).
    bool read(uint64_t at, void* out, size_t n)
    {
        if (at > size_ || size_ - at < n)
            return false;
        in_.clear();
        if (in_.seekg(std::streamoff(at)))
            in_.read(static_cast<char*>(out), std::streamsize(n));
        if (in_ && size_t(in_.gcount()) == n)
            return true;
        failed_ = true;
        return false;
    }

    // The box at `at`, which must end by `end`: false if there isn't one that fits.
    bool box(uint64_t at, uint64_t end, Box& box)
    {
        uint8_t h[16];
        if (at >= end || end - at < 8 || !read(at, h, 8))
            return false;
        uint64_t size = big32(h), header = 8;
        if (size == 1)
        {
            if (end - at < 16 || !read(at + 8, h + 8, 8))
                return false;
            size = uint64_t(big32(h + 8)) << 32 | big32(h + 12);
            header = 16;
        }
        else if (size == 0)
            size = end - at;  // to the end of what holds it
        if (size < header || size > end - at)
            return false;
        std::memcpy(box.type, h + 4, 4);
        box.body = at + header;
        box.end = at + size;
        if (std::memcmp(box.type, "uuid", 4) == 0)
        {
            if (box.end - box.body < 16 || !read(box.body, box.id, 16))
                return false;
            box.body += 16;
        }
        return true;
    }

    // A box's contents, if they're no more than `limit` bytes.
    bool body(const Box& box, size_t limit, std::vector<uint8_t>& out)
    {
        const uint64_t n = box.end - box.body;
        if (n > limit)
            return false;
        out.resize(size_t(n));
        if (read(box.body, out.data(), out.size()))
            return true;
        out.clear();
        return false;
    }

private:
    std::ifstream in_;
    uint64_t size_ = 0;
    bool failed_ = false;
};

// Calls `each` for the boxes from `at` to `end`, until it returns false, one
// doesn't fit or the file can't be read. Only so many: a file of nothing but
// tiny boxes isn't walked for ever.
template <class Each>
void for_each_box(BoxFile& file, uint64_t at, uint64_t end, Each each)
{
    Box box;
    for (int i = 0; i < 1024 && !file.failed() && file.box(at, end, box); ++i, at = box.end)
        if (!each(box))
            return;
}

bool is(const Box& box, const char* type)
{
    return std::memcmp(box.type, type, 4) == 0;
}

// A THMB's or PRVW's JPEG: after a version, the picture's size and the
// JPEG's length (at 8 in a THMB, 12 in a PRVW), the JPEG from 16 on.
std::vector<uint8_t> preview_jpeg(BoxFile& file, const Box& box, size_t length_at, size_t limit)
{
    uint8_t h[16];
    if (box.end - box.body < 16 || !file.read(box.body, h, 16))
        return {};
    const uint32_t length = big32(h + length_at);
    if (length < 4 || length > limit || length > box.end - box.body - 16)
        return {};
    std::vector<uint8_t> jpeg(length);
    if (!file.read(box.body + 16, jpeg.data(), jpeg.size()) || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
        return {};
    return jpeg;
}

// False if the file couldn't be read, to be tried again; one without these
// boxes, or with boxes that don't fit, is read fine and leaves `out` empty.
// Each box is looked for only where it belongs, so nothing's walked deeper
// than that.
bool read_cr3(const fs::path& path, bool want_thumbnail, Cr3& out)
{
    static const uint8_t canon[16] = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0, 0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};
    static const uint8_t preview[16] = {0xea, 0xf4, 0x2b, 0x5e, 0x1c, 0x98, 0x4b, 0x88, 0xb9, 0xfb, 0xb7, 0xdc, 0x40, 0x6e, 0x4d, 0x16};
    constexpr size_t kTiffLimit = size_t(1) << 20, kThumbLimit = size_t(1) << 20, kPreviewLimit = size_t(8) << 20;
    BoxFile file(path);
    if (!file.size())
        return false;
    bool seen_canon = false;
    // The Canon box's own boxes: the EXIF blocks and the THMB.
    const auto canon_box = [&](const Box& c)
    {
        if (is(c, "CMT1"))
            file.body(c, kTiffLimit, out.ifd0);
        else if (is(c, "CMT2"))
            file.body(c, kTiffLimit, out.exif);
        else if (is(c, "CMT4"))
            file.body(c, kTiffLimit, out.gps);
        else if (is(c, "THMB") && want_thumbnail)
            if (std::vector<uint8_t> jpeg = preview_jpeg(file, c, 8, kThumbLimit); !jpeg.empty())
                out.thumbnail = std::move(jpeg);  // over a PRVW found first, only if it's usable
        return true;
    };
    for_each_box(file, 0, file.size(),
                 [&](const Box& top)
                 {
                     if (is(top, "moov") && !seen_canon)
                         for_each_box(file, top.body, top.end,
                                      [&](const Box& b)
                                      {
                                          if (!is(b, "uuid") || std::memcmp(b.id, canon, 16) != 0)
                                              return true;
                                          seen_canon = true;
                                          for_each_box(file, b.body, b.end, canon_box);
                                          return false;
                                      });
                     else if (is(top, "uuid") && std::memcmp(top.id, preview, 16) == 0)
                     {
                         // Eight bytes of its own, then the PRVW box.
                         if (want_thumbnail && out.thumbnail.empty() && top.end - top.body > 8)
                             for_each_box(file, top.body + 8, top.end,
                                          [&](const Box& b)
                                          {
                                              if (!is(b, "PRVW"))
                                                  return true;
                                              out.thumbnail = preview_jpeg(file, b, 12, kPreviewLimit);
                                              return false;
                                          });
                     }
                     else if (is(top, "mdat"))
                         return false;  // the raw data: the boxes wanted are all before it
                     return !file.failed();
                 });
    return !file.failed();
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

    if (is_cr3(d, n))
    {
        Cr3 cr3;
        if (!read_cr3(p, false, cr3))
            return false;
        Image parsed;
        const ExifSummary summary = read_cr3_exif(cr3.ifd0, cr3.exif, cr3.gps, parsed);
        record.orientation = uint8_t(summary.orientation);
        record.camera = summary.camera;
        record.taken = summary.taken;
        record.has_location = parsed.has_location;
        record.latitude = parsed.latitude;
        record.longitude = parsed.longitude;
        return true;
    }

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
    const fs::path p = rap::sys::path_from_utf8(path);
    const std::string bytes = read_bytes(p, kHead);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t n = bytes.size();
    if (is_cr3(d, n))
    {
        Cr3 cr3;
        read_cr3(p, true, cr3);
        return std::move(cr3.thumbnail);
    }
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
    int failure = SQLITE_OK;  // what went wrong bringing an older database up to date

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

    // The database's version, in `version`: SQLite's code if it can't be read.
    int user_version(int& version)
    {
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr);
        if (rc == SQLITE_OK)
        {
            rc = sqlite3_step(st);
            if (rc == SQLITE_ROW)
            {
                version = sqlite3_column_int(st, 0);
                rc = SQLITE_OK;
            }
            else if (rc == SQLITE_DONE)
                rc = SQLITE_ERROR;  // no answer is no version either
        }
        sqlite3_finalize(st);
        return rc;
    }

    // Version 2 knew nothing of a CR3's metadata, so its CR3s are dropped
    // from their blocks, to be read again, everything else kept, and the
    // version moved on with them. All or nothing: SQLite's code for what
    // failed, the database left as it was.
    int forget_cr3s()
    {
        int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK)
            return rc;
        // Read again, now that no other window can be at it.
        int version = 0;
        rc = user_version(version);
        if (rc != SQLITE_OK || version != 2)
        {
            exec("ROLLBACK");
            if (rc != SQLITE_OK)
                return rc;
            // Brought up to date by another window just now; any other
            // version, from who knows what, is left alone for now.
            return version == kSchema ? SQLITE_OK : SQLITE_BUSY;
        }
        std::vector<std::pair<std::string, std::vector<uint8_t>>> changed;
        sqlite3_stmt* all = nullptr;
        rc = sqlite3_prepare_v2(db, "SELECT path, block FROM dirs", -1, &all, nullptr);
        while (rc == SQLITE_OK)
        {
            const int step = sqlite3_step(all);
            if (step != SQLITE_ROW)
            {
                rc = step == SQLITE_DONE ? SQLITE_OK : step;
                break;
            }
            const auto* block = static_cast<const uint8_t*>(sqlite3_column_blob(all, 1));
            std::vector<FileRecord> records;
            if (!block || !decode_folder_block(block, size_t(sqlite3_column_bytes(all, 1)), records))
                continue;  // unreadable either way; load() makes nothing of it
            const auto cr3 = [](const FileRecord& r)
            { return r.name.size() > 4 && g_ascii_strcasecmp(r.name.c_str() + r.name.size() - 4, ".cr3") == 0; };
            if (std::none_of(records.begin(), records.end(), cr3))
                continue;
            records.erase(std::remove_if(records.begin(), records.end(), cr3), records.end());
            if (const unsigned char* key = sqlite3_column_text(all, 0))
                changed.emplace_back(reinterpret_cast<const char*>(key), encode_folder_block(records));
        }
        sqlite3_finalize(all);
        sqlite3_stmt* update = nullptr;
        if (rc == SQLITE_OK)
            rc = sqlite3_prepare_v2(db, "UPDATE dirs SET block=? WHERE path=?", -1, &update, nullptr);
        for (size_t i = 0; rc == SQLITE_OK && i < changed.size(); ++i)
        {
            sqlite3_bind_blob(update, 1, changed[i].second.data(), int(changed[i].second.size()), SQLITE_STATIC);
            sqlite3_bind_text(update, 2, changed[i].first.c_str(), -1, SQLITE_STATIC);
            const int step = sqlite3_step(update);
            rc = step == SQLITE_DONE ? SQLITE_OK : step;
            sqlite3_reset(update);
        }
        sqlite3_finalize(update);
        char set[48];
        std::snprintf(set, sizeof set, "PRAGMA user_version=%d", kSchema);
        if (rc == SQLITE_OK)
            rc = sqlite3_exec(db, set, nullptr, nullptr, nullptr);
        if (rc == SQLITE_OK)
            rc = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK)
            exec("ROLLBACK");
        return rc;
    }

    bool open_file(const std::string& path)
    {
        failure = SQLITE_OK;
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK)
            return false;
        sqlite3_busy_timeout(db, 1000);
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=NORMAL");
        int version = 0;
        if (user_version(version) != SQLITE_OK)
            return false;
        if (version == 2)
        {
            // Failing, the database stays as it is, to be tried again, not reset.
            failure = forget_cr3s();
            if (failure != SQLITE_OK)
                return false;
        }
        else if (version != kSchema && !exec("DROP TABLE IF EXISTS dirs"))
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

    // The journal written into the database and emptied; not while another
    // connection still reads what was there before.
    bool flush()
    {
        int log = 0, copied = 0;
        return db && sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &log, &copied) == SQLITE_OK && log <= 0;
    }

    void open(const std::string& path)
    {
        g_mkdir_with_parents(rap::sys::path_to_utf8(rap::sys::path_from_utf8(path).parent_path()).c_str(), 0700);
        if (open_file(path))
            return;
        // Busy or locked is another window at work: this session goes without.
        // Only a database that's really damaged is started again.
        const int code = !db ? SQLITE_CANTOPEN : (failure != SQLITE_OK ? failure : sqlite3_errcode(db)) & 0xFF;
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

std::string FolderIndex::database_path()
{
    return platform::join_path(platform::cache_dir(), "folder-index.db");
}

FolderIndex::FolderIndex(std::string database) : database_(std::move(database))
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

void FolderIndex::forget(std::function<void(bool)> done)
{
    {
        std::lock_guard lock(mutex_);
        have_job_ = false;
        job_ = {};
        updates_.clear();
        ++serial_;
        forget_ = true;
        if (done)
            forgotten_.push_back(std::move(done));
    }
    wake_.notify_all();
}

void FolderIndex::run()
{
    platform::lower_thread_priority();
    Store store;
    bool opened = false;
    // Forgetting: every folder's records deleted, the space they took given
    // back, so nothing of where the photos were is left in the file, and the
    // journal written in and emptied, so nothing's left there either. Another
    // window writing is waited for, as any write is; one still reading what
    // was there, or busy with the database, stops it part way. Then it's
    // taken up where it stopped (each step done stays done, and records
    // stored after the delete are kept) whenever there's nothing else to do,
    // and on closing; and, this mark left beside the database till it has
    // finished, by whichever window opens the database next.
    enum class Step
    {
        none,  // nothing to forget
        erase,
        compact,
        flush,
    };
    const fs::path mark = rap::sys::path_from_utf8(database_ + ".forget");
    Step pending = Step::none;
    const auto forget = [&](bool asked, const std::vector<std::function<void(bool)>>& done)
    {
        if (asked)
            pending = Step::erase;  // from the start: what's there now goes, whatever's gone before
        std::error_code e;
        if (!fs::exists(mark, e))
            std::ofstream(mark, std::ios::binary).put('\n');
        // A database that couldn't be opened before (another window busy
        // bringing it up to date, say) is tried again.
        if (!store.db)
            store.open(database_);
        if (pending == Step::erase && store.db && store.exec("DELETE FROM dirs"))
            pending = Step::compact;
        if (pending == Step::compact && store.db && store.exec("VACUUM"))
            pending = Step::flush;
        if (pending == Step::flush && store.flush())
            pending = Step::none;
        const bool emptied = pending == Step::none;
        if (emptied)
            fs::remove(mark, e);
        platform::trace("folder index: %s", emptied ? "forgotten" : "not forgotten yet: another connection has it");
        for (const auto& d : done)
            d(emptied);
    };
    const auto open = [&]
    {
        if (opened)
            return;
        store.open(database_);
        opened = true;
        std::error_code e;
        if (fs::exists(mark, e))
            forget(true, {});
    };
    while (true)
    {
        Job job;
        Update update;
        bool scanning = false, updating = false, asked = false, stopping = false;
        std::vector<std::function<void(bool)>> forgotten;
        {
            std::unique_lock lock(mutex_);
            const auto ready = [this] { return stop_ || have_job_ || !updates_.empty() || forget_; };
            if (pending != Step::none)
                wake_.wait_for(lock, std::chrono::seconds(5), ready);
            else
                wake_.wait(lock, ready);
            stopping = stop_;
            asked = std::exchange(forget_, false);
            forgotten = std::move(forgotten_);
            forgotten_.clear();
            // Closing, what's asked for goes with it; a forget doesn't.
            if (!stopping && have_job_)
            {
                job = std::move(job_);
                have_job_ = false;
                scanning = true;
            }
            else if (!stopping && !updates_.empty())
            {
                update = std::move(updates_.front());
                updates_.pop_front();
                updating = true;
            }
        }
        const bool unfinished = pending != Step::none;
        if (stopping && !asked && !unfinished)
            return;
        open();
        if (asked || (pending != Step::none && !scanning && !updating))
            forget(asked, forgotten);
        if (stopping)
            return;
        if (scanning)
            work(store, job);
        else if (updating)
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

int check_index_forget(const std::string& dir)
{
    using namespace std::chrono_literals;
    g_mkdir_with_parents(dir.c_str(), 0700);
    const std::string path = platform::join_path(dir, "check-index.db");
    for (const char* suffix : {"", "-wal", "-shm"})
        g_remove((path + suffix).c_str());
    const std::string photo = platform::join_path(dir, "check-photo.jpg");
    std::ofstream(rap::sys::path_from_utf8(photo), std::ios::binary) << "not much of a photo";

    FolderIndex index(path);
    const auto forget = [&]
    {
        std::promise<bool> result;
        index.forget([&](bool emptied) { result.set_value(emptied); });
        return result.get_future().get();
    };
    sqlite3* writer = nullptr;
    sqlite3* reader = nullptr;
    sqlite3_stmt* read = nullptr;
    const auto rows = [&]
    {
        sqlite3_stmt* count = nullptr;
        int n = -1;
        if (sqlite3_prepare_v2(writer, "SELECT COUNT(*) FROM dirs", -1, &count, nullptr) == SQLITE_OK && sqlite3_step(count) == SQLITE_ROW)
            n = sqlite3_column_int(count, 0);
        sqlite3_finalize(count);
        return n;
    };
    const auto journal = [&]
    {
        std::error_code e;
        const auto size = fs::file_size(rap::sys::path_from_utf8(path + "-wal"), e);
        return e ? 0 : size;
    };
    // Another window part way through reading what's there.
    const auto hold = [&]
    {
        return sqlite3_exec(reader, "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK &&
               sqlite3_prepare_v2(reader, "SELECT path FROM dirs", -1, &read, nullptr) == SQLITE_OK && sqlite3_step(read) == SQLITE_ROW;
    };
    const auto let_go = [&]
    {
        sqlite3_finalize(read);
        read = nullptr;
        sqlite3_exec(reader, "COMMIT", nullptr, nullptr, nullptr);
    };
    const auto add = [&] { return sqlite3_exec(writer, "INSERT OR REPLACE INTO dirs(path, used, block) VALUES('/photos', 0, x'00')", nullptr, nullptr, nullptr) == SQLITE_OK; };
    const auto fail = [&](const char* what)
    {
        std::fprintf(stderr, "check-index-forget: %s\n", what);
        let_go();
        sqlite3_close(reader);
        sqlite3_close(writer);
        return 1;
    };

    if (!forget())  // made, empty
        return fail("an empty index wasn't forgotten");
    if (sqlite3_open(path.c_str(), &writer) != SQLITE_OK || sqlite3_open(path.c_str(), &reader) != SQLITE_OK || !add())
        return fail("couldn't add a folder");

    // Refused while the old records are being read, and done, without being
    // asked again, once they aren't.
    if (!hold())
        return fail("couldn't read the folder back");
    if (forget())
        return fail("forgotten while another connection still reads the old records");
    let_go();
    for (int i = 0; i < 100 && (rows() != 0 || journal() != 0); ++i)
        std::this_thread::sleep_for(100ms);
    if (rows() != 0 || journal() != 0)
        return fail("not forgotten once the reader had finished");

    // An update queued before a forget doesn't write its file back after it.
    // The index is kept busy (a forget waiting on a reader) while they're queued.
    if (!add() || !hold())
        return fail("couldn't add a folder again");
    std::atomic<bool> reported{false};
    std::promise<bool> first;
    index.forget([&](bool) { first.set_value(true); });
    std::this_thread::sleep_for(200ms);
    index.update({photo}, {}, [&](std::vector<std::string>, std::vector<FolderIndex::Located>) { reported = true; });
    std::promise<bool> second;
    index.forget([&](bool emptied) { second.set_value(emptied); });
    let_go();
    first.get_future().get();
    if (!second.get_future().get())
        return fail("not forgotten after the update");
    std::this_thread::sleep_for(500ms);  // time for an update that wasn't dropped
    if (reported || rows() != 0)
        return fail("an update queued before forgetting wrote its records back");

    // A forget asked for as the index closes is done before it goes.
    if (!add())
        return fail("couldn't add a folder for closing");
    {
        FolderIndex closing(path);
        closing.forget();
    }
    if (rows() != 0)
        return fail("a forget asked for as the index closed was dropped");

    // One that can't finish as it closes is left marked, and finished by the
    // next index to open the database.
    const fs::path mark = rap::sys::path_from_utf8(path + ".forget");
    if (!add() || !hold())
        return fail("couldn't add a folder to be left marked");
    {
        FolderIndex closing(path);
        closing.forget();
    }
    std::error_code e;
    if (!fs::exists(mark, e))
        return fail("an unfinished forget wasn't marked");
    let_go();
    {
        FolderIndex next(path);
        std::promise<void> scanned;
        next.scan({}, [&](std::vector<FolderIndex::Located>, bool done) { if (done) scanned.set_value(); });
        scanned.get_future().get();
    }
    if (rows() != 0 || journal() != 0 || fs::exists(mark, e))
        return fail("a marked forget wasn't finished by the next to open the database");

    // A mark on a database from before (version 2, due to be brought up to
    // date) that another window has locked as this one opens it: finished
    // once it lets go, the update made too.
    sqlite3_close(writer);
    sqlite3_close(reader);
    writer = reader = nullptr;
    const std::string old = platform::join_path(dir, "check-index-v2.db");
    const fs::path old_mark = rap::sys::path_from_utf8(old + ".forget");
    for (const char* suffix : {"", "-wal", "-shm", ".forget"})
        g_remove((old + suffix).c_str());
    const auto version = [&]
    {
        sqlite3_stmt* st = nullptr;
        int v = -1;
        if (sqlite3_prepare_v2(writer, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        return v;
    };
    if (sqlite3_open(old.c_str(), &writer) != SQLITE_OK ||
        sqlite3_exec(writer,
                     "PRAGMA journal_mode=WAL; PRAGMA user_version=2;"
                     "CREATE TABLE dirs(path TEXT PRIMARY KEY, used INTEGER NOT NULL, block BLOB NOT NULL) WITHOUT ROWID;"
                     "INSERT INTO dirs(path, used, block) VALUES('/photos', 0, x'00');",
                     nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail("couldn't make a version 2 database");
    std::ofstream(old_mark, std::ios::binary).put('\n');
    if (sqlite3_exec(writer, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail("couldn't lock the version 2 database");
    {
        FolderIndex next(old);
        std::promise<bool> result;
        next.forget([&](bool emptied) { result.set_value(emptied); });
        if (result.get_future().get())
            return fail("forgotten while another window had the database locked");
        sqlite3_exec(writer, "COMMIT", nullptr, nullptr, nullptr);
        for (int i = 0; i < 100 && (rows() != 0 || fs::exists(old_mark, e)); ++i)
            std::this_thread::sleep_for(100ms);
    }
    if (rows() != 0 || fs::exists(old_mark, e))
        return fail("a marked forget wasn't finished once the database could be opened");
    if (version() < 3)
        return fail("the database wasn't brought up to date");

    // The same, with a scan asked for while it's locked: one that stores
    // nothing (it has no database) doesn't excuse the delete when it's next tried.
    sqlite3_close(writer);
    writer = nullptr;
    const std::string scanned = platform::join_path(dir, "check-index-v2-scan.db");
    const fs::path scanned_mark = rap::sys::path_from_utf8(scanned + ".forget");
    for (const char* suffix : {"", "-wal", "-shm", ".forget"})
        g_remove((scanned + suffix).c_str());
    if (sqlite3_open(scanned.c_str(), &writer) != SQLITE_OK ||
        sqlite3_exec(writer,
                     "PRAGMA journal_mode=WAL; PRAGMA user_version=2;"
                     "CREATE TABLE dirs(path TEXT PRIMARY KEY, used INTEGER NOT NULL, block BLOB NOT NULL) WITHOUT ROWID;"
                     "INSERT INTO dirs(path, used, block) VALUES('/photos', 0, x'00');",
                     nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail("couldn't make another version 2 database");
    std::ofstream(scanned_mark, std::ios::binary).put('\n');
    if (sqlite3_exec(writer, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail("couldn't lock the other version 2 database");
    {
        FolderIndex next(scanned);
        std::promise<void> done;
        next.scan({}, [&](std::vector<FolderIndex::Located>, bool all) { if (all) done.set_value(); });
        done.get_future().get();
        sqlite3_exec(writer, "COMMIT", nullptr, nullptr, nullptr);
        for (int i = 0; i < 100 && (rows() != 0 || fs::exists(scanned_mark, e)); ++i)
            std::this_thread::sleep_for(100ms);
    }
    if (rows() != 0)
        return fail("a scan while the database was locked let a marked forget skip its delete");
    if (fs::exists(scanned_mark, e))
        return fail("a marked forget wasn't finished after a scan while the database was locked");

    std::printf("check-index-forget: ok\n");
    sqlite3_close(reader);
    sqlite3_close(writer);
    return 0;
}
