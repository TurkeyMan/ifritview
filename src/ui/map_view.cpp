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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

constexpr int kTile = 256, kMinZoom = 2, kMaxZoom = 19;
constexpr double kNeighbourhood = 14;  // the farthest out a photo opened from a pin is shown
constexpr double kZoomStep = 0.25;  // of a level (a doubling) per wheel notch
constexpr int kPinCell = 56;      // pins in one square this wide are drawn as one
constexpr size_t kMaxThumbs = 512;   // kept
constexpr size_t kThumbsAsked = 64;  // being made at a time, nearest first
constexpr size_t kThumbsNear = 256;  // the nearest, safe from being dropped: fewer than are kept
constexpr int kMaxFetches = 2;     // at once: the tile servers ask for few
constexpr size_t kMaxTiles = 128;  // in memory
constexpr auto kTileLife = std::chrono::hours(24 * 30);
constexpr uintmax_t kDiskBudget = uintmax_t(256) << 20;

const MapView::Provider kStreet[] = {
    {"OpenStreetMap", "https://tile.openstreetmap.org/{z}/{x}/{y}.png", "© OpenStreetMap contributors"},
};
const MapView::Provider kSatellite[] = {
    {"Powered by Esri", "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
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
const char* MapView::attribution() const
{
    size_t count = 0;
    const Provider* list = providers(layer() == 1, count);
    for (size_t i = 0; i < count; ++i)
        if (url_of(layer()) == list[i].url)
            return list[i].attribution;
    return "";
}

void MapView::config_changed()
{
    if (urls_[0] != cfg_.map_tile_url || urls_[1] != cfg_.map_satellite_url)
    {
        tiles_.clear();  // which drops their fetches
        fetching_ = 0;
        wanted_.clear();
        urls_[0] = cfg_.map_tile_url;
        urls_[1] = cfg_.map_satellite_url;
    }
    refresh();
}

void MapView::stop_fetching()
{
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
    if (canvas_)
        canvas_->invalidate();
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
    if (canvas_)
    {
        const rap::size_f size = canvas_->size();
        const double extent = std::max(x1 - x0, y1 - y0);
        if (extent > 0 && size.w > 0 && size.h > 0)
            target = std::min(target, std::log2(0.7 * std::min(size.w, size.h) / (extent * kTile)));
    }
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

// A tile, or while it isn't here the part of the nearest coarser one that
// covers it, so zooming shows something at once.
void MapView::draw_tile(rap::painter& p, int z, int x, int y, rap::rect_f to)
{
    const int count = 1 << z;
    const int wrapped = ((x % count) + count) % count;
    for (int up = 0; up <= std::min(z, 5); ++up)
    {
        const auto it = tiles_.find(key_of(layer(), z - up, wrapped >> up, y >> up));
        if (it == tiles_.end() || !it->second.image)
            continue;
        const float side = float(kTile) / float(1 << up);
        const float sx = float(wrapped & ((1 << up) - 1)) * side, sy = float(y & ((1 << up) - 1)) * side;
        it->second.used = clock_;
        p.image(*it->second.image, {sx, sy, side, side}, to);
        return;
    }
}

void MapView::paint(rap::painter& p, rap::size_f size)
{
    ++clock_;
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
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            // On whole pixels, so neighbours meet.
            const float tx = float(std::round(x * tile - left)), ty = float(std::round(y * tile - top));
            const float tw = float(std::round((x + 1) * tile - left)) - tx, th = float(std::round((y + 1) * tile - top)) - ty;
            const int wrapped = ((x % count) + count) % count;
            const uint64_t key = key_of(layer(), level, wrapped, y);
            Tile& t = tiles_[key];
            t.layer = layer();
            t.z = level;
            t.x = wrapped;
            t.y = y;
            t.used = clock_;
            draw_tile(p, level, x, y, {tx, ty, tw, th});
            const float dx = tx + tw / 2 - size.w / 2, dy = ty + th / 2 - size.h / 2;
            need.push_back({key, dx * dx + dy * dy});
        }
    std::sort(need.begin(), need.end(), [](const Need& a, const Need& b) { return a.distance < b.distance; });
    wanted_.clear();
    for (const Need& n : need)
        wanted_.push_back(n.key);

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
        draw_pin(p, float(pin_dx * world_) + size.w / 2, float((pin_y_ - centre_y_) * world_) + size.h / 2, current_, 1, true, nullptr);

        std::sort(near_pins.begin(), near_pins.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
        std::vector<const Placed*> wanted;
        if (current_)
            wanted.push_back(current_);
        for (const Near& n : near_pins)
            wanted.push_back(n.placed);
        ask_thumbs(wanted);
    }

    // Street / satellite, named for what a click switches to, if both are on.
    chip_ = {};
    if (!cfg_.map_tile_url.empty() && !cfg_.map_satellite_url.empty())
    {
        const std::string other = layer() ? _("Street") : _("Satellite");
        rap::text_style chip_style;
        chip_style.fg = rap::color{255, 255, 255};
        const rap::size_f t = p.measure(other, chip_style);
        chip_ = {size.w - t.w - 24, 8, t.w + 16, t.h + 6};
        p.rect(chip_, 4, rap::color{0, 0, 0, 170}, std::nullopt);
        p.text({chip_.x + 8, chip_.y + 3}, other, chip_style);
    }

    // Who drew it, as their terms ask.
    if (const std::string by = attribution(); !by.empty())
    {
        rap::text_style credit;
        credit.fg = rap::color{20, 20, 20};
        credit.size = 7.5f;
        const rap::size_f c = p.measure(by, credit);
        p.rect({size.w - c.w - 6, size.h - c.h - 2, c.w + 6, c.h + 2}, 0, rap::color{255, 255, 255, 190}, std::nullopt);
        p.text({size.w - c.w - 3, size.h - c.h - 1}, by, credit);
    }

    pump();
}

bool MapView::decode(Tile& t, const std::string& bytes)
{
    const auto d = rap::sys::decode_image(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    if (!d)
        return false;
    t.image = ui_.create_image(d->width, d->height, d->format, d->pixels.data(), size_t(d->width) * 4);
    return t.image != nullptr;
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

void MapView::pump()
{
    // Those scrolled away, or left behind by the next photo, stop.
    for (auto& [key, t] : tiles_)
        if (t.fetch && std::find(wanted_.begin(), wanted_.end(), key) == wanted_.end())
        {
            t.fetch.reset();
            --fetching_;
        }

    bool more = false;
    bool read_one = false;
    for (const uint64_t key : wanted_)
    {
        Tile& t = tiles_[key];
        if (t.image || t.failed || t.fetch)
            continue;
        if (!t.tried_disk)
        {
            // One a paint: reading and decoding happen here, on the UI thread.
            if (read_one)
            {
                more = true;
                continue;
            }
            read_one = true;
            t.tried_disk = true;
            const fs::path path = rap::sys::path_from_utf8(file_of(t));
            std::error_code ec;
            const auto written = fs::last_write_time(path, ec);
            if (!ec && fs::file_time_type::clock::now() - written < kTileLife)
            {
                std::ifstream in(path, std::ios::binary);
                const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                if (decode(t, bytes))
                {
                    more = true;
                    continue;
                }
            }
        }
#ifdef HAVE_GEOCODE
        if (fetching_ >= kMaxFetches)
            continue;
        std::string url = url_of(t.layer);
        replace_all(url, "{z}", std::to_string(t.z));
        replace_all(url, "{x}", std::to_string(t.x));
        replace_all(url, "{y}", std::to_string(t.y));
        static const HttpHeaders headers = {{"User-Agent", "IfritView/" IFRIT_VERSION " (image viewer; map of a photo's location)"}};
        ++fetching_;
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
        store(t, response.body);
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

bool MapView::pointer(const rap::pointer_event& e)
{
    using T = rap::pointer_event_type;
    if (e.type == T::press && e.button == 1)
    {
        if (e.pos.x >= chip_.x && e.pos.x < chip_.x + chip_.w && e.pos.y >= chip_.y && e.pos.y < chip_.y + chip_.h)
        {
            cfg_.map_satellite = layer() == 0;
            cfg_.save();
            refresh();
            return true;
        }
        if (e.clicks == 2)
        {
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
        if (click)
            for (auto it = drawn_.rbegin(); it != drawn_.rend(); ++it)
            {
                const float dx = e.pos.x - it->x, dy = e.pos.y - it->y;
                if (dx * dx + dy * dy <= it->radius * it->radius)
                {
                    pick(it->cluster);
                    break;
                }
            }
        return true;
    }
    if (e.type == T::move && dragging_ && world_ > 0)
    {
        if (std::abs(e.pos.x - pressed_.x) > 3 || std::abs(e.pos.y - pressed_.y) > 3)
            moved_ = true;
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
    if (!has_ || !canvas_)
        return false;
    const double notches = -(e.precise ? e.dy / 40 : e.dy);
    const double zoom = std::clamp(zoom_ + notches * kZoomStep, double(kMinZoom), double(kMaxZoom));
    if (zoom == zoom_)
        return true;
    // The pin stays where it is on the canvas, the map growing around it.
    const double scale = std::pow(2.0, zoom_ - zoom);
    double dx = pin_x_ - centre_x_;
    dx -= std::round(dx);  // the short way, the world being a loop
    centre_x_ = pin_x_ - dx * scale;
    centre_y_ = std::clamp(pin_y_ - (pin_y_ - centre_y_) * scale, 0.0, 1.0);
    centre_x_ -= std::floor(centre_x_);
    zoom_ = zoom;
    refresh();
    return true;
}
