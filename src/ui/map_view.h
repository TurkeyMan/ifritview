#pragma once
// The map in the info pane: slippy-map tiles drawn on a canvas, the shown
// photo's position pinned on them. Drag to pan, wheel to zoom, double-click
// for the full map; a chip in the corner switches between street and
// satellite. Tiles are kept in memory and in the system's temporary folder.

#include "pin_thumbs.h"

#include <rapport/rapport.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct Config;
struct HttpResponse;

class MapView
{
public:
    // Presets offered in settings, and the credit each one's terms ask for.
    struct Provider
    {
        const char* name;
        const char* url;
        const char* attribution;
    };
    static const Provider* providers(bool satellite, size_t& count);

    // `cfg` outlives this, and is read for the tile URLs and the layer.
    MapView(rap::ui& ui, Config& cfg);
    ~MapView();
    MapView(const MapView&) = delete;
    MapView& operator=(const MapView&) = delete;

    // What to ask to be painted again: the pane's canvas, or the fullscreen view.
    void attach(std::function<void()> repaint) { repaint_ = std::move(repaint); }
    bool enabled() const;
    void config_changed();  // the settings were applied: other tiles, maybe
    void stop_fetching();   // it's been hidden: what's on the way isn't wanted
    void cancel_gesture();  // what it's drawn on has changed: a drag there is over

    // Where the red pin goes, and the file it's for: a new position is
    // centred, the zoom kept, unless a click on a gray pin brought it here.
    void set_location(double latitude, double longitude, const std::string& path);

    // The gray pins: the folder's other geotagged files.
    struct Pin
    {
        std::string path;
        double latitude = 0, longitude = 0;
        int orientation = 1;
        int64_t modified = 0;  // the thumbnail's made anew when it changes
    };
    void set_pins(const std::vector<Pin>& pins);
    // The gray pins and their pictures dropped, here and in the cache folder;
    // `done` (on another thread, maybe) told whether those files went.
    void forget_pins(std::function<void(bool deleted)> done);

    void paint(rap::painter& p, rap::size_f size);
    bool pointer(const rap::pointer_event& e);
    bool scroll(const rap::scroll_event& e);

    // The same, in part of a bigger canvas: `area`, framed.
    void paint_in(rap::painter& p, rap::rect_f area);
    bool pointer_in(const rap::pointer_event& e, rap::rect_f area);
    bool scroll_in(const rap::scroll_event& e, rap::rect_f area);

    std::function<void(double latitude, double longitude)> on_open;  // double-clicked
    std::function<void(const std::string& path)> on_pick;            // a gray pin clicked
    std::function<void(const std::string& text)> on_hover;           // what the pointer's over, for a tooltip; "" for none

private:
    struct Tile
    {
        std::vector<uint8_t> pixels;  // width * 4 bytes a row
        int width = 0, height = 0;
        rap::pixel_format format{};
        uint64_t generation = 0;  // different each time pixels arrive
        std::chrono::steady_clock::time_point asked{};  // when its fetch began
        std::shared_ptr<void> fetch;  // while it's being fetched
        bool failed = false, tried_disk = false;
        uint64_t used = 0;
        int layer = 0, z = 0, x = 0, y = 0;
    };

    int layer() const;
    int tile_zoom() const;  // the level whose tiles are drawn
    const std::string& url_of(int layer) const;
    const char* attribution(int layer) const;
    std::string file_of(const Tile& t) const;
    // The tiles under a view put together at their own size, in one image:
    // scaled as one, there are no joins to show.
    struct Mosaic
    {
        std::unique_ptr<rap::image> image;
        std::vector<uint8_t> pixels;
        rap::pixel_format format{};
        int width = 0, height = 0;
        int layer = -1, level = -1, x0 = 0, x1 = -1, y0 = 0, y1 = -1;
        std::vector<uint64_t> sources;  // a cell's source_of, row by row
        uint64_t version = 0;
    };
    void draw_mosaic(rap::painter& p, Mosaic& m, int layer, int level, int x0, int x1, int y0, int y1, double left, double top, double tile);
    uint64_t source_of(int layer, int z, int x, int y) const;
    void blit_tile(Mosaic& m, int ox, int oy, int layer, int z, int x, int y);
    void want_parents(int layer, int z, int x, int y, std::vector<uint64_t>& out);
    void want_prime(int which, int level, double left, double top, rap::size_f size);
    void want_ahead(int which, size_t limit, int level, int x0, int x1, int y0, int y1, double left, double top, double tile, rap::size_f size);
    void pump();
    void got(uint64_t key, const HttpResponse& response);
    bool decode(Tile& t, const std::string& bytes);
    void store(const Tile& t, const std::string& bytes);
    void evict();
    void refresh();
    void build_clusters();
    struct Drawn;
    const Drawn* pin_at(rap::point_f q) const;
    bool over_selected(rap::point_f q) const;
    std::string hover_text(rap::point_f q) const;
    void set_hover(const std::string& text, bool always = false);
    std::string hovering_;  // what on_hover was last told
    rap::point_f pointer_at_{};  // where the pointer last was over the map
    bool pointer_here_ = false;  // and that it's still there
    void pick(size_t cluster);

    struct Drawn
    {
        float x = 0, y = 0, radius = 0;  // the pin's head, on the canvas
        size_t cluster = 0;
    };
    // Pins close enough at this zoom to be one, with how many there are.
    struct Placed
    {
        std::string path;
        double x = 0, y = 0;  // on the world, 0..1
        std::string thumb;    // its key
        int orientation = 1;
    };
    struct Thumb
    {
        std::unique_ptr<rap::image> image;
        bool failed = false;
        uint64_t used = 0;
    };
    void draw_pin(rap::painter& p, float x, float y, const Placed* placed, int count, bool selected, Drawn* hit);
    void ask_thumbs(const std::vector<const Placed*>& wanted);
    void trim_thumbs();
    void thumb_ready(const std::string& key, std::shared_ptr<ThumbLoader::Picture> picture, uint64_t generation);
    struct Cluster
    {
        double x = 0, y = 0;
        int count = 0;
        size_t first = 0;  // in pins_
        std::vector<size_t> members;
    };
    std::vector<Cluster> group(const std::vector<size_t>& pins, int level) const;

    rap::ui& ui_;
    Config& cfg_;
    std::function<void()> repaint_;
    rap::size_f size_{};  // what it was last painted at

    bool has_ = false;
    double lat_ = 0, lon_ = 0;
    std::string path_;
    bool keep_view_ = false;
    std::vector<Placed> pins_;
    std::vector<Cluster> clusters_;
    std::vector<Drawn> drawn_;
    Drawn selected_;  // the red pin, where it was last drawn
    int clusters_zoom_ = -1;
    const Placed* current_ = nullptr;  // the red pin's, once the folder's been read
    std::unordered_map<std::string, Thumb> thumbs_;
    std::vector<std::string> asked_;
    std::shared_ptr<char> alive_ = std::make_shared<char>();
    std::unique_ptr<ThumbLoader> loader_;  // made when first wanted
    uint64_t thumbs_generation_ = 0;       // the loader's, since it last forgot
    enum class Gesture
    {
        none,
        map,
        view
    };
    Gesture gesture_ = Gesture::none;  // in a bigger canvas: whose press this is
    int gesture_button_ = 0;
    bool moved_ = false;
    std::chrono::steady_clock::time_point last_move_{};
    double view_vx_ = 0, view_vy_ = 0;  // canvas px a second, while being dragged
    bool plain_click_ = false;  // the last click was on bare map, not a pin or a drag
    rap::point_f pressed_{};
    double pin_x_ = 0, pin_y_ = 0;        // on the world, 0..1
    double centre_x_ = 0, centre_y_ = 0;  // likewise
    double zoom_ = 15;  // between tile levels too: their tiles scaled to fit

    bool dragging_ = false;
    rap::point_f last_{};
    rap::rect_f chip_{};
    double world_ = 0;  // the world's width on the canvas at this zoom

    std::unordered_map<uint64_t, Tile> tiles_;
    struct Prefetch
    {
        double covers, distance;  // the share of its view it'd fill; how far off it is
        uint64_t key;
    };
    std::vector<Prefetch> ahead_;  // this paint's, from the map and the button
    std::vector<uint64_t> wanted_;  // what the last paint needed, nearest the middle first
    uint64_t clock_ = 0;
    int fetching_ = 0;
    double fetch_seconds_ = 0.25;  // how long a fetch usually takes here, learned as they land
    Mosaic map_mosaic_, preview_mosaic_;
    uint64_t tiles_version_ = 1;  // goes up when a tile's pixels arrive or go
    std::string urls_[2];  // what tiles_ was fetched from
    int stores_ = 0;
    std::atomic<bool> trimming_{false};
    std::thread trim_;  // the disk's old tiles going, now and then
};
