#include "geocode.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#ifdef HAVE_GEOCODE
#include "http.h"

#include <json-glib/json-glib.h>
#endif

namespace
{

const Geocoder::Provider kProviders[] = {
    {"OpenStreetMap (Nominatim)", "https://nominatim.openstreetmap.org/reverse?lat={lat}&lon={lon}&format=jsonv2&zoom=14"},
    {"Photon (komoot)", "https://photon.komoot.io/reverse?lat={lat}&lon={lon}"},
    {"BigDataCloud", "https://api.bigdatacloud.net/data/reverse-geocode-client?latitude={lat}&longitude={lon}&localityLanguage=en"},
};

std::string key_for(double lat, double lon)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f,%.3f", lat, lon);
    return buf;
}

std::string cache_path()
{
    gchar* p = g_build_filename(g_get_user_cache_dir(), "ifritview", "places.tsv", nullptr);
    std::string s = p;
    g_free(p);
    return s;
}

}  // namespace

struct Geocoder::Impl
{
    std::string url_template;
    std::map<std::string, std::string> known;              // key -> place ("" = looked up, nothing found)
    std::map<std::string, std::vector<Callback>> waiting;  // key -> callbacks
    std::deque<std::pair<std::string, std::pair<double, double>>> queue;
    std::string language;
    bool in_flight = false;
    guint timer = 0;
#ifdef HAVE_GEOCODE
    std::shared_ptr<void> request;  // the lookup in flight; dropping it cancels
#endif

    void load_cache()
    {
        gchar* text = nullptr;
        gsize len = 0;
        if (!g_file_get_contents(cache_path().c_str(), &text, &len, nullptr))
            return;
        gchar** lines = g_strsplit(text, "\n", -1);
        for (gchar** l = lines; *l; ++l)
        {
            const char* tab = std::strchr(*l, '\t');
            if (!tab)
                continue;
            known[std::string(*l, size_t(tab - *l))] = tab + 1;
        }
        g_strfreev(lines);
        g_free(text);
    }

    void remember(const std::string& key, const std::string& place)
    {
        known[key] = place;
        const std::string path = cache_path();
        gchar* dir = g_path_get_dirname(path.c_str());
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
        if (FILE* f = g_fopen(path.c_str(), "a"))
        {
            std::fprintf(f, "%s\t%s\n", key.c_str(), place.c_str());
            std::fclose(f);
        }
    }

    void resolve(const std::string& key, const std::string& place)
    {
        remember(key, place);
        auto it = waiting.find(key);
        if (it == waiting.end())
            return;
        std::vector<Callback> cbs = std::move(it->second);
        waiting.erase(it);
        for (Callback& cb : cbs)
            cb(place);
    }

    void pump()
    {
        if (in_flight || queue.empty() || timer)
            return;
        auto [key, coords] = queue.front();
        queue.pop_front();
        if (known.count(key))
        {  // resolved meanwhile
            resolve(key, known[key]);
            pump();
            return;
        }
        send(key, coords.first, coords.second);
    }

    void send(const std::string& key, double lat, double lon);

#ifdef HAVE_GEOCODE
    // Pull a readable "locality, region, country" out of whichever shape came back.
    static std::string parse(const std::string& url, JsonNode* root)
    {
        if (!root || !JSON_NODE_HOLDS_OBJECT(root))
            return "";
        JsonObject* obj = json_node_get_object(root);
        auto str = [](JsonObject* o, const char* k) -> std::string
        {
            if (!o || !json_object_has_member(o, k))
                return "";
            JsonNode* n = json_object_get_member(o, k);
            return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
        };
        auto first = [&](JsonObject* o, std::initializer_list<const char*> keys) -> std::string
        {
            for (const char* k : keys)
            {
                std::string v = str(o, k);
                if (!v.empty())
                    return v;
            }
            return "";
        };
        auto join = [](std::initializer_list<std::string> parts)
        {
            std::vector<std::string> kept;
            for (const std::string& p : parts)
            {
                bool dup = p.empty();
                for (const std::string& k : kept)
                    dup |= k == p;
                if (!dup)
                    kept.push_back(p);
            }
            std::string out;
            for (const std::string& k : kept)
                out += (out.empty() ? "" : ", ") + k;
            return out;
        };
        if (url.find("photon") != std::string::npos)
        {
            JsonObject* props = nullptr;
            if (json_object_has_member(obj, "features"))
            {
                JsonArray* features = json_object_get_array_member(obj, "features");
                if (features && json_array_get_length(features) > 0)
                    props = json_object_get_object_member(json_array_get_object_element(features, 0), "properties");
            }
            return join({first(props, {"city", "town", "village", "locality", "name"}), first(props, {"state", "county"}), str(props, "country")});
        }
        if (url.find("bigdatacloud") != std::string::npos)
            return join({first(obj, {"city", "locality"}), str(obj, "principalSubdivision"), str(obj, "countryName")});
        // Nominatim and look-alikes
        JsonObject* addr = json_object_has_member(obj, "address") ? json_object_get_object_member(obj, "address") : nullptr;
        std::string place = join({first(addr, {"city", "town", "village", "hamlet", "municipality", "suburb", "county"}),
                                  first(addr, {"state", "region", "province"}), str(addr, "country")});
        if (place.empty())
            place = first(obj, {"display_name", "name"});
        return place;
    }
#endif
};

#ifdef HAVE_GEOCODE
void Geocoder::Impl::send(const std::string& key, double lat, double lon)
{
    char latb[32], lonb[32];
    std::snprintf(latb, sizeof latb, "%.6f", lat);
    std::snprintf(lonb, sizeof lonb, "%.6f", lon);
    std::string url = url_template;
    for (auto pos = url.find("{lat}"); pos != std::string::npos; pos = url.find("{lat}"))
        url.replace(pos, 5, latb);
    for (auto pos = url.find("{lon}"); pos != std::string::npos; pos = url.find("{lon}"))
        url.replace(pos, 5, lonb);
    HttpHeaders headers{{"User-Agent", "IfritView/" IFRIT_VERSION " (image viewer; reverse geocoding of photo locations)"}, {"Accept", "application/json"}};
    if (!language.empty())
        headers.emplace_back("Accept-Language", language);
    in_flight = true;
    request = http_get(url, headers,
                       [this, key, url](const HttpResponse& r)
                       {
                           std::string place;
                           if (r.status == 200)
                           {
                               JsonParser* parser = json_parser_new();
                               if (json_parser_load_from_data(parser, r.body.data(), gssize(r.body.size()), nullptr))
                                   place = Impl::parse(url, json_parser_get_root(parser));
                               g_object_unref(parser);
                           }
                           else if (g_getenv("IFRIT_TRACE"))
                               std::fprintf(stderr, "geocode: %d %s\n", r.status, r.error.c_str());
                           request.reset();
                           in_flight = false;
                           resolve(key, place);
                           // Nominatim asks for at most one request per second.
                           timer = g_timeout_add(
                               1100,
                               +[](gpointer p) -> gboolean
                               {
                                   auto* impl = static_cast<Impl*>(p);
                                   impl->timer = 0;
                                   impl->pump();
                                   return G_SOURCE_REMOVE;
                               },
                               this);
                       });
}
#else
void Geocoder::Impl::send(const std::string& key, double, double)
{
    resolve(key, "");
}
#endif

Geocoder::Geocoder() : impl_(new Impl)
{
    const gchar* const* langs = g_get_language_names();
    if (langs && langs[0] && std::strcmp(langs[0], "C") != 0)
    {
        std::string l = langs[0];
        for (char& c : l)
            if (c == '_')
                c = '-';
        impl_->language = l.substr(0, l.find('.'));
    }
    impl_->load_cache();
}

Geocoder::~Geocoder()
{
    if (impl_->timer)
        g_source_remove(impl_->timer);
#ifdef HAVE_GEOCODE
    impl_->request.reset();  // cancel; its callback won't run
#endif
    delete impl_;
}

void Geocoder::set_provider(const std::string& url_template)
{
    impl_->url_template = url_template;
    impl_->queue.clear();
}

bool Geocoder::lookup(double lat, double lon, Callback cb)
{
    const std::string key = key_for(lat, lon);
    auto it = impl_->known.find(key);
    if (it != impl_->known.end())
    {
        cb(it->second);
        return true;
    }
    if (impl_->url_template.empty())
        return false;
    const bool queued = impl_->waiting.count(key) != 0;
    impl_->waiting[key].push_back(std::move(cb));
    if (!queued)
    {
        impl_->queue.push_back({key, {lat, lon}});
        impl_->pump();
    }
    return false;
}

const Geocoder::Provider* Geocoder::providers(size_t& count)
{
    count = sizeof kProviders / sizeof kProviders[0];
    return kProviders;
}
