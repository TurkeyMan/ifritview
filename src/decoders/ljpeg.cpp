#include "ljpeg.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _MSC_VER
#include <stdlib.h>
#endif

namespace ljpeg
{
namespace
{

uint64_t load_be64(const uint8_t* p)
{
    uint64_t v;
    std::memcpy(&v, p, 8);
#ifdef _MSC_VER
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

// One Huffman table as a lookup on the next 16 bits: the code's length in
// the high byte (0: no such code), its symbol (the difference's size in
// bits) in the low one.
struct Table
{
    std::vector<uint16_t> lookup;
};

bool build_table(const uint8_t counts[16], const uint8_t* symbols, size_t n, Table& t)
{
    t.lookup.assign(65536, 0);
    uint32_t code = 0;
    size_t k = 0;
    for (int len = 1; len <= 16; ++len)
    {
        for (int i = 0; i < counts[len - 1]; ++i, ++code)
        {
            if (k >= n || code >= (1u << len))
                return false;
            const uint8_t s = symbols[k++];
            if (s > 16)
                return false;
            const uint32_t first = code << (16 - len), last = (code + 1) << (16 - len);
            for (uint32_t c = first; c < last; ++c)
                t.lookup[c] = uint16_t(len << 8 | s);
        }
        code <<= 1;
    }
    return true;
}

struct Stream
{
    const uint8_t* bits;  // unstuffed, with 8 bytes of padding after `size`
    uint64_t size_bits;
    const uint16_t* table[4];  // each component's
    int components;
    bool nikon;  // a difference's bits as Nikon codes them
};

// One sample's difference at `pos` (in bits) with component `c`'s table.
// False for a code that doesn't exist; a speculative decode takes it as a
// one-bit symbol and goes on (it's a deterministic path all the same).
inline bool next(const Stream& s, uint64_t& pos, int c, int& diff)
{
    uint64_t w = load_be64(s.bits + (pos >> 3)) << (pos & 7);
    const uint16_t e = s.table[c][w >> 48];
    const unsigned len = e >> 8, size = e & 0xff;
    if (!len)
    {
        pos += 1;
        diff = 0;
        return false;
    }
    pos += len;
    if (s.nikon)
    {
        // The symbol is a length and a shift: that many bits less the
        // shift, then shifted back up, with a one under them.
        const unsigned bits = size & 15, shift = size >> 4, n = bits > shift ? bits - shift : 0;
        uint32_t v = 0;
        if (n)
        {
            v = uint32_t((w << len) >> (64 - n));
            pos += n;
        }
        diff = int(((v << 1) + 1) << shift >> 1);
        if (bits > 0 && (diff & (1 << (bits - 1))) == 0)
            diff -= (1 << bits) - (shift == 0 ? 1 : 0);
        return true;
    }
    if (size == 0)
        diff = 0;
    else if (size == 16)
        diff = -32768;
    else
    {
        w <<= len;
        const uint32_t v = uint32_t(w >> (64 - size));
        pos += size;
        diff = v < (1u << (size - 1)) ? int(v) - int((1u << size) - 1) : int(v);
    }
    return true;
}

template <typename F>
void parallel(unsigned threads, size_t n, F f)
{
    if (threads <= 1 || n <= 1)
    {
        for (size_t i = 0; i < n; ++i)
            f(i);
        return;
    }
    std::vector<std::thread> pool;
    const unsigned t = unsigned(std::min<size_t>(threads, n));
    for (unsigned k = 0; k < t; ++k)
        pool.emplace_back(
            [&, k]
            {
                for (size_t i = k; i < n; i += t)
                    f(i);
            });
    for (std::thread& th : pool)
        th.join();
}

// Where a stretch of the stream ends: the first code boundary at or past its
// end, the component due there, and how many samples it decoded.
struct Exit
{
    uint64_t pos = 0;
    int component = 0;
    uint64_t count = 0;
};

}  // namespace

namespace
{

// prepare's work, or only its headers' (`header_only`: whether it takes the stream).
bool parse(const uint8_t* data, size_t size, Prepared& p, std::string& error, unsigned threads, bool header_only)
{
    if (threads == 0)
        threads = std::max(1u, std::thread::hardware_concurrency());
    // -- The markers, up to the scan.
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8)
    {
        error = "not a JPEG stream";
        return false;
    }
    Table tables[4];
    bool have_table[4] = {};
    int precision = 0, width = 0, height = 0, ncomp = 0;
    int comp_id[4] = {}, comp_table[4] = {};
    size_t i = 2, scan = 0;
    while (!scan)
    {
        while (i < size && data[i] == 0xFF && i + 1 < size && data[i + 1] == 0xFF)
            ++i;  // fill bytes
        if (i + 4 > size || data[i] != 0xFF)
        {
            error = "truncated before the scan";
            return false;
        }
        const uint8_t marker = data[i + 1];
        const size_t len = size_t(data[i + 2]) << 8 | data[i + 3];
        const uint8_t* seg = data + i + 4;
        if (len < 2 || i + 2 + len > size)
        {
            error = "bad segment";
            return false;
        }
        const size_t body = len - 2;
        switch (marker)
        {
            case 0xC4:  // DHT
                for (size_t j = 0; j + 17 <= body;)
                {
                    const int cls = seg[j] >> 4, id = seg[j] & 15;
                    size_t n = 0;
                    for (int k = 0; k < 16; ++k)
                        n += seg[j + 1 + k];
                    if (cls != 0 || id > 3 || j + 17 + n > body || !build_table(seg + j + 1, seg + j + 17, n, tables[id]))
                    {
                        error = "bad Huffman table";
                        return false;
                    }
                    have_table[id] = true;
                    j += 17 + n;
                }
                break;
            case 0xC3:  // SOF3, lossless
                if (body < 6)
                    return error = "bad frame header", false;
                precision = seg[0];
                height = seg[1] << 8 | seg[2];
                width = seg[3] << 8 | seg[4];
                ncomp = seg[5];
                if (ncomp < 1 || ncomp > 4 || body < 6 + size_t(ncomp) * 3 || precision < 2 || precision > 16)
                    return error = "unsupported frame", false;
                for (int c = 0; c < ncomp; ++c)
                {
                    comp_id[c] = seg[6 + 3 * c];
                    if (seg[7 + 3 * c] != 0x11)
                        return error = "subsampled components", false;
                }
                break;
            case 0xDD:  // DRI
                if (body >= 2 && (seg[0] | seg[1]))
                    return error = "restart intervals", false;
                break;
            case 0xDA:  // SOS
            {
                const int ns = seg[0];
                if (ns != ncomp || body < 1 + size_t(ns) * 2 + 3)
                    return error = "unsupported scan", false;
                for (int k = 0; k < ns; ++k)
                {
                    int c = 0;
                    while (c < ncomp && comp_id[c] != seg[1 + 2 * k])
                        ++c;
                    if (c != k)
                        return error = "scan components out of order", false;
                    comp_table[k] = seg[2 + 2 * k] >> 4;
                    if (comp_table[k] > 3 || !have_table[comp_table[k]])
                        return error = "missing Huffman table", false;
                }
                const int predictor = seg[1 + 2 * ns], transform = seg[3 + 2 * ns] & 15;
                if (predictor != 1 || transform != 0)
                    return error = "unsupported predictor", false;
                scan = i + 2 + len;
                break;
            }
            default:
                if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC)
                    return error = "not lossless JPEG", false;
                break;
        }
        i += 2 + len;
    }
    if (!width || !height || !ncomp)
        return error = "no frame", false;
    if (header_only)
        return true;

    // -- The entropy-coded data without its stuffed zero bytes, to the marker
    // that ends it.
    // In ranges on every thread: where the data ends (the first 0xFF not
    // followed by a zero: a marker), then each range's bytes less the zeros
    // stuffed after an 0xFF, each range copied to where the ones before it
    // end.
    const size_t span = size - scan;
    const size_t ranges = std::max<size_t>(1, std::min<size_t>(threads * 4, span / 65536));
    const size_t range = (span + ranges - 1) / ranges;
    std::vector<size_t> marker(ranges, size), kept(ranges + 1, 0);
    parallel(threads, ranges,
             [&](size_t r)
             {
                 const size_t a = scan + r * range, b = std::min(size, a + range);
                 for (const uint8_t* p = data + a; (p = static_cast<const uint8_t*>(std::memchr(p, 0xFF, size_t(data + b - p)))) != nullptr; ++p)
                     if (size_t(p - data) + 1 >= size || p[1] != 0x00)
                     {
                         marker[r] = size_t(p - data);
                         break;
                     }
             });
    const size_t end = *std::min_element(marker.begin(), marker.end());
    const auto stuffed = [&](size_t j) { return j > scan && data[j] == 0x00 && data[j - 1] == 0xFF; };
    parallel(threads, ranges,
             [&](size_t r)
             {
                 const size_t a = std::min(end, scan + r * range), b = std::min(end, a + range);
                 size_t n = b - a;
                 for (const uint8_t* p = data + a; p < data + b && (p = static_cast<const uint8_t*>(std::memchr(p, 0xFF, size_t(data + b - p)))) != nullptr; ++p)
                     if (stuffed(size_t(p - data) + 1) && size_t(p - data) + 1 < b)
                         --n;
                 if (a < b && stuffed(a))
                     --n;  // its 0xFF ended the range before
                 kept[r + 1] = n;
             });
    for (size_t r = 0; r < ranges; ++r)
        kept[r + 1] += kept[r];
    std::vector<uint8_t> bits(kept[ranges] + 8, 0);
    parallel(threads, ranges,
             [&](size_t r)
             {
                 const size_t a = std::min(end, scan + r * range), b = std::min(end, a + range);
                 uint8_t* o = bits.data() + kept[r];
                 for (size_t j = a; j < b;)
                 {
                     if (stuffed(j))
                     {
                         ++j;
                         continue;
                     }
                     const uint8_t* ff = static_cast<const uint8_t*>(std::memchr(data + j, 0xFF, b - j));
                     const size_t until = ff ? size_t(ff - data) + 1 : b;  // through the 0xFF
                     std::memcpy(o, data + j, until - j);
                     o += until - j;
                     j = until;
                 }
             });
    p.width = width, p.height = height, p.components = ncomp, p.precision = precision;
    p.step = 1;
    for (int c = 0; c < 4; ++c)
        p.initial[0][c] = p.initial[1][c] = uint16_t(1u << (precision - 1));
    p.total_bits = uint64_t(kept[ranges]) * 8;
    p.bits = std::move(bits);
    for (int c = 0; c < ncomp; ++c)
        p.lookup[c] = tables[comp_table[c]].lookup;
    return true;
}

}  // namespace

bool prepare(const uint8_t* data, size_t size, Prepared& p, std::string& error, unsigned threads)
{
    return parse(data, size, p, error, threads, false);
}

bool prepare_nikon(const uint8_t* data, size_t size, int columns, int rows, const uint8_t tree[32], const uint16_t start[2][2],
                   const uint16_t* curve, size_t curve_size, Prepared& p, std::string& error)
{
    if (columns < 2 || columns % 2 || rows < 1 || !size)
        return error = "not a Nikon stream", false;
    size_t n = 0;
    for (int k = 0; k < 16; ++k)
        n += tree[k];
    Table t;
    if (n > 16 || !build_table(tree, tree + 16, n, t))
        return error = "bad Nikon tree", false;
    p = Prepared{};
    p.width = columns / 2, p.height = rows, p.components = 2, p.precision = 14;
    p.nikon = true;
    p.step = 2;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 2; ++c)
            p.initial[r][c] = start[r][c];
    p.bits.assign(data, data + size);
    p.bits.resize(size + 8, 0);
    p.total_bits = uint64_t(size) * 8;
    p.lookup[0] = p.lookup[1] = t.lookup;
    if (curve)
        p.curve.assign(curve, curve + curve_size);
    return true;
}

bool takes(const uint8_t* data, size_t size)
{
    Prepared p;
    std::string error;
    return parse(data, size, p, error, 1, true);
}

bool decode(const uint8_t* data, size_t size, Image& out, std::string& error, unsigned threads)
{
    Prepared p;
    return prepare(data, size, p, error, threads) && decode(p, out, error, threads);
}

bool decode(const Prepared& p, Image& out, std::string& error, unsigned threads)
{
    if (threads == 0)
        threads = std::max(1u, std::thread::hardware_concurrency());
    // IFRIT_TRACE: each pass's time.
    static const bool trace = std::getenv("IFRIT_TRACE") != nullptr;
    auto last = std::chrono::steady_clock::now();
    std::string times;
    const auto mark = [&](const char* what)
    {
        if (!trace)
            return;
        const auto now = std::chrono::steady_clock::now();
        times += std::string(times.empty() ? "" : ", ") + what + " " + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now - last).count() / 1000) + " ms";
        last = now;
    };
    const int width = p.width, height = p.height, ncomp = p.components, precision = p.precision;
    const uint64_t total_bits = p.total_bits;
    if (!width || !height || ncomp < 1 || ncomp > 4 || p.bits.size() < total_bits / 8 + 8)
        return error = "not prepared", false;
    Stream s{p.bits.data(), total_bits, {}, ncomp, p.nikon};
    for (int c = 0; c < ncomp; ++c)
    {
        if (p.lookup[c].size() != 65536)
            return error = "not prepared", false;
        s.table[c] = p.lookup[c].data();
    }
    const uint64_t samples = uint64_t(width) * height * ncomp;
    out.width = width, out.height = height, out.components = ncomp, out.precision = precision;
    out.samples.assign(samples, 0);
    uint16_t* d = out.samples.data();

    // -- Pass 1: each chunk decoded from its start as if a code began there,
    // once for each component that could be due (each has its own table), to
    // past its end.
    const size_t chunks = std::clamp<size_t>(size_t(threads) * 4, 1, std::max<uint64_t>(1, total_bits / 4096));
    const uint64_t chunk_bits = (total_bits + chunks - 1) / chunks;
    std::vector<Exit> guess(chunks * ncomp);
    parallel(threads, chunks * ncomp,
             [&](size_t g)
             {
                 const size_t k = g / ncomp;
                 uint64_t pos = k * chunk_bits, n = 0;
                 const uint64_t end = std::min(total_bits, (k + 1) * chunk_bits);
                 int c = int(g % ncomp), diff;
                 while (pos < end)
                 {
                     next(s, pos, c, diff);
                     c = c + 1 == ncomp ? 0 : c + 1;
                     ++n;
                 }
                 guess[g] = {pos, c, n};
             });
    mark("guessed");
    // -- Pass 2: the true decode's way into each chunk is the true way out of
    // the one before. Followed alongside the guess until the two meet on a
    // code boundary with the same component due: from there the guess was
    // right, and its exit and count (less what it decoded before the meeting,
    // plus what the true one did) stand.
    // The true decode stops at the last sample: what follows is padding.
    std::vector<Exit> entry(chunks), exit(chunks);
    entry[0] = {0, 0, 0};
    exit[0] = guess[0];
    uint64_t done = guess[0].count;
    size_t met_count = 0;
    uint64_t lockstep = 0;
    for (size_t k = 1; k < chunks; ++k)
    {
        entry[k] = {exit[k - 1].pos, exit[k - 1].component, 0};
        if (done >= samples)
        {
            exit[k] = entry[k];
            continue;
        }
        // The guesses advance together, the true decode catching up with
        // the furthest behind, until it stands where one of them does.
        const uint64_t end = std::min(total_bits, (k + 1) * chunk_bits);
        uint64_t tp = entry[k].pos, a = 0;
        int tc = entry[k].component, diff;
        uint64_t sp[4], b[4];
        int sc[4];
        for (int g = 0; g < ncomp; ++g)
            sp[g] = k * chunk_bits, sc[g] = g, b[g] = 0;
        int met = -1;
        while (tp < end && done + a < samples && met < 0)
        {
            bool behind = false;
            for (int g = 0; g < ncomp; ++g)
            {
                if (sp[g] == tp && sc[g] == tc)
                {
                    met = g;
                    break;
                }
                while (sp[g] < tp && sp[g] < guess[k * ncomp + g].pos)
                {
                    next(s, sp[g], sc[g], diff);
                    sc[g] = sc[g] + 1 == ncomp ? 0 : sc[g] + 1;
                    ++b[g];
                }
                if (sp[g] == tp && sc[g] == tc)
                {
                    met = g;
                    break;
                }
                behind |= sp[g] < guess[k * ncomp + g].pos;
            }
            if (met >= 0 || !behind)
                break;
            if (!next(s, tp, tc, diff))
                return error = "corrupt data", false;
            tc = tc + 1 == ncomp ? 0 : tc + 1;
            ++a;
        }
        if (met >= 0)
        {
            const Exit& g = guess[k * ncomp + met];
            exit[k] = {g.pos, g.component, a + (g.count - b[met])};
            ++met_count;
            lockstep += a + b[met];
        }
        else
        {
            while (tp < end && done + a < samples)
            {
                if (!next(s, tp, tc, diff))
                    return error = "corrupt data", false;
                tc = tc + 1 == ncomp ? 0 : tc + 1;
                ++a;
            }
            exit[k] = {tp, tc, a};
            lockstep += a;
        }
        done += exit[k].count;
    }
    mark("synchronised");
    if (trace)
        times += " (" + std::to_string(met_count) + " of " + std::to_string(chunks - 1) + " met, " + std::to_string(lockstep) + " steps)";
    // -- Pass 3: each chunk decoded again from its true entry, its differences
    // written where they go (the first sample's place: the counts before it).
    std::vector<uint64_t> first(chunks + 1, 0);
    for (size_t k = 0; k < chunks; ++k)
        first[k + 1] = first[k] + exit[k].count;
    if (first[chunks] < samples)
        return error = "too few samples", false;
    bool bad = false;
    parallel(threads, chunks,
             [&](size_t k)
             {
                 if (first[k] >= samples)
                     return;
                 uint64_t pos = entry[k].pos;
                 int c = entry[k].component, diff;
                 const uint64_t n = std::min(exit[k].count, samples - first[k]);
                 uint16_t* o = d + first[k];
                 for (uint64_t j = 0; j < n; ++j)
                 {
                     if (!next(s, pos, c, diff))
                     {
                         bad = true;
                         return;
                     }
                     o[j] = uint16_t(diff);
                     c = c + 1 == ncomp ? 0 : c + 1;
                 }
             });
    if (bad)
        return error = "corrupt data", false;

    mark("decoded");
    // -- Pass 4: differences to samples. Each row's first samples continue
    // down from those `step` rows above (the first rows' from where the
    // stream says they start); the rest from their left, so the rows go in
    // parallel. Arithmetic modulo 2^16, as the standard has it; then a
    // Nikon's curve, clamped to it.
    const size_t row = size_t(width) * ncomp;
    for (int c = 0; c < ncomp; ++c)
        for (int y = 0; y < height; ++y)
        {
            const uint16_t above = y < p.step ? p.initial[y][c] : d[(y - p.step) * row + c];
            d[y * row + c] = uint16_t(above + d[y * row + c]);
        }
    const uint16_t* curve = p.curve.empty() ? nullptr : p.curve.data();
    const int top = int(p.curve.size()) - 1;
    parallel(threads, size_t(height),
             [&](size_t y)
             {
                 uint16_t* r = d + y * row;
                 for (size_t x = ncomp; x < row; ++x)
                     r[x] = uint16_t(r[x - ncomp] + r[x]);
                 if (curve)
                     for (size_t x = 0; x < row; ++x)
                         r[x] = curve[std::clamp(int(int16_t(r[x])), 0, top)];
             });
    mark("summed");
    if (trace)
        std::fprintf(stderr, "ljpeg: %dx%d x%d, %zu chunks on %u threads: %s\n", width, height, ncomp, chunks, threads, times.c_str());
    return true;
}

}  // namespace ljpeg
