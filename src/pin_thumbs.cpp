#include "pin_thumbs.h"

#include "folder_index.h"
#include "platform.h"

#include <rapport/sys.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <new>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace
{

constexpr uintmax_t kMaxWhole = uintmax_t(48) << 20;  // a file decoded whole just for its pin

// Made once, kept in the cache folder: the photo's key (its path and modified
// time) names the file, so a changed photo's is simply not found. A little
// header, then the pixels, compressed.
constexpr int kSide = 112;                        // pixels: enough for a pin on a 2x display
constexpr char kMagic[4] = {'I', 'F', 'T', '1'};
constexpr uintmax_t kDiskBudget = uintmax_t(256) << 20;

std::string& kept_in()
{
    static std::string dir;
    return dir;
}

std::string thumbs_dir()
{
    return kept_in().empty() ? platform::join_path(platform::cache_dir(), "pin-thumbs") : kept_in();
}

std::string file_for(const std::string& key)
{
    // FNV-1a, 64 bits: a name, not a secret.
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : key)
        h = (h ^ c) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.thumb", static_cast<unsigned long long>(h));
    return platform::join_path(thumbs_dir(), name);
}

std::shared_ptr<ThumbLoader::Picture> load_kept(const std::string& key)
{
    // Never bigger than one of ours could be: anything else isn't read at all.
    const fs::path path = rap::sys::path_from_utf8(file_for(key));
    std::error_code ec;
    const uintmax_t file_size = fs::file_size(path, ec);
    const uintmax_t largest = 12 + key.size() + compressBound(uLong(kSide) * kSide * 4);
    if (ec || file_size > largest)
        return nullptr;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return nullptr;
    char head[12];
    if (!in.read(head, sizeof head) || std::memcmp(head, kMagic, 4) != 0)
        return nullptr;
    const int side = int(uint8_t(head[4])) | int(uint8_t(head[5])) << 8;
    const int format = uint8_t(head[6]);
    const uint32_t key_size = uint32_t(uint8_t(head[8])) | uint32_t(uint8_t(head[9])) << 8;
    if (side != kSide || format > 1 || key_size != key.size())
        return nullptr;
    std::string stored(key_size, '\0');
    if (!in.read(stored.data(), key_size) || stored != key)
        return nullptr;  // another photo's, its name the same by chance
    const std::string packed{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto picture = std::make_shared<ThumbLoader::Picture>();
    picture->side = side;
    picture->format = rap::pixel_format(format);
    picture->pixels.resize(size_t(side) * side * 4);
    uLongf size = uLongf(picture->pixels.size());
    if (uncompress(picture->pixels.data(), &size, reinterpret_cast<const Bytef*>(packed.data()), uLong(packed.size())) != Z_OK ||
        size != picture->pixels.size())
        return nullptr;
    return picture;
}

// The oldest made go until what's left fits.
void trim_kept()
{
    struct Entry
    {
        fs::path path;
        fs::file_time_type when;
        uintmax_t size;
    };
    std::vector<Entry> files;
    uintmax_t total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(rap::sys::path_from_utf8(thumbs_dir()), ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code e;
        const uintmax_t size = it->file_size(e);
        const fs::file_time_type when = it->last_write_time(e);
        if (!e)
        {
            files.push_back({it->path(), when, size});
            total += size;
        }
    }
    if (total <= kDiskBudget)
        return;
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.when < b.when; });
    for (const Entry& f : files)
    {
        if (total <= kDiskBudget)
            break;
        std::error_code e;
        if (fs::remove(f.path, e))
            total -= f.size;
    }
}

void keep(const std::string& key, const ThumbLoader::Picture& picture)
{
    static int written = 0;
    const std::string dir = thumbs_dir();
    if (g_mkdir_with_parents(dir.c_str(), 0700) != 0)
        return;
    std::vector<uint8_t> packed(compressBound(uLong(picture.pixels.size())));
    uLongf size = uLongf(packed.size());
    if (compress2(packed.data(), &size, picture.pixels.data(), uLong(picture.pixels.size()), 6) != Z_OK)
        return;
    const uint8_t head[12] = {uint8_t(kMagic[0]), uint8_t(kMagic[1]), uint8_t(kMagic[2]), uint8_t(kMagic[3]), uint8_t(picture.side), uint8_t(picture.side >> 8),
                              uint8_t(picture.format), 0, uint8_t(key.size()), uint8_t(key.size() >> 8), 0, 0};
    if (key.size() > 0xFFFF)
        return;
    const std::string path = file_for(key);
    char suffix[32];
    std::snprintf(suffix, sizeof suffix, ".%08x.partial", unsigned(g_random_int()));
    const fs::path target = rap::sys::path_from_utf8(path), partial = rap::sys::path_from_utf8(path + suffix);
    bool ok = false;
    {
        std::ofstream out(partial, std::ios::binary);
        out.write(reinterpret_cast<const char*>(head), sizeof head);
        out.write(key.data(), std::streamsize(key.size()));
        out.write(reinterpret_cast<const char*>(packed.data()), std::streamsize(size));
        out.close();
        ok = bool(out);
    }
    std::error_code ec;
    if (ok)
        fs::rename(partial, target, ec);
    if (!ok || ec)
        fs::remove(partial, ec);
    if (written++ % 256 == 0)
        trim_kept();
}

// The middle square of `d`, averaged down to kSide, turned upright.
std::shared_ptr<ThumbLoader::Picture> square(const rap::sys::decoded_image& d, int orientation)
{
    if (d.width <= 0 || d.height <= 0 || d.pixels.size() < size_t(d.width) * size_t(d.height) * 4)
        return nullptr;
    const int crop = std::min(d.width, d.height);
    const int ox = (d.width - crop) / 2, oy = (d.height - crop) / 2;
    std::vector<uint8_t> flat(size_t(kSide) * kSide * 4);
    for (int y = 0; y < kSide; ++y)
    {
        const int y0 = oy + y * crop / kSide, y1 = std::max(y0 + 1, oy + (y + 1) * crop / kSide);
        for (int x = 0; x < kSide; ++x)
        {
            const int x0 = ox + x * crop / kSide, x1 = std::max(x0 + 1, ox + (x + 1) * crop / kSide);
            unsigned sum[4] = {};
            for (int sy = y0; sy < y1; ++sy)
            {
                const uint8_t* s = d.pixels.data() + size_t(sy) * d.width * 4 + size_t(x0) * 4;
                for (int sx = x0; sx < x1; ++sx, s += 4)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += s[c];
            }
            const unsigned n = unsigned(x1 - x0) * unsigned(y1 - y0);
            uint8_t* o = flat.data() + (size_t(y) * kSide + x) * 4;
            for (int c = 0; c < 4; ++c)
                o[c] = uint8_t(sum[c] / n);
        }
    }

    auto picture = std::make_shared<ThumbLoader::Picture>();
    picture->side = kSide;
    picture->format = d.format;
    picture->pixels.resize(flat.size());
    const int m = kSide - 1;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x)
        {
            int sx = x, sy = y;
            switch (orientation)
            {
                case 2: sx = m - x; break;
                case 3: sx = m - x; sy = m - y; break;
                case 4: sy = m - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = m - x; break;
                case 7: sx = m - y; sy = m - x; break;
                case 8: sx = m - y; sy = x; break;
                default: break;
            }
            std::copy_n(flat.data() + (size_t(sy) * kSide + sx) * 4, 4, picture->pixels.data() + (size_t(y) * kSide + x) * 4);
        }
    return picture;
}

std::shared_ptr<ThumbLoader::Picture> make(const ThumbLoader::Request& request)
{
    const std::vector<uint8_t> embedded = embedded_thumbnail(request.path);
    if (!embedded.empty())
        if (const auto d = rap::sys::decode_image(std::span<const uint8_t>(embedded)))
            return square(*d, request.orientation);

    const fs::path path = rap::sys::path_from_utf8(request.path);
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kMaxWhole)
        return nullptr;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!in)
        return nullptr;
    const auto d = rap::sys::decode_image(std::span<const uint8_t>(bytes));
    return d ? square(*d, request.orientation) : nullptr;
}

}  // namespace

ThumbLoader::ThumbLoader(Done done) : done_(std::move(done))
{
    thread_ = std::thread([this] { run(); });
}

ThumbLoader::~ThumbLoader()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void ThumbLoader::want(std::vector<Request> requests)
{
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
        for (Request& r : requests)
            if (r.key != working_)  // it's on its way
                queue_.push_back(std::move(r));
    }
    wake_.notify_all();
}

uint64_t ThumbLoader::forget(std::function<void(bool)> done)
{
    uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
        working_.clear();  // what's being made is from before: asked for again, it's made again
        forget_ = true;
        if (done)
            forgotten_.push_back(std::move(done));
        generation = ++generation_;
    }
    wake_.notify_all();
    return generation;
}

void ThumbLoader::keep_in(std::string dir)
{
    kept_in() = std::move(dir);
}

bool ThumbLoader::forget_kept()
{
    std::error_code e;
    fs::remove_all(rap::sys::path_from_utf8(thumbs_dir()), e);
    if (e)
        platform::trace("pin thumbnails: not deleted: %s", e.message().c_str());
    return !e;
}

void ThumbLoader::run()
{
    platform::lower_thread_priority();
    while (true)
    {
        Request request;
        uint64_t generation = 0;
        {
            std::unique_lock lock(mutex_);
            working_.clear();
            wake_.wait(lock, [this] { return stop_ || forget_ || !queue_.empty(); });
            // Forgetting's done before closing too, after what was being made.
            if (std::exchange(forget_, false))
            {
                std::vector<std::function<void(bool)>> forgotten = std::move(forgotten_);
                forgotten_.clear();
                lock.unlock();
                const bool deleted = forget_kept();
                for (const auto& done : forgotten)
                    done(deleted);
                continue;
            }
            if (stop_)
                return;
            request = std::move(queue_.front());
            queue_.pop_front();
            working_ = request.key;
            generation = generation_;
        }
        const auto current = [&]
        {
            std::lock_guard lock(mutex_);
            return generation == generation_;
        };
        std::shared_ptr<Picture> picture;
        try
        {
            picture = load_kept(request.key);
            if (!picture)
            {
                picture = make(request);
                if (picture && current())  // else forgotten meanwhile (and if then, deleted next)
                    keep(request.key, *picture);
            }
        }
        catch (const std::bad_alloc&)
        {
            // Too big for now: an empty frame, and on to the next.
        }
        try
        {
            done_(request.key, std::move(picture), generation);
        }
        catch (const std::bad_alloc&)
        {
        }
    }
}

int check_thumbs_forget(const std::string& dir)
{
    using namespace std::chrono_literals;
    const std::string kept = platform::join_path(dir, "check-thumbs");
    const fs::path kept_path = rap::sys::path_from_utf8(kept);
    ThumbLoader::keep_in(kept);
    std::error_code e;
    fs::remove_all(kept_path, e);
    const auto fail = [](const char* what)
    {
        std::fprintf(stderr, "check-thumbs-forget: %s\n", what);
        return 1;
    };
    const auto none_kept = [&] { return !fs::exists(kept_path, e) || fs::is_empty(kept_path, e); };

    // A big plain BMP, slow enough to decode that it's still being made when
    // the pictures are forgotten.
    const int w = 4000, h = 3000;
    const uint32_t row = uint32_t(w) * 3, pixels = row * uint32_t(h);
    std::vector<uint8_t> bmp(54 + pixels, 0x80);
    const auto put32 = [&](size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            bmp[at + size_t(i)] = uint8_t(v >> (8 * i));
    };
    std::memset(bmp.data(), 0, 54);
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, uint32_t(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(w));
    put32(22, uint32_t(h));
    bmp[26] = 1;
    bmp[28] = 24;
    put32(34, pixels);
    const std::string photo = platform::join_path(dir, "check-thumb.bmp");
    std::ofstream(rap::sys::path_from_utf8(photo), std::ios::binary).write(reinterpret_cast<const char*>(bmp.data()), std::streamsize(bmp.size()));

    std::mutex m;
    std::vector<uint64_t> handed;
    {
        ThumbLoader loader(
            [&](const std::string&, std::shared_ptr<ThumbLoader::Picture>, uint64_t generation)
            {
                std::lock_guard lock(m);
                handed.push_back(generation);
            });
        // One made and kept, so there's something to forget.
        loader.want({{"before", photo, 1}});
        // A minute: a deadline for a slow machine (a big picture's decoded).
        for (const auto deadline = std::chrono::steady_clock::now() + 60s; none_kept() && std::chrono::steady_clock::now() < deadline;)
            std::this_thread::sleep_for(20ms);
        if (none_kept())
        {
            std::printf("check-thumbs-forget: skipped (this platform can't decode a BMP)\n");
            return 0;
        }
        // Another, being made as they're forgotten.
        loader.want({{"during", photo, 1}});
        std::this_thread::sleep_for(20ms);
        std::promise<bool> deleted;
        const uint64_t now = loader.forget([&](bool d) { deleted.set_value(d); });
        if (!deleted.get_future().get())
            return fail("the kept pictures couldn't be deleted");
        std::this_thread::sleep_for(500ms);  // time for what was being made to be kept, if it would be
        std::lock_guard lock(m);
        for (const uint64_t g : handed)
            if (g == now)
                return fail("a picture asked for before forgetting was handed back as current");
    }
    if (fs::exists(kept_path, e))
        return fail("a picture being made as they were forgotten was kept");

    // The picture being made as they're forgotten, asked for again straight
    // after (the map's pins come back): made again, and handed back as current.
    {
        std::promise<void> current;
        uint64_t now = ~uint64_t(0);
        bool seen = false;
        ThumbLoader loader(
            [&](const std::string& key, std::shared_ptr<ThumbLoader::Picture>, uint64_t generation)
            {
                std::lock_guard lock(m);
                if (key == "again" && generation == now && !seen)
                {
                    seen = true;
                    current.set_value();
                }
            });
        loader.want({{"again", photo, 1}});
        std::this_thread::sleep_for(20ms);
        {
            std::lock_guard lock(m);
            now = loader.forget();
        }
        loader.want({{"again", photo, 1}});
        if (current.get_future().wait_for(120s) != std::future_status::ready)  // two big pictures decoded, on a slow machine
            return fail("a picture asked for again after forgetting, while it was being made, never came");
    }
    std::printf("check-thumbs-forget: ok\n");
    return 0;
}
