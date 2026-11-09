#include "folder_block.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>

namespace
{

constexpr uint8_t kVersion = 1;
constexpr double kDegrees = 1e6;  // positions kept to about 11 cm
constexpr int64_t kMaxLatitude = 90'000'000, kMaxLongitude = 180'000'000;
constexpr int64_t kMaxTime = int64_t(1) << 62;  // inside it, any two can be subtracted
constexpr uint64_t kMaxRecords = 4'000'000, kMaxText = uint64_t(256) << 20;  // what a damaged block may ask for

struct Writer
{
    std::vector<uint8_t> out;
    void u(uint64_t v)
    {
        while (v >= 0x80)
        {
            out.push_back(uint8_t(v) | 0x80);
            v >>= 7;
        }
        out.push_back(uint8_t(v));
    }
    void s(int64_t v) { u((uint64_t(v) << 1) ^ uint64_t(v >> 63)); }
    void text(const std::string& t)
    {
        u(t.size());
        out.insert(out.end(), t.begin(), t.end());
    }
};

struct Reader
{
    const uint8_t* at;
    const uint8_t* end;
    bool ok = true;
    uint64_t text_left = kMaxText;
    // Room for `n` more bytes of decoded text; a little input can ask for a lot.
    bool spend(uint64_t n)
    {
        if (n > text_left)
            return ok = false;
        text_left -= n;
        return true;
    }
    uint64_t u()
    {
        uint64_t v = 0;
        for (int shift = 0; shift < 64 && at < end; shift += 7)
        {
            const uint8_t b = *at++;
            if (shift == 63 && b > 1)
                break;  // more than 64 bits
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80))
                return v;
        }
        ok = false;
        return 0;
    }
    int64_t s()
    {
        const uint64_t v = u();
        return int64_t(v >> 1) ^ -int64_t(v & 1);
    }
    size_t left() const { return size_t(end - at); }
    std::string bytes(uint64_t n)
    {
        if (n > left())
        {
            ok = false;
            return {};
        }
        std::string t(reinterpret_cast<const char*>(at), size_t(n));
        at += n;
        return t;
    }
    std::string text() { return bytes(u()); }
};

// A list of the different values, then runs of which one each file has.
template <class T, class Put>
void put_column(Writer& w, const std::vector<T>& values, Put put)
{
    std::map<T, uint64_t> known;
    std::vector<const T*> dict;
    std::vector<uint64_t> ids;
    ids.reserve(values.size());
    for (const T& v : values)
    {
        const auto [it, added] = known.try_emplace(v, dict.size());
        if (added)
            dict.push_back(&it->first);
        ids.push_back(it->second);
    }
    w.u(dict.size());
    for (const T* d : dict)
        put(*d);
    for (size_t i = 0; i < ids.size();)
    {
        size_t j = i + 1;
        while (j < ids.size() && ids[j] == ids[i])
            ++j;
        w.u(ids[i]);
        w.u(j - i);
        i = j;
    }
}

template <class T, class Get, class Cost>
bool get_column(Reader& r, size_t count, std::vector<T>& values, Get get, Cost cost)
{
    const uint64_t distinct = r.u();
    if (!r.ok || distinct > count || distinct > r.left())
        return false;
    std::vector<T> dict;
    dict.reserve(size_t(distinct));
    for (uint64_t i = 0; i < distinct; ++i)
        dict.push_back(get());
    values.clear();
    values.reserve(count);
    while (values.size() < count)
    {
        const uint64_t id = r.u(), run = r.u();
        if (!r.ok || id >= dict.size() || run == 0 || run > count - values.size() || cost(dict[size_t(id)]) > kMaxText / run ||
            !r.spend(run * cost(dict[size_t(id)])))
            return false;
        values.insert(values.end(), size_t(run), dict[size_t(id)]);
    }
    return r.ok;
}

// Differences from a damaged block mustn't overflow.
bool add(int64_t& value, int64_t delta)
{
    using L = std::numeric_limits<int64_t>;
    if ((delta > 0 && value > L::max() - delta) || (delta < 0 && value < L::min() - delta))
        return false;
    value += delta;
    return true;
}

// The name up to its extension, which has a column of its own.
size_t stem_length(const std::string& name)
{
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? name.size() : dot;
}

}  // namespace

std::vector<uint8_t> encode_folder_block(std::vector<FileRecord>& records)
{
    std::sort(records.begin(), records.end(), [](const FileRecord& a, const FileRecord& b) { return a.name < b.name; });
    Writer w;
    w.out.push_back(kVersion);
    w.u(records.size());

    std::string previous;
    for (const FileRecord& f : records)
    {
        const std::string stem = f.name.substr(0, stem_length(f.name));
        size_t shared = 0;
        while (shared < stem.size() && shared < previous.size() && stem[shared] == previous[shared])
            ++shared;
        w.u(shared);
        w.text(stem.substr(shared));
        previous = stem;
    }

    std::vector<std::string> extensions, cameras;
    std::vector<uint64_t> sizes, orientations, located;
    for (const FileRecord& f : records)
    {
        extensions.push_back(f.name.substr(stem_length(f.name)));
        cameras.push_back(f.camera);
        sizes.push_back(uint64_t(f.width) << 32 | f.height);
        orientations.push_back(f.orientation);
        located.push_back(f.has_location);
    }
    const auto put_text = [&](const std::string& t) { w.text(t); };
    const auto put_number = [&](uint64_t v) { w.u(v); };
    put_column(w, extensions, put_text);
    put_column(w, cameras, put_text);
    put_column(w, sizes, put_number);
    put_column(w, orientations, put_number);
    put_column(w, located, put_number);

    int64_t mtime = 0, taken = 0, lat = 0, lon = 0;
    for (const FileRecord& f : records)
    {
        w.u(uint64_t(f.size));
        w.s(f.mtime - mtime);
        w.s(f.taken - taken);
        mtime = f.mtime;
        taken = f.taken;
        if (f.has_location)
        {
            const int64_t y = std::llround(f.latitude * kDegrees), x = std::llround(f.longitude * kDegrees);
            w.s(y - lat);
            w.s(x - lon);
            lat = y;
            lon = x;
        }
    }
    return std::move(w.out);
}

static bool decode_block(const uint8_t* data, size_t size, std::vector<FileRecord>& records);

bool decode_folder_block(const uint8_t* data, size_t size, std::vector<FileRecord>& records)
{
    try
    {
        return decode_block(data, size, records);
    }
    catch (const std::bad_alloc&)
    {
        records.clear();
        return false;
    }
}

static bool decode_block(const uint8_t* data, size_t size, std::vector<FileRecord>& records)
{
    records.clear();
    if (size < 2 || data[0] != kVersion)
        return false;
    Reader r{data + 1, data + size};
    const uint64_t n = r.u();
    if (!r.ok || n > r.left() || n > kMaxRecords)  // every file takes bytes
        return false;
    const size_t count = size_t(n);

    std::vector<FileRecord> out(count);
    std::string previous;
    std::vector<size_t> stems(count);
    for (size_t i = 0; i < count; ++i)
    {
        const uint64_t shared = r.u();
        if (!r.ok || shared > previous.size())
            return false;
        previous = previous.substr(0, size_t(shared)) + r.text();
        if (!r.ok || !r.spend(previous.size()))
            return false;
        out[i].name = previous;
    }

    std::vector<std::string> extensions, cameras;
    std::vector<uint64_t> sizes, orientations, located;
    const auto get_text = [&] { return r.text(); };
    const auto get_number = [&] { return r.u(); };
    const auto text_cost = [](const std::string& t) { return uint64_t(t.size()); };
    const auto no_cost = [](uint64_t) { return uint64_t(0); };
    if (!get_column(r, count, extensions, get_text, text_cost) || !get_column(r, count, cameras, get_text, text_cost) ||
        !get_column(r, count, sizes, get_number, no_cost) || !get_column(r, count, orientations, get_number, no_cost) ||
        !get_column(r, count, located, get_number, no_cost))
        return false;

    int64_t mtime = 0, taken = 0, lat = 0, lon = 0;
    for (size_t i = 0; i < count; ++i)
    {
        FileRecord& f = out[i];
        if (orientations[i] > 8 || located[i] > 1)
            return false;
        f.name += extensions[i];
        f.camera = std::move(cameras[i]);
        f.width = uint32_t(sizes[i] >> 32);
        f.height = uint32_t(sizes[i]);
        f.orientation = uint8_t(orientations[i]);
        f.has_location = located[i] != 0;
        const uint64_t bytes = r.u();
        if (bytes > uint64_t(std::numeric_limits<int64_t>::max()) || !add(mtime, r.s()) || !add(taken, r.s()) || mtime <= -kMaxTime || mtime >= kMaxTime ||
            taken <= -kMaxTime || taken >= kMaxTime)
            return false;
        f.size = int64_t(bytes);
        f.mtime = mtime;
        f.taken = taken;
        if (f.has_location)
        {
            if (!add(lat, r.s()) || !add(lon, r.s()) || lat < -kMaxLatitude || lat > kMaxLatitude || lon < -kMaxLongitude || lon > kMaxLongitude)
                return false;
            f.latitude = double(lat) / kDegrees;
            f.longitude = double(lon) / kDegrees;
        }
    }
    if (!r.ok)
        return false;
    records = std::move(out);
    return true;
}
