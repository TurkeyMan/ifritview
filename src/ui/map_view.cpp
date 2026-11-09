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
#include <span>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

constexpr int kTile = 256, kMinZoom = 2, kMaxZoom = 19;
constexpr double kZoomStep = 0.25;  // of a level (a doubling) per wheel notch
constexpr int kPinCell = 34;      // pins in one square this wide are drawn as one
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
    pins_.clear();
    pins_.reserve(pins.size());
    for (const Pin& p : pins)
    {
        // -180 and +180 are one place.
        const double x = world_x(p.longitude);
        pins_.push_back({p.path, x - std::floor(x), world_y(p.latitude)});
    }
    clusters_zoom_ = -1;
    refresh();
}

int64_t MapView::cell_of(const Placed& p) const
{
    const double scale = double(int64_t(1) << tile_zoom()) * kTile / kPinCell;
    return int64_t(p.x * scale) << 32 | int64_t(p.y * scale);
}

void MapView::build_clusters()
{
    clusters_zoom_ = tile_zoom();
    clusters_.clear();
    drawn_.clear();
    std::map<int64_t, size_t> at;
    for (size_t i = 0; i < pins_.size(); ++i)
    {
        const Placed& p = pins_[i];
        if (p.path == path_)
            continue;
        const int64_t cell = cell_of(p);
        const auto [it, added] = at.try_emplace(cell, clusters_.size());
        if (added)
        {
            Cluster c;
            c.first = i;
            c.cell = cell;
            clusters_.push_back(c);
        }
        Cluster& c = clusters_[it->second];
        c.x += (p.x - c.x) / double(++c.count);
        c.y += (p.y - c.y) / double(c.count);
    }
}

void MapView::pick(size_t cluster)
{
    const Cluster c = clusters_[cluster];
    if (c.count == 1 && on_pick)
    {
        keep_view_ = true;
        on_pick(pins_[c.first].path);
        return;
    }
    if (zoom_ < kMaxZoom)
    {
        // Closer, till they come apart.
        centre_x_ = c.x;
        centre_y_ = c.y;
        zoom_ = std::min(zoom_ + 2, double(kMaxZoom));
        refresh();
        return;
    }
    // As close as it goes: all in one spot, so each click the next after the
    // photo shown, in the folder's order, going through every one.
    std::vector<size_t> members;
    size_t after = 0;
    for (size_t i = 0; i < pins_.size(); ++i)
        if (cell_of(pins_[i]) == c.cell)
        {
            members.push_back(i);
            if (pins_[i].path == path_)
                after = members.size();
        }
    if (members.size() > 1 && on_pick)
    {
        keep_view_ = true;
        on_pick(pins_[members[after % members.size()]].path);
    }
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

    // The folder's other photos, under the red pin.
    if (clusters_zoom_ != tile_zoom())
        build_clusters();
    drawn_.clear();
    {
        const rap::color edge{255, 255, 255}, gray{112, 118, 130};
        rap::text_style count_style;
        count_style.fg = rap::color{255, 255, 255};
        count_style.size = 7;
        count_style.bold = true;
        for (size_t i = 0; i < clusters_.size(); ++i)
        {
            const Cluster& c = clusters_[i];
            double dx = c.x - centre_x_;
            dx -= std::round(dx);  // the nearer way round the world
            const float x = float(dx * world_) + size.w / 2, y = float((c.y - centre_y_) * world_) + size.h / 2;
            if (x < -30 || x > size.w + 30 || y < -8 || y > size.h + 44)
                continue;
            const float r = c.count > 1 ? 10.f : 7.f, head = y - r - 7;
            p.polygon({{x - r + 1.f, head + 2}, {x + r - 1.f, head + 2}, {x, y + 1.5f}}, edge, std::nullopt);
            p.circle({x, head}, r + 1.25f, edge, std::nullopt);
            p.polygon({{x - r + 2.5f, head + 2}, {x + r - 2.5f, head + 2}, {x, y - 1}}, gray, std::nullopt);
            p.circle({x, head}, r, gray, std::nullopt);
            if (c.count > 1)
            {
                const std::string n = c.count > 99 ? "99+" : std::to_string(c.count);
                const rap::size_f t = p.measure(n, count_style);
                p.text({x - t.w / 2, head - t.h / 2}, n, count_style);
            }
            drawn_.push_back({x, head, r + 3, i});
        }
    }

    // The pin: its point on the position.
    double pin_dx = pin_x_ - centre_x_;
    pin_dx -= std::round(pin_dx);  // the nearer way round the world
    const float px = float(pin_dx * world_) + size.w / 2, py = float((pin_y_ - centre_y_) * world_) + size.h / 2;
    const rap::color white{255, 255, 255}, red{220, 40, 40};
    p.polygon({{px - 9.5f, py - 15}, {px + 9.5f, py - 15}, {px, py + 1.5f}}, white, std::nullopt);
    p.circle({px, py - 20}, 11, white, std::nullopt);
    p.polygon({{px - 8, py - 15}, {px + 8, py - 15}, {px, py - 1}}, red, std::nullopt);
    p.circle({px, py - 20}, 9.5f, red, std::nullopt);
    p.circle({px, py - 20}, 3.5f, white, std::nullopt);

    // Street / satellite, named for what a click switches to, if both are on.
    chip_ = {};
    if (!cfg_.map_tile_url.empty() && !cfg_.map_satellite_url.empty())
    {
        const std::string other = layer() ? _("Street") : _("Satellite");
        rap::text_style chip_style;
        chip_style.fg = white;
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
