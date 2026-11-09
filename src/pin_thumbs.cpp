#include "pin_thumbs.h"

#include "folder_index.h"
#include "platform.h"

#include <rapport/sys.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <new>
#include <span>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

constexpr int kSide = 112;                        // pixels: enough for a pin on a 2x display
constexpr uintmax_t kMaxWhole = uintmax_t(48) << 20;  // a file decoded whole just for its pin

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
            picture = make(request);
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
