#pragma once

#include <functional>
#include <string>

// Reverse geocoding: a coordinate in, a short human-readable place out
// ("Paris, Île-de-France, France"). Lookups are asynchronous, rate limited
// to one request per second (Nominatim's policy), and cached on disk by
// coordinate rounded to ~100 m so a burst from one spot costs one request.
//
// The URL template takes {lat} and {lon}. Known providers get a tailored
// parse of their JSON; anything else is scanned for the usual fields.
class Geocoder
{
public:
    using Callback = std::function<void(const std::string& place)>;

    Geocoder();
    ~Geocoder();
    Geocoder(const Geocoder&) = delete;
    Geocoder& operator=(const Geocoder&) = delete;

    // Empty URL disables lookups (callbacks never fire).
    void set_provider(const std::string& url_template);

    // Returns true and sets `place` when already known; otherwise queues a
    // request and calls `cb` on the main thread when it resolves. A cached
    // miss (previous lookup found nothing) returns true with an empty place.
    bool lookup(double latitude, double longitude, Callback cb);

    // Presets offered in settings.
    struct Provider
    {
        const char* name;
        const char* url;
    };
    static const Provider* providers(size_t& count);

    struct Impl;

private:
    Impl* impl_;
};
