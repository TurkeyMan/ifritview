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
#include <new>
#include <span>
#include <system_error>

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

std::string thumbs_dir()
{
    return platform::join_path(platform::cache_dir(), "pin-thumbs");
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

void ThumbLoader::run()
{
    platform::lower_thread_priority();
    while (true)
    {
        Request request;
        {
            std::unique_lock lock(mutex_);
            working_.clear();
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            request = std::move(queue_.front());
            queue_.pop_front();
            working_ = request.key;
        }
        std::shared_ptr<Picture> picture;
        try
        {
            picture = load_kept(request.key);
            if (!picture)
            {
                picture = make(request);
                if (picture)
                    keep(request.key, *picture);
            }
        }
        catch (const std::bad_alloc&)
        {
            // Too big for now: an empty frame, and on to the next.
        }
        try
        {
            done_(request.key, std::move(picture));
        }
        catch (const std::bad_alloc&)
        {
        }
    }
}
