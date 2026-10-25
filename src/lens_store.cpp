#include "lens_store.h"

#include "decoders/decoder.h"
#include "lens_database.h"
#include "platform.h"

#include <glib.h>
#include <glib/gstdio.h>

#ifdef HAVE_LENS_DOWNLOADS
#include "http.h"

#include <bzlib.h>
#include <json-glib/json-glib.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace lenses
{
namespace
{

constexpr int kVersion = 2;  // the database's format: lensfun 0.3.95 on's, the newest

enum State
{
    Idle,
    Reading,
    Fetching,
    Ready,
    Failed,
};

// All on the main thread but `current`, which the workers' results reach
// through post_main too; the mutex is for database()'s callers elsewhere.
std::mutex mutex;
std::shared_ptr<const Database> current;
State stage = Idle;
bool downloads = true;
bool started = false;
std::function<void()> listener;
int64_t local_timestamp = 0;  // the cached copy's (0: none)

std::string root()
{
    return platform::join_path(platform::cache_dir(), "lensfun");
}

std::string folder()
{
    return platform::join_path(root(), "version_" + std::to_string(kVersion));
}

int64_t read_number(const std::string& file)
{
    gchar* text = nullptr;
    if (!g_file_get_contents(file.c_str(), &text, nullptr, nullptr))
        return 0;
    const int64_t v = g_ascii_strtoll(text, nullptr, 10);
    g_free(text);
    return v;
}

// Every XML file in `dir` (in name order) into `db`.
bool read_folder(const std::string& dir, Database& db, std::string& error)
{
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d)
    {
        error = "no database";
        return false;
    }
    std::vector<std::string> names;
    while (const char* n = g_dir_read_name(d))
        if (g_str_has_suffix(n, ".xml"))
            names.push_back(n);
    g_dir_close(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names)
    {
        gchar* text = nullptr;
        gsize len = 0;
        std::string why;
        if (!g_file_get_contents(platform::join_path(dir, n).c_str(), &text, &len, nullptr) || !db.add_xml(text, len, why))
            error = n + ": " + (why.empty() ? "unreadable" : why);
        g_free(text);
        if (!error.empty())
            return false;
    }
    if (db.empty())
        error = "no lenses";
    return !db.empty();
}

// On the main loop: a database in, for the raw shown.
void publish(std::shared_ptr<const Database> db)
{
    {
        std::lock_guard lock(mutex);
        current = std::move(db);
    }
    stage = Ready;
    if (listener)
        listener();
}

#ifdef HAVE_LENS_DOWNLOADS
// lensfun-update-data's sites: versions.json says the database's date and
// the versions there are, each one's version_<n>.tar.bz2 beside it.
const char* const kSites[] = {"https://lensfun.github.io/db/", "https://wilson.bronger.org/lensfun-db/"};
constexpr int64_t kRecheck = 30 * 24 * 3600;  // seconds between looks for a newer one
constexpr size_t kMostUnpacked = 64u << 20;   // its 5 MB today (the archive, 0.4 MB)

std::shared_ptr<void> request;  // the download under way; dropping it cancels

bool write_number(const std::string& file, int64_t v)
{
    const std::string s = std::to_string(v) + "\n";
    return g_file_set_contents(file.c_str(), s.c_str(), gssize(s.size()), nullptr);
}

// versions.json: [timestamp, [versions], [other sites]].
bool parse_versions(const std::string& body, int64_t& timestamp, bool& listed)
{
    JsonParser* parser = json_parser_new();
    bool ok = false;
    if (json_parser_load_from_data(parser, body.data(), gssize(body.size()), nullptr))
    {
        JsonNode* root = json_parser_get_root(parser);
        JsonArray* a = root && JSON_NODE_HOLDS_ARRAY(root) ? json_node_get_array(root) : nullptr;
        if (a && json_array_get_length(a) >= 2 && JSON_NODE_HOLDS_VALUE(json_array_get_element(a, 0)) &&
            JSON_NODE_HOLDS_ARRAY(json_array_get_element(a, 1)))
        {
            timestamp = json_array_get_int_element(a, 0);
            JsonArray* versions = json_array_get_array_element(a, 1);
            listed = false;
            for (guint i = 0; i < json_array_get_length(versions); ++i)
                listed |= json_array_get_int_element(versions, i) == kVersion;
            ok = timestamp > 0;
        }
    }
    g_object_unref(parser);
    return ok;
}

// The archive bzip2 compressed, at most kMostUnpacked of it.
bool bunzip2(const std::string& archive, std::string& out, std::string& error)
{
    bz_stream s{};
    if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK)
    {
        error = "bzip2 failed";
        return false;
    }
    s.next_in = const_cast<char*>(archive.data());
    s.avail_in = unsigned(archive.size());
    out.resize(std::min(archive.size() * 8, kMostUnpacked));
    int r = BZ_OK;
    while (r == BZ_OK)
    {
        if (s.total_out_lo32 == out.size())
        {
            if (out.size() >= kMostUnpacked)
                break;
            out.resize(std::min(out.size() * 2, kMostUnpacked));
        }
        s.next_out = out.data() + s.total_out_lo32;
        s.avail_out = unsigned(out.size() - s.total_out_lo32);
        r = BZ2_bzDecompress(&s);
    }
    out.resize(s.total_out_lo32);
    BZ2_bzDecompressEnd(&s);
    if (r != BZ_STREAM_END)
        error = r == BZ_OK ? "the archive unpacks to too much" : "the archive is damaged (bzip2 " + std::to_string(r) + ")";
    return r == BZ_STREAM_END;
}

// A tar archive's regular files whose names (their last part) are plain:
// letters, digits, '.', '-', '_'. Anything else is passed over.
bool untar(const std::string& tar, std::vector<std::pair<std::string, std::string>>& files, std::string& error)
{
    size_t at = 0;
    while (at + 512 <= tar.size())
    {
        const char* h = tar.data() + at;
        if (std::all_of(h, h + 512, [](char c) { return c == 0; }))
            return true;  // the end
        std::string name(h, strnlen(h, 100));
        const char* size_field = h + 124;
        uint64_t size = 0;
        for (int i = 0; i < 12 && size_field[i]; ++i)
            if (size_field[i] >= '0' && size_field[i] <= '7')
                size = size * 8 + uint64_t(size_field[i] - '0');
            else if (size_field[i] != ' ')
            {
                error = "bad tar header";
                return false;
            }
        at += 512;
        if (size > tar.size() - at)
        {
            error = "truncated tar";
            return false;
        }
        const char type = h[156];
        if (const size_t slash = name.rfind('/'); slash != std::string::npos)
            name.erase(0, slash + 1);
        const bool plain = !name.empty() && name[0] != '.' &&
                           std::all_of(name.begin(), name.end(), [](char c) { return g_ascii_isalnum(c) || c == '.' || c == '-' || c == '_'; });
        if ((type == '0' || type == 0) && plain)
            files.emplace_back(name, tar.substr(at, size_t(size)));
        at += size_t((size + 511) / 512 * 512);
    }
    return true;
}

void remove_folder(const std::string& dir)
{
    if (GDir* d = g_dir_open(dir.c_str(), 0, nullptr))
    {
        while (const char* n = g_dir_read_name(d))
            g_remove(platform::join_path(dir, n).c_str());
        g_dir_close(d);
    }
    g_rmdir(dir.c_str());
}

void checked()
{
    write_number(platform::join_path(root(), "checked"), int64_t(std::time(nullptr)));
}

// The archive (lensfun-update-data's .tar.bz2) checked (every file must read
// as the database) and put in the cache in place of what's there. A worker's.
bool unpack(const std::string& archive, int64_t timestamp, std::shared_ptr<Database>& out, std::string& error)
{
    std::string tar;
    std::vector<std::pair<std::string, std::string>> files;
    if (!bunzip2(archive, tar, error) || !untar(tar, files, error))
        return false;
    auto db = std::make_shared<Database>();
    for (const auto& [name, data] : files)
        if (g_str_has_suffix(name.c_str(), ".xml") && !db->add_xml(data.data(), data.size(), error))
        {
            error = name + ": " + error;
            return false;
        }
    if (db->lens_count() < 100)
    {
        error = "only " + std::to_string(db->lens_count()) + " lenses";
        return false;
    }
    // Written beside the old copy, then put in its place.
    const std::string dir = folder(), fresh = dir + ".new";
    remove_folder(fresh);
    if (g_mkdir_with_parents(fresh.c_str(), 0755) != 0)
    {
        error = "can't make " + fresh;
        return false;
    }
    for (const auto& [name, data] : files)
        if (g_str_has_suffix(name.c_str(), ".xml") &&
            !g_file_set_contents(platform::join_path(fresh, name).c_str(), data.data(), gssize(data.size()), nullptr))
        {
            error = "can't write " + fresh;
            return false;
        }
    write_number(platform::join_path(fresh, "timestamp.txt"), timestamp);
    remove_folder(dir);
    if (g_rename(fresh.c_str(), dir.c_str()) != 0)
    {
        error = "can't replace " + dir;
        return false;
    }
    out = std::move(db);
    return true;
}

const HttpHeaders& headers()
{
    static const HttpHeaders h = {{"User-Agent", "IfritView/" IFRIT_VERSION}};
    return h;
}

void fetch_from(size_t site);

void failed(size_t site, const std::string& why)
{
    platform::trace("lenses: %s: %s", kSites[site], why.c_str());
    if (site + 1 < std::size(kSites))
        fetch_from(site + 1);
    else
    {
        request.reset();
        stage = current ? Ready : Failed;
    }
}

// The archive itself, unpacked off the main thread.
void download(size_t site, int64_t timestamp)
{
    const std::string url = std::string(kSites[site]) + "version_" + std::to_string(kVersion) + ".tar.bz2";
    const int64_t t0 = g_get_monotonic_time();
    request = http_get(url, headers(),
                       [site, timestamp, t0](const HttpResponse& r)
                       {
                           if (r.status != 200)
                               return failed(site, r.status ? "HTTP " + std::to_string(r.status) : r.error);
                           const int64_t t1 = g_get_monotonic_time();
                           platform::trace("lenses: lensfun's database fetched in %.0f ms, %zu KB", (t1 - t0) / 1000.0, r.body.size() >> 10);
                           std::thread(
                               [site, timestamp, t1, archive = r.body]
                               {
                                   platform::lower_thread_priority();
                                   std::shared_ptr<Database> db;
                                   std::string error;
                                   const bool ok = unpack(archive, timestamp, db, error);
                                   platform::post_main(
                                       [site, ok, db, error, t1]
                                       {
                                           if (!ok)
                                               return failed(site, error);
                                           platform::trace("lenses: lensfun's database (%zu lenses, %zu cameras) unpacked, read and cached in %.0f ms",
                                                           db->lens_count(), db->camera_count(), (g_get_monotonic_time() - t1) / 1000.0);
                                           request.reset();
                                           checked();
                                           publish(db);
                                       });
                               })
                               .detach();
                       });
}

// What the site has: a newer database than ours, or not.
void fetch_from(size_t site)
{
    stage = Fetching;
    g_mkdir_with_parents(root().c_str(), 0755);
    request = http_get(std::string(kSites[site]) + "versions.json", headers(),
                       [site](const HttpResponse& r)
                       {
                           int64_t timestamp = 0;
                           bool listed = false;
                           if (r.status != 200)
                               return failed(site, "versions.json: " + (r.status ? "HTTP " + std::to_string(r.status) : r.error));
                           if (!parse_versions(r.body, timestamp, listed) || !listed)
                               return failed(site, "versions.json: not as expected");
                           if (current && timestamp <= local_timestamp)
                           {
                               platform::trace("lenses: lensfun's database is up to date (%lld)", (long long)local_timestamp);
                               request.reset();
                               checked();
                               stage = Ready;
                               return;
                           }
                           download(site, timestamp);
                       });
}

// Whether it's time to look for a newer one (or there's none).
bool due(bool have)
{
    return !have || int64_t(std::time(nullptr)) - read_number(platform::join_path(root(), "checked")) >= kRecheck;
}
#else
void fetch_from(size_t)
{
    stage = current ? Ready : Failed;
}

bool due(bool)
{
    return false;
}
#endif

// The cached copy read (a worker's), then fetched if it's missing or due.
void read_cache()
{
    stage = Reading;
    std::thread(
        []
        {
            platform::lower_thread_priority();
            const char* local = g_getenv("IFRIT_LENSFUN_DB");
            const std::string dir = local ? local : folder();
            const int64_t t0 = g_get_monotonic_time();
            auto db = std::make_shared<Database>();
            std::string error;
            const bool have = read_folder(dir, *db, error);
            const int64_t timestamp = read_number(platform::join_path(dir, "timestamp.txt"));
            const bool fetch = !local && due(have);
            if (have)
                platform::trace("lenses: lensfun's database read in %.0f ms (%zu lenses, %zu cameras)", (g_get_monotonic_time() - t0) / 1000.0,
                                db->lens_count(), db->camera_count());
            else
                platform::trace("lenses: no database read from %s (%s)", dir.c_str(), error.c_str());
            platform::post_main(
                [have, db, timestamp, fetch]
                {
                    local_timestamp = timestamp;
                    if (have)
                        publish(db);
                    if (fetch && downloads)
                        fetch_from(0);
                    else
                        stage = have ? Ready : Failed;
                });
        })
        .detach();
}

}  // namespace

void allow_downloads(bool allow)
{
    downloads = allow;
#ifdef HAVE_LENS_DOWNLOADS
    if (!allow && request)
    {
        request.reset();
        stage = current ? Ready : Failed;
    }
#endif
    // Turned on with nothing to show for the last try: try again.
    if (allow && started && stage == Failed)
        fetch_from(0);
}

std::shared_ptr<const Database> database()
{
    std::lock_guard lock(mutex);
    return current;
}

void want(std::function<void()> arrived)
{
    listener = std::move(arrived);
    if (started)
        return;
    started = true;
    read_cache();
}

int report(int count, char** files)
{
    const char* local = g_getenv("IFRIT_LENSFUN_DB");
    Database db;
    std::string error;
    if (!read_folder(local ? local : folder(), db, error))
    {
        std::fprintf(stderr, "No lens database in %s (%s): the viewer fetches it when it first opens a raw.\n", local ? local : folder().c_str(),
                     error.c_str());
        return 1;
    }
    std::printf("lensfun's database: %zu lenses, %zu cameras\n", db.lens_count(), db.camera_count());
    struct Row
    {
        int files = 0;
        std::string match;
        std::map<std::string, int> corrected;  // what was, how often
    };
    std::map<std::string, Row> rows;
    int unread = 0;
    for (int i = 0; i < count; ++i)
    {
        Shot s;
        if (!raw_shot(files[i], s))
        {
            ++unread;
            continue;
        }
        Row& row = rows[s.make + " " + s.model + " | " + (s.lens.empty() ? "(no lens name)" : s.lens)];
        ++row.files;
        const Camera* camera = db.camera(s);
        std::string why;
        const Lens* lens = camera ? db.lens(*camera, s, why) : nullptr;
        if (!camera)
            row.match = "no: the camera isn't in the database";
        else if (!lens)
            row.match = "no: " + why;
        else
        {
            row.match = "lensfun: " + lens->models[0] + " (crop " + std::to_string(lens->crop).substr(0, 4) + ")";
            const dng::Corrections c = corrections(*lens, *camera, s);
            ++row.corrected[c.summary.empty() ? "nothing calibrated at this focal length" : c.summary];
        }
    }
    for (const auto& [key, row] : rows)
    {
        std::printf("%5d  %s\n       %s\n", row.files, key.c_str(), row.match.c_str());
        for (const auto& [what, n] : row.corrected)
            std::printf("       %5d  %s\n", n, what.c_str());
    }
    if (unread)
        std::printf("%d files weren't raws LibRaw opens\n", unread);
    return 0;
}

std::string state()
{
    switch (stage)
    {
        case Reading: return "reading the lens database";
        case Fetching: return "downloading lensfun's lens database";
        case Failed:
#ifdef HAVE_LENS_DOWNLOADS
            return downloads ? "no lens database (it couldn't be downloaded)" : "no lens database (downloads are off)";
#else
            return "no lens database (this build can't download it)";
#endif
        default: return "";
    }
}

}  // namespace lenses
