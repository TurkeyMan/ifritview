#include "map_view.h"

#include "config.h"
#include "http.h"
#include "i18n.h"
#include "platform.h"

#include <rapport/sys.h>

#include <glib.h>
#include <glib/gstdio.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

constexpr int kTile = 256, kMinZoom = 2, kMaxZoom = 19;
constexpr size_t kMaxPreviewAhead = 8;  // for the button's little window
constexpr size_t kMaxAhead = 32;    // tiles fetched past the edges, when there's nothing else to fetch
constexpr double kMarginTiles = 1.5;  // how far past the view is fetched: the cone's far end
constexpr double kStillSpeed = 40;  // canvas px a second: slower than this, it's not going anywhere
constexpr int kPrimeLevels = 3;     // the wide picture fetched first is this many levels coarser
constexpr double kNeighbourhood = 14;  // the farthest out a photo opened from a pin is shown
constexpr double kZoomStep = 0.25;  // of a level (a doubling) per wheel notch
constexpr int kPinCell = 56;      // pins in one square this wide are drawn as one
constexpr size_t kMaxThumbs = 512;   // kept
constexpr size_t kThumbsAsked = 64;  // being made at a time, nearest first
constexpr size_t kThumbsNear = 256;  // the nearest, safe from being dropped: fewer than are kept
constexpr int kMaxFetches = 4;     // at once: the four tiles around the middle arrive together
constexpr size_t kMaxTiles = 256;  // in memory
constexpr auto kTileLife = std::chrono::hours(24 * 30);
constexpr uintmax_t kDiskBudget = uintmax_t(256) << 20;

const MapView::Provider kStreet[] = {
    {"OpenStreetMap", "https://tile.openstreetmap.org/{z}/{x}/{y}.png", "© OpenStreetMap contributors"},
};
const MapView::Provider kSatellite[] = {
    {"Powered by Esri", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}?blankTile=false",
     "Powered by Esri · Source: Esri, Maxar, Earthstar Geographics"},
};

uint64_t key_of(int layer, int z, int x, int y)
{
    return uint64_t(layer) << 56 | uint64_t(z) << 48 | uint64_t(x) << 24 | uint64_t(y);
}

double world_x(double longitude)
{
    return (longitude + 180) / 360;
}

double world_y(double latitude)
{
    const double r = std::clamp(latitude, -85.05112878, 85.05112878) * std::numbers::pi / 180;
    return (1 - std::asinh(std::tan(r)) / std::numbers::pi) / 2;
}

void replace_all(std::string& s, const std::string& what, const std::string& with)
{
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size()))
        s.replace(at, what.size(), with);
}

// In the system's temporary folder, one per user. That's shared on Linux, so
// it's used only if it's really this user's and no one else's: anyone can make
// a folder of that name there first. If not, the cache folder has them.
std::string tiles_dir()
{
    static const std::string dir = []
    {
        const std::string temp = platform::join_path(g_get_tmp_dir(), std::string("ifritview-") + g_get_user_name() + "-map-tiles");
#ifndef _WIN32
        g_mkdir(temp.c_str(), 0700);
        struct stat st;
        if (lstat(temp.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) != 0)
            return platform::join_path(platform::cache_dir(), "map-tiles");
#endif
        return temp;
    }();
    return dir;
}

// Those past their life go, then the oldest fetched until what's left fits.
void trim_disk(const std::string& dir)
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
    for (fs::recursive_directory_iterator it(rap::sys::path_from_utf8(dir), ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code e;
        if (!it->is_regular_file(e) || e)
            continue;
        const uintmax_t size = it->file_size(e);
        const fs::file_time_type when = it->last_write_time(e);
        if (!e && fs::file_time_type::clock::now() - when > kTileLife)
        {
            fs::remove(it->path(), e);
            continue;
        }
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

}  // namespace

const MapView::Provider* MapView::providers(bool satellite, size_t& count)
{
    count = satellite ? std::size(kSatellite) : std::size(kStreet);
    return satellite ? kSatellite : kStreet;
}

MapView::MapView(rap::ui& ui, Config& cfg) : ui_(ui), cfg_(cfg) {}

MapView::~MapView()
{
    loader_.reset();
    if (trim_.joinable())
        trim_.join();
}

// The one chosen, or with that one off, the other.
int MapView::layer() const
{
    if (cfg_.map_satellite)
        return cfg_.map_satellite_url.empty() ? 0 : 1;
    return cfg_.map_tile_url.empty() ? 1 : 0;
}

int MapView::tile_zoom() const
{
    return std::clamp(int(std::lround(zoom_)), kMinZoom, kMaxZoom);
}

const std::string& MapView::url_of(int layer) const
{
    return layer ? cfg_.map_satellite_url : cfg_.map_tile_url;
}

bool MapView::enabled() const
{
#ifdef HAVE_GEOCODE
    return !url_of(layer()).empty();
#else
    return false;
#endif
}

// The credit of the preset in use; a URL of the user's own has none we know.
const char* MapView::attribution(int layer) const
{
    size_t count = 0;
    const Provider* list = providers(layer == 1, count);
    for (size_t i = 0; i < count; ++i)
        if (url_of(layer) == list[i].url)
            return list[i].attribution;
    return "";
}

void MapView::config_changed()
{
    if (urls_[0] != cfg_.map_tile_url || urls_[1] != cfg_.map_satellite_url)
    {
        tiles_.clear();  // which drops their fetches
        map_mosaic_ = {};
        preview_mosaic_ = {};
        ++tiles_version_;
        fetching_ = 0;
        wanted_.clear();
        urls_[0] = cfg_.map_tile_url;
        urls_[1] = cfg_.map_satellite_url;
    }
    refresh();
}

void MapView::cancel_gesture()
{
    dragging_ = false;
    moved_ = false;
    plain_click_ = false;
    gesture_ = Gesture::none;
}

void MapView::stop_fetching()
{
    cancel_gesture();
    for (auto& [key, t] : tiles_)
        t.fetch.reset();
    fetching_ = 0;
    wanted_.clear();
    if (loader_)
        loader_->want({});
    asked_.clear();
}

void MapView::refresh()
{
    if (repaint_)
        repaint_();
}

void MapView::set_location(double latitude, double longitude, const std::string& path)
{
    if (path != path_)
    {
        path_ = path;
        clusters_zoom_ = -1;  // that one's the red pin now, not gray
        refresh();
    }
    if (has_ && latitude == lat_ && longitude == lon_)
    {
        keep_view_ = false;
        return;
    }
    has_ = true;
    lat_ = latitude;
    lon_ = longitude;
    pin_x_ = world_x(longitude);
    pin_y_ = world_y(latitude);
    if (!keep_view_)
    {
        centre_x_ = pin_x_;
        centre_y_ = pin_y_;
    }
    keep_view_ = false;
    refresh();
}

void MapView::set_pins(const std::vector<Pin>& pins)
{
    // What points into the old list goes with it, till the next paint.
    current_ = nullptr;
    clusters_.clear();
    drawn_.clear();
    pins_.clear();
    pins_.reserve(pins.size());
    for (const Pin& p : pins)
    {
        // -180 and +180 are one place.
        const double x = world_x(p.longitude);
        pins_.push_back({p.path, x - std::floor(x), world_y(p.latitude), p.path + "|" + std::to_string(p.modified), p.orientation});
    }
    clusters_zoom_ = -1;
    refresh();
}

// Pins closer than a pin's width at `level` make one: each joins the first
// group whose first pin it's near, so the groups don't change as the map's
// panned, and a stack isn't split by where some grid happens to fall.
std::vector<MapView::Cluster> MapView::group(const std::vector<size_t>& pins, int level) const
{
    const double world = double(int64_t(1) << level) * kTile;
    // Whole cells round the world, each at least a pin wide, so a neighbour's
    // always in the next cell, across the seam too.
    const int64_t across = std::max<int64_t>(1, int64_t(world / kPinCell));
    const double cell_size = world / double(across);
    std::vector<Cluster> out;
    std::unordered_map<int64_t, std::vector<size_t>> grid;  // groups by the cell of their first pin
    const auto cell = [&](int64_t cx, int64_t cy) { return ((cx % across + across) % across) << 32 | ((cy + 1) & 0xFFFFFFFF); };
    for (const size_t i : pins)
    {
        const Placed& p = pins_[i];
        const int64_t cx = int64_t(p.x * world / cell_size), cy = int64_t(p.y * world / cell_size);
        size_t joined = out.size();
        for (int64_t dy = -1; dy <= 1 && joined == out.size(); ++dy)
            for (int64_t dx = -1; dx <= 1 && joined == out.size(); ++dx)
            {
                const auto it = grid.find(cell(cx + dx, cy + dy));
                if (it == grid.end())
                    continue;
                for (const size_t g : it->second)
                {
                    const Placed& first = pins_[out[g].first];
                    double ax = p.x - first.x;
                    ax -= std::round(ax);  // the nearer way round the world
                    const double px = ax * world, py = (p.y - first.y) * world;
                    if (px * px + py * py < double(kPinCell) * kPinCell)
                    {
                        joined = g;
                        break;
                    }
                }
            }
        if (joined == out.size())
        {
            Cluster c;
            c.first = i;
            c.x = p.x;
            c.y = p.y;
            out.push_back(std::move(c));
            grid[cell(cx, cy)].push_back(joined);
        }
        Cluster& c = out[joined];
        c.members.push_back(i);
        ++c.count;
        double ax = p.x - c.x;
        ax -= std::round(ax);
        c.x += ax / double(c.count);
        c.y += (p.y - c.y) / double(c.count);
    }
    return out;
}

void MapView::build_clusters()
{
    clusters_zoom_ = tile_zoom();
    drawn_.clear();
    current_ = nullptr;
    std::vector<size_t> others;
    others.reserve(pins_.size());
    for (size_t i = 0; i < pins_.size(); ++i)
    {
        if (pins_[i].path == path_)
            current_ = &pins_[i];
        else
            others.push_back(i);
    }
    clusters_ = group(others, clusters_zoom_);
}

void MapView::pick(size_t cluster)
{
    if (cluster >= clusters_.size())
        return;
    const Cluster c = clusters_[cluster];
    // A photo opened from far out comes close enough to see where it was
    // taken; from nearer, the map stays where it is.
    const auto open = [this](const Placed& pin)
    {
        if (!on_pick)
            return;
        if (zoom_ < kNeighbourhood)
        {
            zoom_ = kNeighbourhood;
            centre_x_ = pin.x;
            centre_y_ = pin.y;
            refresh();
        }
        keep_view_ = true;
        on_pick(pin.path);
    };
    if (c.count == 1)
    {
        open(pins_[c.first]);
        return;
    }

    // The different places in it, and where they lie, measured from the
    // first of them the short way round the world.
    const Placed& origin = pins_[c.first];
    std::set<std::pair<double, double>> places;
    double x0 = 0, x1 = 0, y0 = origin.y, y1 = origin.y;
    for (const size_t i : c.members)
    {
        places.emplace(pins_[i].x, pins_[i].y);
        double rx = pins_[i].x - origin.x;
        rx -= std::round(rx);
        x0 = std::min(x0, rx);
        x1 = std::max(x1, rx);
        y0 = std::min(y0, pins_[i].y);
        y1 = std::max(y1, pins_[i].y);
    }

    if (places.size() < 2 || group(c.members, kMaxZoom).size() == 1)
    {
        // No zoom will part them, so each click the next after the photo
        // shown (if it's one of them too), going through every one.
        std::vector<size_t> cycle = c.members;
        if (current_)
        {
            std::vector<size_t> with = c.members;
            with.push_back(size_t(current_ - pins_.data()));
            if (group(with, kMaxZoom).size() == 1)
                cycle = std::move(with);
        }
        std::sort(cycle.begin(), cycle.end());
        size_t after = 0;
        for (size_t i = 0; i < cycle.size(); ++i)
            if (pins_[cycle[i]].path == path_)
                after = i + 1;
        open(pins_[cycle[after % cycle.size()]]);
        return;
    }

    // The first level where each place has a pin of its own, but no closer
    // than shows them all.
    int level = std::min(tile_zoom() + 1, kMaxZoom);
    for (; level < kMaxZoom; ++level)
        if (group(c.members, level).size() == places.size())
            break;
    double target = level;
    const double extent = std::max(x1 - x0, y1 - y0);
    if (extent > 0 && size_.w > 0 && size_.h > 0)
        target = std::min(target, std::log2(0.7 * std::min(size_.w, size_.h) / (extent * kTile)));
    zoom_ = std::clamp(std::max(target, zoom_ + 0.5), double(kMinZoom), double(kMaxZoom));
    centre_x_ = origin.x + (x0 + x1) / 2;
    centre_x_ -= std::floor(centre_x_);
    // A little lower down the world than their middle, for the pins stand above their points.
    centre_y_ = (y0 + y1) / 2 - 26 / (kTile * std::pow(2.0, zoom_));
    refresh();
}

void MapView::draw_pin(rap::painter& p, float x, float y, const Placed* placed, int count, bool selected, Drawn* hit)
{
    const float inner = selected ? 46.f : 36.f, border = selected ? 4.f : 3.f, outer = inner + 2 * border, tip = selected ? 11.f : 9.f;
    const float left = x - outer / 2, top = y - tip - outer, mid = top + outer / 2;
    const rap::color frame = selected ? rap::color{214, 52, 42} : rap::color{255, 255, 255};

    // The cards behind, a stack's.
    if (count > 1)
    {
        const rap::color card{240, 241, 244}, line{170, 175, 185};
        for (const float degrees : {14.f, -10.f})
        {
            const float r = degrees * float(std::numbers::pi) / 180, c = std::cos(r), s = std::sin(r), h = outer / 2 - 1;
            std::vector<rap::point_f> corners;
            for (const auto& [a, b] : {std::pair{-h, -h}, std::pair{h, -h}, std::pair{h, h}, std::pair{-h, h}})
                corners.push_back({x + a * c - b * s, mid + a * s + b * c});
            p.polygon(corners, card, line, 1);
        }
    }

    p.rect({left, top + 2, outer, outer}, 7, rap::color{0, 0, 0, 60}, std::nullopt);
    p.polygon({{x - tip, top + outer - 2}, {x + tip, top + outer - 2}, {x, y}}, frame, std::nullopt);
    p.rect({left, top, outer, outer}, 7, frame, std::nullopt);

    const auto it = placed ? thumbs_.find(placed->thumb) : thumbs_.end();
    const rap::rect_f picture{left + border, top + border, inner, inner};
    if (it != thumbs_.end() && it->second.image)
    {
        it->second.used = clock_;
        const float side = float(it->second.image->width());
        p.image(*it->second.image, {0, 0, side, side}, picture);
    }
    else
        p.rect(picture, 3, rap::color{203, 208, 216}, std::nullopt);

    if (count > 1)
    {
        const std::string n = count > 99 ? "99+" : std::to_string(count);
        rap::text_style style;
        style.fg = rap::color{255, 255, 255};
        style.size = 7;
        style.bold = true;
        const rap::size_f t = p.measure(n, style);
        const float bx = left + outer - 3, by = top + 3, r = std::max(8.5f, t.w / 2 + 4);
        p.circle({bx, by}, r, rap::color{35, 38, 46}, std::nullopt);
        p.text({bx - t.w / 2, by - t.h / 2}, n, style);
    }
    if (hit)
    {
        hit->x = x;
        hit->y = mid;
        hit->radius = outer / 2 + 3;
    }
}

// What's wanted and isn't here yet, the most wanted first.
void MapView::ask_thumbs(const std::vector<const Placed*>& wanted)
{
    std::vector<ThumbLoader::Request> requests;
    std::vector<std::string> keys;
    for (size_t i = 0; i < wanted.size() && i < kThumbsNear; ++i)
    {
        if (requests.size() >= kThumbsAsked)
            break;
        const Placed* placed = wanted[i];
        Thumb& t = thumbs_[placed->thumb];
        t.used = clock_;
        if (t.image || t.failed)
            continue;
        keys.push_back(placed->thumb);
        requests.push_back({placed->thumb, placed->path, placed->orientation});
    }
    trim_thumbs();
    if (keys == asked_)
        return;
    asked_ = std::move(keys);
    if (!loader_)
        loader_ = std::make_unique<ThumbLoader>(
            [this, alive = std::weak_ptr<char>(alive_)](const std::string& key, std::shared_ptr<ThumbLoader::Picture> picture)
            {
                platform::post_main(
                    [this, alive, key, picture = std::move(picture)]() mutable
                    {
                        if (alive.lock())
                            thumb_ready(key, std::move(picture));
                    });
            });
    loader_->want(std::move(requests));
}

void MapView::trim_thumbs()
{
    while (thumbs_.size() > kMaxThumbs)
    {
        auto oldest = thumbs_.end();
        for (auto it = thumbs_.begin(); it != thumbs_.end(); ++it)
            if (it->second.used != clock_ && (oldest == thumbs_.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == thumbs_.end())
            break;
        thumbs_.erase(oldest);
    }
}

void MapView::thumb_ready(const std::string& key, std::shared_ptr<ThumbLoader::Picture> picture)
{
    Thumb& t = thumbs_[key];
    t.used = clock_;
    if (picture)
        t.image = ui_.create_image(picture->side, picture->side, picture->format, picture->pixels.data(), size_t(picture->side) * 4);
    t.failed = !t.image;
    trim_thumbs();
    refresh();
}

std::string MapView::file_of(const Tile& t) const
{
    // By where they came from: another provider's aren't these.
    char name[64];
    std::snprintf(name, sizeof name, "%08x/%d/%d/%d", unsigned(std::hash<std::string>{}(url_of(t.layer))), t.z, t.x, t.y);
    return platform::join_path(tiles_dir(), name);
}

// What a cell of the mosaic would be made from, as a number that's the same
// only if the pixels would be: the tile it uses, and if that's being scaled,
// the tiles around it the filter reads. 0 if there's nothing yet.
uint64_t MapView::source_of(int layer, int z, int x, int y) const
{
    for (int up = 0; up <= std::min(z, 5); ++up)
    {
        const auto it = tiles_.find(key_of(layer, z - up, x >> up, y >> up));
        if (it == tiles_.end() || it->second.pixels.empty())
            continue;
        uint64_t mix = uint64_t(up + 1);
        const auto add = [&mix](uint64_t v) { mix = (mix ^ v) * 1099511628211ull; };
        add(it->second.generation);
        if (up > 0 || it->second.width != kTile || it->second.height != kTile)
        {
            const int coarse = z - up, across = 1 << coarse, cx = x >> up, cy = y >> up;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    if (cy + dy < 0 || cy + dy >= across)
                    {
                        add(0);
                        continue;
                    }
                    const auto n = tiles_.find(key_of(layer, coarse, ((cx + dx) % across + across) % across, cy + dy));
                    add(n != tiles_.end() && !n->second.pixels.empty() ? n->second.generation : 0);
                }
        }
        return mix ? mix : 1;
    }
    return 0;
}

// A tile into the mosaic at (ox, oy), or while it isn't here the part of the
// nearest coarser one that covers it, so zooming shows something at once.
void MapView::blit_tile(Mosaic& m, int ox, int oy, int layer, int z, int x, int y)
{
    for (int up = 0; up <= std::min(z, 5); ++up)
    {
        const auto it = tiles_.find(key_of(layer, z - up, x >> up, y >> up));
        if (it == tiles_.end() || it->second.pixels.empty())
            continue;
        Tile& s = it->second;
        s.used = clock_;
        m.format = s.format;
        const size_t stride = size_t(m.width) * 4;
        if (up == 0 && s.width == kTile && s.height == kTile)
        {
            for (int row = 0; row < kTile; ++row)
                std::memcpy(m.pixels.data() + size_t(oy + row) * stride + size_t(ox) * 4, s.pixels.data() + size_t(row) * size_t(kTile) * 4, size_t(kTile) * 4);
            return;
        }
        // Its part, scaled up smoothly: filtering reaches into the coarse
        // tiles around it, so where two coarse tiles meet there's no join.
        const int coarse = z - up, across = 1 << coarse, cx = x >> up, cy = y >> up;
        const Tile* around[3][3] = {};
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (cy + dy < 0 || cy + dy >= across)
                    continue;
                const auto n = tiles_.find(key_of(layer, coarse, ((cx + dx) % across + across) % across, cy + dy));
                if (n != tiles_.end() && n->second.width == s.width && n->second.height == s.height && !n->second.pixels.empty())
                    around[dy + 1][dx + 1] = &n->second;
            }
        // A pixel of the coarse level, measured from this tile's corner; off
        // the edge of what's here, the nearest in this tile.
        const auto texel = [&](int u, int v) -> const uint8_t*
        {
            const int col = u < 0 ? 0 : u >= s.width ? 2 : 1, row = v < 0 ? 0 : v >= s.height ? 2 : 1;
            const Tile* from = around[row][col];
            if (from)
                return from->pixels.data() + (size_t(v - (row - 1) * s.height) * s.width + size_t(u - (col - 1) * s.width)) * 4;
            return s.pixels.data() + (size_t(std::clamp(v, 0, s.height - 1)) * s.width + size_t(std::clamp(u, 0, s.width - 1))) * 4;
        };
        const double step_x = double(s.width) / double(1 << up) / kTile, step_y = double(s.height) / double(1 << up) / kTile;
        const double from_x = (x & ((1 << up) - 1)) * double(s.width) / double(1 << up), from_y = (y & ((1 << up) - 1)) * double(s.height) / double(1 << up);
        for (int row = 0; row < kTile; ++row)
        {
            const double v = from_y + (row + 0.5) * step_y - 0.5;
            const int v0 = int(std::floor(v));
            const int fv = int((v - v0) * 256);
            uint8_t* out = m.pixels.data() + size_t(oy + row) * stride + size_t(ox) * 4;
            for (int col = 0; col < kTile; ++col, out += 4)
            {
                const double u = from_x + (col + 0.5) * step_x - 0.5;
                const int u0 = int(std::floor(u));
                const int fu = int((u - u0) * 256);
                const uint8_t* a = texel(u0, v0);
                const uint8_t* b = texel(u0 + 1, v0);
                const uint8_t* c = texel(u0, v0 + 1);
                const uint8_t* d = texel(u0 + 1, v0 + 1);
                for (int ch = 0; ch < 4; ++ch)
                {
                    const int top_row = a[ch] * (256 - fu) + b[ch] * fu, bottom_row = c[ch] * (256 - fu) + d[ch] * fu;
                    out[ch] = uint8_t((top_row * (256 - fv) + bottom_row * fv) >> 16);
                }
            }
        }
        return;
    }
}

void MapView::draw_mosaic(rap::painter& p, Mosaic& m, int layer, int level, int x0, int x1, int y0, int y1, double left, double top, double tile)
{
    const int cols = x1 - x0 + 1, rows = y1 - y0 + 1;
    if (cols <= 0 || rows <= 0)
        return;
    if (m.layer != layer || m.level != level || m.x0 != x0 || m.x1 != x1 || m.y0 != y0 || m.y1 != y1 || m.version != tiles_version_)
    {
        const int width = cols * kTile, height = rows * kTile;
        const bool resized = width != m.width || height != m.height;
        const bool same_map = m.layer == layer && m.level == level;
        const rap::pixel_format was = m.format;
        const std::vector<uint8_t> old = std::move(m.pixels);
        const std::vector<uint64_t> old_sources = std::move(m.sources);
        const int old_x0 = m.x0, old_x1 = m.x1, old_y0 = m.y0, old_y1 = m.y1, old_width = m.width;
        m.width = width;
        m.height = height;
        m.layer = layer;
        m.level = level;
        m.x0 = x0;
        m.x1 = x1;
        m.y0 = y0;
        m.y1 = y1;
        m.version = tiles_version_;
        m.pixels.assign(size_t(width) * size_t(height) * 4, 0);
        m.sources.assign(size_t(cols) * rows, 0);
        bool any = false;
        const int count = 1 << level;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
            {
                const int wrapped = ((x % count) + count) % count;
                const uint64_t source = source_of(layer, level, wrapped, y);
                m.sources[size_t(y - y0) * cols + (x - x0)] = source;
                if (source == 0)
                    continue;
                any = true;
                const int ox = (x - x0) * kTile, oy = (y - y0) * kTile;
                // Already there, as it would be made again: moved over.
                if (same_map && x >= old_x0 && x <= old_x1 && y >= old_y0 && y <= old_y1 &&
                    old_sources[size_t(y - old_y0) * (old_x1 - old_x0 + 1) + (x - old_x0)] == source)
                {
                    const int from_x = (x - old_x0) * kTile, from_y = (y - old_y0) * kTile;
                    for (int row = 0; row < kTile; ++row)
                        std::memcpy(m.pixels.data() + size_t(oy + row) * size_t(width) * 4 + size_t(ox) * 4,
                                    old.data() + size_t(from_y + row) * size_t(old_width) * 4 + size_t(from_x) * 4, size_t(kTile) * 4);
                    continue;
                }
                blit_tile(m, ox, oy, layer, level, wrapped, y);
            }
        if (!any)
            m.image.reset();
        else if (m.image && !resized && was == m.format)
            m.image->update(m.pixels.data(), size_t(width) * 4);
        else
            m.image = ui_.create_image(width, height, m.format, m.pixels.data(), size_t(width) * 4);
    }
    if (m.image)
        p.image(*m.image, {0, 0, float(m.width), float(m.height)},
                {float(x0 * tile - left), float(y0 * tile - top), float(cols * tile), float(rows * tile)});
}

namespace
{

// Another painter, its origin moved.
class MovedPainter : public rap::painter
{
public:
    MovedPainter(rap::painter& p, rap::point_f by) : p_(p), by_(by) {}

    rap::color resolve(const rap::paint& v) const override { return p_.resolve(v); }
    void line(rap::point_f a, rap::point_f b, const rap::paint& v, float w, rap::line_cap c) override { p_.line(at(a), at(b), v, w, c); }
    void curve(rap::point_f a, rap::point_f c1, rap::point_f c2, rap::point_f b, const rap::paint& v, float w) override { p_.curve(at(a), at(c1), at(c2), at(b), v, w); }
    void circle(rap::point_f c, float r, std::optional<rap::paint> f, std::optional<rap::paint> s) override { p_.circle(at(c), r, std::move(f), std::move(s)); }
    void rect(rap::rect_f r, float c, std::optional<rap::paint> f, std::optional<rap::paint> s) override { p_.rect(at(r), c, std::move(f), std::move(s)); }
    void polygon(const std::vector<rap::point_f>& ps, std::optional<rap::paint> f, std::optional<rap::paint> s, float w) override
    {
        std::vector<rap::point_f> moved;
        moved.reserve(ps.size());
        for (const rap::point_f& q : ps)
            moved.push_back(at(q));
        p_.polygon(moved, std::move(f), std::move(s), w);
    }
    void arc(rap::point_f c, float r, float a, float b, const rap::paint& v, float w, rap::line_cap cap) override { p_.arc(at(c), r, a, b, v, w, cap); }
    void image(const rap::image& im, rap::rect_f src, rap::rect_f dst, rap::filter f, float alpha) override { p_.image(im, src, at(dst), f, alpha); }
    float device_scale() const override { return p_.device_scale(); }
    float pixel_scale() const override { return p_.pixel_scale(); }
    void push_clip(rap::rect_f r) override { p_.push_clip(at(r)); }
    void pop_clip() override { p_.pop_clip(); }
    void text(rap::point_f q, const std::string& t, const rap::text_style& s) override { p_.text(at(q), t, s); }
    rap::size_f measure(const std::string& t, const rap::text_style& s) const override { return p_.measure(t, s); }

private:
    rap::point_f at(rap::point_f q) const { return {q.x + by_.x, q.y + by_.y}; }
    rap::rect_f at(rap::rect_f r) const { return {r.x + by_.x, r.y + by_.y, r.w, r.h}; }

    rap::painter& p_;
    rap::point_f by_;
};

bool inside(rap::point_f q, rap::rect_f r)
{
    return q.x >= r.x && q.x < r.x + r.w && q.y >= r.y && q.y < r.y + r.h;
}

}  // namespace

void MapView::paint_in(rap::painter& p, rap::rect_f area)
{
    p.rect({area.x - 3, area.y - 3, area.w + 6, area.h + 6}, 6, rap::color{255, 255, 255}, std::nullopt);
    p.push_clip(area);
    MovedPainter moved(p, {area.x, area.y});
    paint(moved, {area.w, area.h});
    p.pop_clip();
}

bool MapView::pointer_in(const rap::pointer_event& e, rap::rect_f area)
{
    using T = rap::pointer_event_type;
    const bool over = inside(e.pos, area);
    // A gesture belongs to whichever side got its press, to its release, in
    // and out of the map as it goes.
    if (e.type == T::press && gesture_ == Gesture::none)
    {
        gesture_ = over ? Gesture::map : Gesture::view;
        gesture_button_ = e.button;
    }
    if (gesture_ == Gesture::view)
    {
        if (e.type == T::release && e.button == gesture_button_)
            gesture_ = Gesture::none;
        return false;
    }
    rap::pointer_event moved = e;
    moved.pos.x -= area.x;
    moved.pos.y -= area.y;
    if (gesture_ == Gesture::map)
    {
        pointer(moved);
        if (e.type == T::release && e.button == gesture_button_)
            gesture_ = Gesture::none;
        return true;
    }
    // A release with no press of ours is the end of the view's gesture, one
    // begun before the map came up: the view needs to see it.
    if (e.type == T::release)
        return false;
    return over && (pointer(moved) || true);  // the view under it doesn't get what's over the map
}

bool MapView::scroll_in(const rap::scroll_event& e, rap::rect_f area)
{
    if (!inside(e.pos, area))
        return false;
    rap::scroll_event moved = e;
    moved.pos.x -= area.x;
    moved.pos.y -= area.y;
    return scroll(moved);
}

void MapView::paint(rap::painter& p, rap::size_f size)
{
    ++clock_;
    size_ = size;
    p.rect({0, 0, size.w, size.h}, 0, rap::paint{rap::theme_color::base}, std::nullopt);
    if (!has_)
        return;

    const int level = tile_zoom();
    const int count = 1 << level;
    world_ = kTile * std::pow(2.0, zoom_);
    const double tile = world_ / count;  // a tile's size on the canvas
    const double left = std::round(centre_x_ * world_ - size.w / 2), top = std::round(centre_y_ * world_ - size.h / 2);
    const int x0 = int(std::floor(left / tile)), x1 = int(std::floor((left + size.w) / tile));
    const int y0 = std::max(0, int(std::floor(top / tile))), y1 = std::min(count - 1, int(std::floor((top + size.h) / tile)));

    struct Need
    {
        uint64_t key;
        float distance;
    };
    std::vector<Need> need;
    std::vector<uint64_t> stand_ins;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const int wrapped = ((x % count) + count) % count;
            const uint64_t key = key_of(layer(), level, wrapped, y);
            Tile& t = tiles_[key];
            t.layer = layer();
            t.z = level;
            t.x = wrapped;
            t.y = y;
            t.used = clock_;
            const float dx = float((x + 0.5) * tile - left) - size.w / 2, dy = float((y + 0.5) * tile - top) - size.h / 2;
            need.push_back({key, dx * dx + dy * dy});
            want_parents(layer(), level, wrapped, y, stand_ins);
        }
    std::sort(need.begin(), need.end(), [](const Need& a, const Need& b) { return a.distance < b.distance; });
    wanted_.clear();
    want_prime(layer(), level, left, top, size);
    for (const Need& n : need)
        wanted_.push_back(n.key);
    wanted_.insert(wanted_.end(), stand_ins.begin(), stand_ins.end());
    draw_mosaic(p, map_mosaic_, layer(), level, x0, x1, y0, y1, left, top, tile);

    // The folder's other photos, the ones nearest the bottom in front and the
    // red pin over them all. Thumbnails are made for those a good way past
    // the edges too, so zooming out or panning finds them ready.
    if (clusters_zoom_ != tile_zoom())
        build_clusters();
    drawn_.clear();
    {
        struct Seen
        {
            float x, y;
            size_t cluster;
        };
        std::vector<Seen> seen;
        struct Near
        {
            float distance;
            const Placed* placed;
        };
        std::vector<Near> near_pins;
        const float margin = std::max(size.w, size.h);
        for (size_t i = 0; i < clusters_.size(); ++i)
        {
            const Cluster& c = clusters_[i];
            double dx = c.x - centre_x_;
            dx -= std::round(dx);  // the nearer way round the world
            const float x = float(dx * world_) + size.w / 2, y = float((c.y - centre_y_) * world_) + size.h / 2;
            if (x < -margin || x > size.w + margin || y < -margin || y > size.h + margin)
                continue;
            near_pins.push_back({std::hypot(x - size.w / 2, y - size.h / 2), &pins_[c.first]});
            if (x > -50 && x < size.w + 50 && y > -8 && y < size.h + 70)
                seen.push_back({x, y, i});
        }
        std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.y < b.y; });
        for (const Seen& s : seen)
        {
            Drawn d;
            d.cluster = s.cluster;
            const Cluster& c = clusters_[s.cluster];
            draw_pin(p, s.x, s.y, &pins_[c.first], c.count, false, &d);
            drawn_.push_back(d);
        }
        double pin_dx = pin_x_ - centre_x_;
        pin_dx -= std::round(pin_dx);  // the nearer way round the world
        draw_pin(p, float(pin_dx * world_) + size.w / 2, float((pin_y_ - centre_y_) * world_) + size.h / 2, current_, 1, true, &selected_);

        std::sort(near_pins.begin(), near_pins.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
        std::vector<const Placed*> wanted;
        if (current_)
            wanted.push_back(current_);
        for (const Near& n : near_pins)
            wanted.push_back(n.placed);
        ask_thumbs(wanted);
    }

    // Street / satellite, if both are on: a window onto the other one, lined
    // up with the map under it, moving as it does.
    chip_ = {};
    const bool both = !cfg_.map_tile_url.empty() && !cfg_.map_satellite_url.empty();
    const bool live = both && cfg_.map_live_toggle;
    if (both && !live)
    {
        // Turned off in Settings, not to fetch the other map too: just a button.
        const std::string name = layer() ? _("Street") : _("Satellite");
        rap::text_style label;
        label.fg = rap::color{255, 255, 255};
        const rap::size_f t = p.measure(name, label);
        chip_ = {size.w - t.w - 24, 8, t.w + 16, t.h + 6};
        p.rect(chip_, 4, rap::color{0, 0, 0, 170}, std::nullopt);
        p.text({chip_.x + 8, chip_.y + 3}, name, label);
    }
    if (live)
    {
        const int other = 1 - layer();
        const float w = 84, h = 60;
        chip_ = {size.w - w - 8, 8, w, h};
        p.rect({chip_.x, chip_.y + 2, w, h}, 7, rap::color{0, 0, 0, 70}, std::nullopt);
        p.rect(chip_, 7, rap::color{255, 255, 255}, std::nullopt);
        const rap::rect_f inside{chip_.x + 3, chip_.y + 3, w - 6, h - 6};
        p.rect(inside, 3, rap::paint{rap::theme_color::base}, std::nullopt);

        p.push_clip(inside);
        const int cx0 = int(std::floor((left + inside.x) / tile)), cx1 = int(std::floor((left + inside.x + inside.w) / tile));
        const int cy0 = std::max(0, int(std::floor((top + inside.y) / tile))), cy1 = std::min(count - 1, int(std::floor((top + inside.y + inside.h) / tile)));
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x)
            {
                const int wrapped = ((x % count) + count) % count;
                const uint64_t key = key_of(other, level, wrapped, y);
                Tile& t = tiles_[key];
                t.layer = other;
                t.z = level;
                t.x = wrapped;
                t.y = y;
                t.used = clock_;
                wanted_.push_back(key);  // after the map's own
                want_parents(other, level, wrapped, y, wanted_);
            }
        draw_mosaic(p, preview_mosaic_, other, level, cx0, cx1, cy0, cy1, left, top, tile);
        p.pop_clip();
        // It moves with the map, so it wants what comes next as much as the map does.
        want_prime(other, level, left, top, size);
        want_ahead(other, kMaxPreviewAhead, level, cx0, cx1, cy0, cy1, left + inside.x, top + inside.y, tile, {inside.w, inside.h});
    }

    // Who drew what's showing, as their terms ask.
    if (const std::string by = attribution(layer()); !by.empty())
    {
        rap::text_style credit;
        credit.fg = rap::color{75, 75, 75};
        credit.size = 6.5f;
        const rap::size_f c = p.measure(by, credit);
        p.rect({size.w - c.w - 5, size.h - c.h - 1, c.w + 5, c.h + 1}, 0, rap::color{255, 255, 255, 120}, std::nullopt);
        p.text({size.w - c.w - 2.5f, size.h - c.h}, by, credit);
    }

    want_ahead(layer(), kMaxAhead, level, x0, x1, y0, y1, left, top, tile, size);
    // The map's and the button's, the most valuable first.
    std::sort(ahead_.begin(), ahead_.end(), [](const Prefetch& a, const Prefetch& b) { return a.covers != b.covers ? a.covers > b.covers : a.distance < b.distance; });
    for (const Prefetch& a : ahead_)
        wanted_.push_back(a.key);
    ahead_.clear();

    pump();
}

// First a coarser picture of the view and two canvases around it: a few tiles
// cover the lot, so what's panned to or still loading shows it, scaled up,
// rather than bare background.
void MapView::want_prime(int which, int level, double left, double top, rap::size_f size)
{
    const int prime = std::max(kMinZoom, level - kPrimeLevels);
    if (prime >= level)
        return;
    const int across = 1 << prime;
    const double prime_tile = world_ / across;
    const int px0 = int(std::floor((left - 2 * size.w) / prime_tile)), px1 = int(std::floor((left + 3 * size.w) / prime_tile));
    const int py0 = std::max(0, int(std::floor((top - 2 * size.h) / prime_tile))), py1 = std::min(across - 1, int(std::floor((top + 3 * size.h) / prime_tile)));
    for (int y = py0; y <= py1; ++y)
        for (int x = px0; x <= px1 && x - px0 < across; ++x)
        {
            const int wrapped = ((x % across) + across) % across;
            const uint64_t key = key_of(which, prime, wrapped, y);
            Tile& t = tiles_[key];
            t.layer = which;
            t.z = prime;
            t.x = wrapped;
            t.y = y;
            t.used = clock_;
            wanted_.push_back(key);
        }
}

// With everything on screen asked for, what's about to come into view. An
// ice-cream cone is cast from the middle of the view to where it's heading,
// opening to the margin at its far end (still, it's just the margin around
// the middle), and tiles grown by half the view are tested against it. The
// tiles that touch are wanted, first those that'd cover the most of the view
// if it keeps going the way it is, then the nearest.
void MapView::want_ahead(int which, size_t limit, int level, int x0, int x1, int y0, int y1, double left, double top, double tile, rap::size_f size)
{
    using namespace std::chrono;
    const bool moving = duration<double>(steady_clock::now() - last_move_).count() < 0.3 && std::hypot(view_vx_, view_vy_) >= kStillSpeed;
    // Far enough ahead to matter by the time a fetch started now could land:
    // a fetch's usual time, and as long again for those queued before it.
    const double horizon = std::clamp(fetch_seconds_ * 2, 0.2, 2.0);
    double reach_x = moving ? view_vx_ * horizon : 0, reach_y = moving ? view_vy_ * horizon : 0;
    if (const double reach = std::hypot(reach_x, reach_y); reach > tile * 8)
    {
        reach_x *= tile * 8 / reach;
        reach_y *= tile * 8 / reach;
    }
    const int count = 1 << level;
    const double margin = tile * kMarginTiles;
    const double mid_x = left + size.w / 2, mid_y = top + size.h / 2;
    const double half_w = size.w / 2, half_h = size.h / 2;
    const int steps = moving ? 8 : 1;
    const double gap = std::hypot(reach_x, reach_y) / steps / 2;  // covers the way between one step and the next

    struct Ahead
    {
        double covers, distance;
        int x, y;
    };
    std::vector<Ahead> ahead;
    const int tx0 = int(std::floor((left - margin + std::min(0.0, reach_x)) / tile)), tx1 = int(std::floor((left + size.w + margin + std::max(0.0, reach_x)) / tile));
    const int ty0 = std::max(0, int(std::floor((top - margin + std::min(0.0, reach_y)) / tile)));
    const int ty1 = std::min(count - 1, int(std::floor((top + size.h + margin + std::max(0.0, reach_y)) / tile)));
    for (int y = ty0; y <= ty1; ++y)
        for (int x = tx0; x <= tx1; ++x)
        {
            if (x >= x0 && x <= x1 && y >= y0 && y <= y1)
                continue;  // on screen: asked for already
            const double l = x * tile, r = (x + 1) * tile, t = y * tile, b = (y + 1) * tile;
            bool touches = false;
            double covers = 0;
            for (int step = 1; step <= steps; ++step)
            {
                const double f = double(step) / steps;
                const double cx = mid_x + reach_x * f, cy = mid_y + reach_y * f;
                const double dx = std::max({l - half_w - cx, cx - r - half_w, 0.0}), dy = std::max({t - half_h - cy, cy - b - half_h, 0.0});
                touches = touches || std::hypot(dx, dy) <= margin * f + gap;
                const double w = std::min(r, cx + half_w) - std::max(l, cx - half_w), h = std::min(b, cy + half_h) - std::max(t, cy - half_h);
                // How much of the view it'd fill there, if it can have landed by then.
                if (moving && f * horizon >= fetch_seconds_ && w > 0 && h > 0)
                    covers += w * h / (size.w * size.h * steps);
            }
            if (touches)
                ahead.push_back({covers, std::hypot((l + r) / 2 - mid_x, (t + b) / 2 - mid_y), x, y});
        }

    std::sort(ahead.begin(), ahead.end(), [](const Ahead& a, const Ahead& b) { return a.covers != b.covers ? a.covers > b.covers : a.distance < b.distance; });
    if (ahead.size() > limit)
        ahead.resize(limit);
    for (const Ahead& a : ahead)
    {
        const int wrapped = ((a.x % count) + count) % count;
        const uint64_t key = key_of(which, level, wrapped, a.y);
        Tile& t = tiles_[key];
        t.layer = which;
        t.z = level;
        t.x = wrapped;
        t.y = a.y;
        t.used = clock_;
        ahead_.push_back({a.covers, a.distance, key});
    }
}

bool MapView::decode(Tile& t, const std::string& bytes)
{
    auto d = rap::sys::decode_image(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    if (!d)
        return false;
    if (d->width <= 0 || d->height <= 0 || d->width > 1024 || d->height > 1024 || d->pixels.size() < size_t(d->width) * size_t(d->height) * 4)
        return false;
    t.width = d->width;
    t.height = d->height;
    t.format = d->format;
    t.pixels = std::move(d->pixels);
    t.generation = ++tiles_version_;
    return true;
}

void MapView::store(const Tile& t, const std::string& bytes)
{
    const std::string path = file_of(t);
    gchar* parent = g_path_get_dirname(path.c_str());
    const bool made = g_mkdir_with_parents(parent, 0700) == 0;
    g_free(parent);
    if (!made)
        return;
    // Written beside it under a name of its own (another window may be
    // fetching the same tile), then put in place whole.
    char suffix[32];
    std::snprintf(suffix, sizeof suffix, ".%08x.partial", unsigned(g_random_int()));
    const fs::path target = rap::sys::path_from_utf8(path), partial = rap::sys::path_from_utf8(path + suffix);
    bool written = false;
    {
        std::ofstream out(partial, std::ios::binary);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.close();
        written = bool(out);
    }
    std::error_code ec;
    if (written)
        fs::rename(partial, target, ec);
    if (!written || ec)
        fs::remove(partial, ec);

    // Now and then, what's on disk is brought back within its budget.
    if (stores_++ % 64 == 0 && !trimming_)
    {
        if (trim_.joinable())
            trim_.join();
        trimming_ = true;
        trim_ = std::thread(
            [this, dir = tiles_dir()]
            {
                trim_disk(dir);
                trimming_ = false;
            });
    }
}

// A tile the server hasn't got: the ones above it are wanted instead, as far
// up as draw_tile will look, for it to stand in.
void MapView::want_parents(int layer, int z, int x, int y, std::vector<uint64_t>& out)
{
    for (int up = 0; up < 5 && z > kMinZoom; ++up)
    {
        const auto it = tiles_.find(key_of(layer, z, x, y));
        if (it == tiles_.end() || !it->second.failed)
            return;
        --z;
        x >>= 1;
        y >>= 1;
        const uint64_t key = key_of(layer, z, x, y);
        Tile& t = tiles_[key];
        t.layer = layer;
        t.z = z;
        t.x = x;
        t.y = y;
        t.used = clock_;
        out.push_back(key);
    }
}

void MapView::pump()
{
    // Each only once, at its best place.
    std::unordered_set<uint64_t> seen;
    wanted_.erase(std::remove_if(wanted_.begin(), wanted_.end(), [&](uint64_t key) { return !seen.insert(key).second; }), wanted_.end());

    // What's on disk first, one a paint: reading and decoding happen here, on
    // the UI thread. Nothing goes to the network until the disk's been asked.
    bool more = false;
    for (const uint64_t key : wanted_)
    {
        Tile& t = tiles_[key];
        if (!t.pixels.empty() || t.failed || t.tried_disk)
            continue;
        t.tried_disk = true;
        more = true;  // and on to the next, next paint
        const fs::path path = rap::sys::path_from_utf8(file_of(t));
        std::error_code ec;
        const auto written = fs::last_write_time(path, ec);
        if (!ec && fs::file_time_type::clock::now() - written < kTileLife)
        {
            std::ifstream in(path, std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            decode(t, bytes);
        }
        break;
    }

    // The slots are for the most wanted of what's still missing: anything
    // else being fetched gives its slot up, scrolled away or simply outranked.
    std::vector<uint64_t> top;
    for (const uint64_t key : wanted_)
    {
        const Tile& t = tiles_[key];
        if (!t.pixels.empty() || t.failed || !t.tried_disk)
            continue;
        top.push_back(key);
        if (top.size() == size_t(kMaxFetches))
            break;
    }
    for (auto& [key, t] : tiles_)
        if (t.fetch && std::find(top.begin(), top.end(), key) == top.end())
        {
            t.fetch.reset();
            --fetching_;
        }

    for (const uint64_t key : top)
    {
        Tile& t = tiles_[key];
        if (t.fetch)
            continue;
#ifdef HAVE_GEOCODE
        std::string url = url_of(t.layer);
        replace_all(url, "{z}", std::to_string(t.z));
        replace_all(url, "{x}", std::to_string(t.x));
        replace_all(url, "{y}", std::to_string(t.y));
        static const HttpHeaders headers = {{"User-Agent", "IfritView/" IFRIT_VERSION " (image viewer; map of a photo's location)"}};
        ++fetching_;
        t.asked = std::chrono::steady_clock::now();
        t.fetch = http_get(url, headers, [this, key](const HttpResponse& r) { got(key, r); });
#else
        t.failed = true;
#endif
    }
    evict();
    if (more)
        refresh();
}

void MapView::got(uint64_t key, const HttpResponse& response)
{
    --fetching_;
    const auto it = tiles_.find(key);
    if (it == tiles_.end())
        return;
    Tile& t = it->second;
    t.fetch.reset();
    if (response.status == 200 && decode(t, response.body))
    {
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t.asked).count();
        fetch_seconds_ += 0.2 * (std::min(took, 5.0) - fetch_seconds_);
        store(t, response.body);
    }
    else
    {
        t.failed = true;
        platform::trace("map tile %d/%d/%d: status %d %s", t.z, t.x, t.y, response.status, response.error.c_str());
    }
    refresh();
}

void MapView::evict()
{
    while (tiles_.size() > kMaxTiles)
    {
        auto oldest = tiles_.end();
        for (auto it = tiles_.begin(); it != tiles_.end(); ++it)
            if (!it->second.fetch && it->second.used != clock_ && (oldest == tiles_.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == tiles_.end())
            return;
        tiles_.erase(oldest);
    }
}

bool MapView::over_selected(rap::point_f q) const
{
    const float dx = q.x - selected_.x, dy = q.y - selected_.y;
    return selected_.radius > 0 && dx * dx + dy * dy <= selected_.radius * selected_.radius;
}

// The topmost pin there, if any.
const MapView::Drawn* MapView::pin_at(rap::point_f q) const
{
    for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it)
    {
        const float dx = q.x - it->x, dy = q.y - it->y;
        if (dx * dx + dy * dy <= it->radius * it->radius)
            return &*it;
    }
    return nullptr;
}

bool MapView::pointer(const rap::pointer_event& e)
{
    using T = rap::pointer_event_type;
    if (e.type == T::press && e.button == 1)
    {
        if (e.pos.x >= chip_.x && e.pos.x < chip_.x + chip_.w && e.pos.y >= chip_.y && e.pos.y < chip_.y + chip_.h)
        {
            plain_click_ = false;
            cfg_.map_satellite = layer() == 0;
            cfg_.save();
            refresh();
            return true;
        }
        // The full map is for a double-click on the map itself: two clicks
        // both on bare map, neither of them a drag. A second click on a pin
        // is just that pin clicked again.
        if (e.clicks == 2 && plain_click_ && !pin_at(e.pos) && !over_selected(e.pos))
        {
            plain_click_ = false;
            if (on_open && has_)
                on_open(lat_, lon_);
            return true;
        }
        dragging_ = true;
        moved_ = false;
        pressed_ = last_ = e.pos;
        return true;
    }
    if (e.type == T::release && e.button == 1)
    {
        const bool click = dragging_ && !moved_;
        dragging_ = false;
        const Drawn* pin = click ? pin_at(e.pos) : nullptr;
        plain_click_ = click && !pin && !over_selected(e.pos);
        if (pin)
            pick(pin->cluster);
        return true;
    }
    if (e.type == T::move && dragging_ && world_ > 0)
    {
        if (std::abs(e.pos.x - pressed_.x) > 3 || std::abs(e.pos.y - pressed_.y) > 3)
            moved_ = true;
        // How fast the view's going (the opposite way to the hand), smoothed.
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::max(0.001, std::chrono::duration<double>(now - last_move_).count());
        last_move_ = now;
        if (seconds < 0.3)
        {
            view_vx_ += 0.3 * (-(e.pos.x - last_.x) / seconds - view_vx_);
            view_vy_ += 0.3 * (-(e.pos.y - last_.y) / seconds - view_vy_);
        }
        else
            view_vx_ = view_vy_ = 0;
        centre_x_ -= (e.pos.x - last_.x) / world_;
        centre_y_ = std::clamp(centre_y_ - (e.pos.y - last_.y) / world_, 0.0, 1.0);
        centre_x_ -= std::floor(centre_x_);
        last_ = e.pos;
        refresh();
        return true;
    }
    return false;
}

bool MapView::scroll(const rap::scroll_event& e)
{
    if (!has_)
        return false;
    const double notches = -(e.precise ? e.dy / 40 : e.dy);
    const double zoom = std::clamp(zoom_ + notches * kZoomStep, double(kMinZoom), double(kMaxZoom));
    if (zoom == zoom_)
        return true;
    // What's under the pointer stays there.
    const double before = kTile * std::pow(2.0, zoom_), after = kTile * std::pow(2.0, zoom);
    const double dx = e.pos.x - size_.w / 2, dy = e.pos.y - size_.h / 2;
    centre_x_ += dx / before - dx / after;
    centre_y_ = std::clamp(centre_y_ + dy / before - dy / after, 0.0, 1.0);
    centre_x_ -= std::floor(centre_x_);
    zoom_ = zoom;
    refresh();
    return true;
}
