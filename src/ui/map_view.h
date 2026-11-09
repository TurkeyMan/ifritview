#pragma once
// The map in the info pane: slippy-map tiles drawn on a canvas, the shown
// photo's position pinned on them. Drag to pan, wheel to zoom, double-click
// for the full map; a chip in the corner switches between street and
// satellite. Tiles are kept in memory and in the system's temporary folder.

#include <rapport/rapport.h>

#include <atomic>
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

    void attach(rap::canvas* canvas) { canvas_ = canvas; }
    bool attached() const { return canvas_ != nullptr; }
    bool enabled() const;
    void config_changed();  // the settings were applied: other tiles, maybe
    void stop_fetching();   // it's been hidden: what's on the way isn't wanted

    // Where the red pin goes, and the file it's for: a new position is
    // centred, the zoom kept, unless a click on a gray pin brought it here.
    void set_location(double latitude, double longitude, const std::string& path);

    // The gray pins: the folder's other geotagged files.
    struct Pin
    {
        std::string path;
        double latitude = 0, longitude = 0;
    };
    void set_pins(const std::vector<Pin>& pins);

    void paint(rap::painter& p, rap::size_f size);
    bool pointer(const rap::pointer_event& e);
    bool scroll(const rap::scroll_event& e);

    std::function<void(double latitude, double longitude)> on_open;  // double-clicked
    std::function<void(const std::string& path)> on_pick;            // a gray pin clicked

private:
    struct Tile
    {
        std::unique_ptr<rap::image> image;
        std::shared_ptr<void> fetch;  // while it's being fetched
        bool failed = false, tried_disk = false;
        uint64_t used = 0;
        int layer = 0, z = 0, x = 0, y = 0;
    };

    int layer() const;
    int tile_zoom() const;  // the level whose tiles are drawn
    const std::string& url_of(int layer) const;
    const char* attribution() const;
    std::string file_of(const Tile& t) const;
    void draw_tile(rap::painter& p, int z, int x, int y, rap::rect_f to);
    void pump();
    void got(uint64_t key, const HttpResponse& response);
    bool decode(Tile& t, const std::string& bytes);
    void store(const Tile& t, const std::string& bytes);
    void evict();
    void refresh();
    void build_clusters();
    void pick(size_t cluster);

    // Pins close enough at this zoom to be one, with how many there are.
    struct Placed
    {
        std::string path;
        double x = 0, y = 0;  // on the world, 0..1
    };
    struct Cluster
    {
        double x = 0, y = 0;
        int count = 0;
        size_t first = 0;  // in pins_
        int64_t cell = 0;
    };
    struct Drawn
    {
        float x = 0, y = 0, radius = 0;  // the pin's head, on the canvas
        size_t cluster = 0;
    };
    int64_t cell_of(const Placed& p) const;

    rap::ui& ui_;
    Config& cfg_;
    rap::canvas* canvas_ = nullptr;

    bool has_ = false;
    double lat_ = 0, lon_ = 0;
    std::string path_;
    bool keep_view_ = false;
    std::vector<Placed> pins_;
    std::vector<Cluster> clusters_;
    std::vector<Drawn> drawn_;
    int clusters_zoom_ = -1;
    bool moved_ = false;
    rap::point_f pressed_{};
    double pin_x_ = 0, pin_y_ = 0;        // on the world, 0..1
    double centre_x_ = 0, centre_y_ = 0;  // likewise
    double zoom_ = 15;  // between tile levels too: their tiles scaled to fit

    bool dragging_ = false;
    rap::point_f last_{};
    rap::rect_f chip_{};
    double world_ = 0;  // the world's width on the canvas at this zoom

    std::unordered_map<uint64_t, Tile> tiles_;
    std::vector<uint64_t> wanted_;  // what the last paint needed, nearest the middle first
    uint64_t clock_ = 0;
    int fetching_ = 0;
    std::string urls_[2];  // what tiles_ was fetched from
    int stores_ = 0;
    std::atomic<bool> trimming_{false};
    std::thread trim_;  // the disk's old tiles going, now and then
};
