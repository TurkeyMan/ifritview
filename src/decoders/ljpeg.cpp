#include "ljpeg.h"

#include "../bytes.h"
#include "../parallel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace ljpeg
{
namespace
{

bool build_table(const uint8_t counts[16], const uint8_t* symbols, size_t n, Table& t)
{
    t = Table{};
    if (n > sizeof t.symbols)
        return false;
    uint32_t code = 0;
    size_t k = 0;
    for (int len = 1; len <= 16; ++len)
    {
        t.maxcode[len] = -1;
        t.delta[len] = int(k) - int(code);
        for (int i = 0; i < counts[len - 1]; ++i, ++code)
        {
            if (k >= n || code >= (1u << len))
                return false;
            const uint8_t s = symbols[k];
            if (s > 16)
                return false;
            t.symbols[k++] = s;
            t.maxcode[len] = int32_t(code);
            if (len <= Table::kFast)
            {
                const uint32_t first = code << (Table::kFast - len), last = (code + 1) << (Table::kFast - len);
                for (uint32_t c = first; c < last; ++c)
                    t.fast[c] = uint16_t(len << 8 | s);
            }
        }
        code <<= 1;
    }
    return true;
}

struct Stream
{
    const uint8_t* bits;  // unstuffed, with 8 bytes of padding after `size`
    uint64_t size_bits;
    const Table* table[4];  // each component's
    int components;
    bool nikon;  // a difference's bits as Nikon codes them
};

// One sample's difference at `pos` (in bits) with component `c`'s table.
// False for a code that doesn't exist; a speculative decode takes it as a
// one-bit symbol and goes on (it's a deterministic path all the same).
inline bool next(const Stream& s, uint64_t& pos, int c, int& diff)
{
    uint64_t w = be64(s.bits + (pos >> 3)) << (pos & 7);
    const Table& t = *s.table[c];
    const uint16_t e = t.fast[w >> (64 - Table::kFast)];
    unsigned len = e >> 8, size = e & 0xff;
    if (!len)
    {
        // Longer than the lookup: by its length.
        const uint32_t v = uint32_t(w >> 48);
        for (unsigned l = Table::kFast + 1; l <= 16; ++l)
            if (const int code = int(v >> (16 - l)); code <= t.maxcode[l])
            {
                len = l;
                size = t.symbols[code + t.delta[l]];
                break;
            }
        if (!len)
        {
            pos += 1;
            diff = 0;
            return false;
        }
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

// Where the entropy-coded data from `from` on ends: the first 0xFF not
// followed by a zero (a marker), or `size`. In ranges on `threads`.
size_t data_end(const uint8_t* data, size_t from, size_t size, unsigned threads)
{
    const size_t span = size - from;
    const size_t ranges = std::max<size_t>(1, std::min<size_t>(threads * 4, span / 65536));
    const size_t range = (span + ranges - 1) / ranges;
    std::vector<size_t> marker(ranges, size);
    parallel(
        ranges,
        [&](size_t r)
        {
            const size_t a = from + r * range, b = std::min(size, a + range);
            for (const uint8_t* p = data + a; (p = static_cast<const uint8_t*>(std::memchr(p, 0xFF, size_t(data + b - p)))) != nullptr; ++p)
                if (size_t(p - data) + 1 >= size || p[1] != 0x00)
                {
                    marker[r] = size_t(p - data);
                    break;
                }
        },
        threads);
    return *std::min_element(marker.begin(), marker.end());
}

// The entropy-coded data from `from` to `end` less the zeros stuffed after
// each 0xFF, then 8 zero bytes. In ranges on `threads`: each range's bytes
// counted, then copied to where the ones before it end.
std::vector<uint8_t> unstuff(const uint8_t* data, size_t from, size_t end, unsigned threads)
{
    const size_t span = end - from;
    const size_t ranges = std::max<size_t>(1, std::min<size_t>(threads * 4, span / 65536));
    const size_t range = (span + ranges - 1) / ranges;
    std::vector<size_t> kept(ranges + 1, 0);
    const auto stuffed = [&](size_t j) { return j > from && data[j] == 0x00 && data[j - 1] == 0xFF; };
    parallel(
        ranges,
        [&](size_t r)
        {
            const size_t a = std::min(end, from + r * range), b = std::min(end, a + range);
            size_t n = b - a;
            for (const uint8_t* p = data + a; p < data + b && (p = static_cast<const uint8_t*>(std::memchr(p, 0xFF, size_t(data + b - p)))) != nullptr; ++p)
                if (stuffed(size_t(p - data) + 1) && size_t(p - data) + 1 < b)
                    --n;
            if (a < b && stuffed(a))
                --n;  // its 0xFF ended the range before
            kept[r + 1] = n;
        },
        threads);
    for (size_t r = 0; r < ranges; ++r)
        kept[r + 1] += kept[r];
    std::vector<uint8_t> bits(kept[ranges] + 8, 0);
    parallel(
        ranges,
        [&](size_t r)
        {
            const size_t a = std::min(end, from + r * range), b = std::min(end, a + range);
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
        },
        threads);
    return bits;
}

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
    int precision = 0, width = 0, height = 0, ncomp = 0, predictor = 1, restart = 0;
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
            case 0xDD:  // DRI (0: none, LibRaw neither resetting its predictions nor its bits)
                if (body < 2)
                    return error = "bad restart interval", false;
                restart = seg[0] << 8 | seg[1];
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
                    // LibRaw (the reference) takes component k's table as
                    // the one numbered k, or the last numbered before it,
                    // whatever the scan says (a CR2's are alike): where
                    // the two differ, not taken.
                    int libraw = k;
                    while (libraw > 0 && !have_table[libraw])
                        --libraw;
                    if (libraw != comp_table[k] && std::memcmp(&tables[libraw], &tables[comp_table[k]], sizeof(Table)) != 0)
                        return error = "tables not LibRaw's", false;
                }
                predictor = seg[1 + 2 * ns];
                if (predictor < 1 || predictor > 7 || (seg[3 + 2 * ns] & 15) != 0)
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
    // LibRaw starts its predictions again only at a row whose first sample
    // begins an interval, and predicts that row from the one above all the
    // same: intervals of whole rows predicted from the left read as the
    // standard has them.
    if (restart && (restart % width || predictor != 1))
        return error = "restart intervals other than whole rows predicted from the left", false;
    p.predictor = predictor;
    if (header_only)
        return true;

    // -- The entropy-coded data without its stuffed zero bytes, to the marker
    // that ends it; with restart intervals, each interval's to its RST marker
    // (LibRaw's ljpeg_row reads on past it), a stream of its own.
    const int rows = restart ? restart / width : height, intervals = (height + rows - 1) / rows;
    std::vector<std::pair<size_t, size_t>> spans{{scan, data_end(data, scan, size, restart ? 1 : threads)}};
    while (int(spans.size()) < intervals && spans.back().second + 1 < size && (data[spans.back().second + 1] & 0xF8) == 0xD0)
    {
        const size_t from = spans.back().second + 2;
        spans.emplace_back(from, data_end(data, from, size, 1));
    }
    if (int(spans.size()) != intervals)
        return error = "restart intervals not the frame's", false;
    std::vector<std::vector<uint8_t>> pieces(spans.size());
    parallel(spans.size(), [&](size_t k) { pieces[k] = unstuff(data, spans[k].first, spans[k].second, spans.size() > 1 ? 1 : threads); }, threads);
    p.width = width, p.height = rows, p.components = ncomp, p.precision = precision;
    p.step = 1;
    for (int c = 0; c < 4; ++c)
        p.initial[0][c] = p.initial[1][c] = uint16_t(1u << (precision - 1));
    p.parts.assign(pieces.size(), {});
    std::vector<size_t> at(pieces.size() + 1, 0);
    for (size_t k = 0; k < pieces.size(); ++k)
    {
        at[k + 1] = at[k] + pieces[k].size();  // each with its padding
        p.parts[k].start = uint64_t(at[k]) * 8;
        p.parts[k].bits = uint64_t(pieces[k].size() - 8) * 8;
        p.parts[k].rows = std::min(rows, height - int(k) * rows);
    }
    if (pieces.size() == 1)
        p.bits = std::move(pieces[0]);
    else
    {
        p.bits.resize(at.back());
        parallel(pieces.size(), [&](size_t k) { std::memcpy(p.bits.data() + at[k], pieces[k].data(), pieces[k].size()); }, threads);
    }
    p.tables.clear();
    int index[4] = {-1, -1, -1, -1};
    for (int c = 0; c < ncomp; ++c)
    {
        int& i = index[comp_table[c]];
        if (i < 0)
        {
            i = int(p.tables.size());
            p.tables.push_back(tables[comp_table[c]]);
        }
        for (Prepared::Part& part : p.parts)
            part.table[c] = uint32_t(i);
    }
    return true;
}

}  // namespace

bool prepare(const uint8_t* data, size_t size, Prepared& p, std::string& error, unsigned threads)
{
    p = Prepared{};
    return parse(data, size, p, error, threads, false);
}

bool prepare_tiles(const uint8_t* file, size_t size, const std::vector<uint64_t>& offsets, Prepared& p, std::string& error, unsigned threads)
{
    if (threads == 0)
        threads = std::max(1u, std::thread::hardware_concurrency());
    const auto t0 = std::chrono::steady_clock::now();
    p = Prepared{};
    if (offsets.empty())
        return error = "no tiles", false;
    for (uint64_t at : offsets)
        if (at >= size)
            return error = "a tile past the file's end", false;
    if (offsets.size() == 1)
        return parse(file + offsets[0], size_t(size - offsets[0]), p, error, threads, false);
    // Each tile on its own (a tile's stream is read to the marker ending
    // it, however far the file goes on), then one after another.
    std::vector<Prepared> tiles(offsets.size());
    std::vector<std::string> errors(offsets.size());
    parallel(offsets.size(), [&](size_t k) { parse(file + offsets[k], size_t(size - offsets[k]), tiles[k], errors[k], 1, false); }, threads);
    std::vector<size_t> at(tiles.size() + 1, 0);
    for (size_t k = 0; k < tiles.size(); ++k)
    {
        const Prepared& t = tiles[k];
        if (t.parts.size() != 1)
            return error = "tile " + std::to_string(k) + ": " + (errors[k].empty() ? "restart intervals" : errors[k]), false;
        if (t.width != tiles[0].width || t.height != tiles[0].height || t.components != tiles[0].components || t.precision != tiles[0].precision ||
            t.predictor != tiles[0].predictor)
            return error = "tiles not alike", false;
        at[k + 1] = at[k] + t.bits.size();  // each with its padding
    }
    p.width = tiles[0].width, p.height = tiles[0].height, p.components = tiles[0].components, p.precision = tiles[0].precision;
    p.predictor = tiles[0].predictor;
    p.step = 1;
    std::copy(&tiles[0].initial[0][0], &tiles[0].initial[0][0] + 8, &p.initial[0][0]);
    p.bits.resize(at.back());
    p.parts.resize(tiles.size());
    for (size_t k = 0; k < tiles.size(); ++k)
    {
        Prepared::Part& part = p.parts[k];
        part = tiles[k].parts[0];
        part.start = uint64_t(at[k]) * 8;
        for (int c = 0; c < p.components; ++c)
            part.table[c] += uint32_t(p.tables.size());
        p.tables.insert(p.tables.end(), tiles[k].tables.begin(), tiles[k].tables.end());
    }
    parallel(tiles.size(), [&](size_t k) { std::memcpy(p.bits.data() + at[k], tiles[k].bits.data(), tiles[k].bits.size()); }, threads);
    if (std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "ljpeg: %zu tiles prepared in %.1f ms\n", tiles.size(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

bool prepare_nikon(const uint8_t* data, size_t size, int columns, int rows, const uint8_t tree[32], const uint16_t start[2][2], const uint16_t* curve,
                   size_t curve_size, Prepared& p, std::string& error)
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
    p.parts.assign(1, {});
    p.parts[0].bits = uint64_t(size) * 8;
    p.parts[0].rows = rows;
    p.tables.assign(1, t);
    if (curve && curve_size)
    {
        // As nikon_load_raw indexes it: the sample as a signed 16 bits,
        // clamped to the curve.
        p.curve.resize(65536);
        const int top = int(curve_size) - 1;
        for (int v = 0; v < 65536; ++v)
            p.curve[v] = curve[std::clamp(int(int16_t(uint16_t(v))), 0, top)];
    }
    return true;
}

bool takes(const uint8_t* data, size_t size, int* predictor)
{
    Prepared p;
    std::string error;
    if (!parse(data, size, p, error, 1, true))
        return false;
    if (predictor)
        *predictor = p.predictor;
    return true;
}

bool decode(const uint8_t* data, size_t size, Image& out, std::string& error, unsigned threads)
{
    Prepared p;
    return prepare(data, size, p, error, threads) && decode(p, out, error, threads);
}

namespace
{

// A stream's differences decoded on this thread alone, start to end.
bool decode_serial(const Stream& s, uint64_t samples, uint16_t* d, std::string& error)
{
    uint64_t pos = 0;
    int c = 0, diff;
    for (uint64_t j = 0; j < samples; ++j)
    {
        if (pos >= s.size_bits)
            return error = "too few samples", false;
        if (!next(s, pos, c, diff))
            return error = "corrupt data", false;
        d[j] = uint16_t(diff);
        c = c + 1 == s.components ? 0 : c + 1;
    }
    return true;
}

// One stream's differences into `d` (its frame's samples), on `threads`.
bool decode_differences(const Prepared& p, const Prepared::Part& part, uint16_t* d, unsigned threads, std::string& error, std::string& times)
{
    static const bool trace = std::getenv("IFRIT_TRACE") != nullptr;
    auto last = std::chrono::steady_clock::now();
    const auto mark = [&](const char* what)
    {
        if (!trace)
            return;
        const auto now = std::chrono::steady_clock::now();
        times += std::string(times.empty() ? "" : ", ") + what + " " +
                 std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now - last).count() / 1000) + " ms";
        last = now;
    };
    const int ncomp = p.components;
    const uint64_t total_bits = part.bits;
    Stream s{p.bits.data() + part.start / 8, total_bits, {}, ncomp, p.nikon};
    for (int c = 0; c < ncomp; ++c)
        s.table[c] = &p.tables[part.table[c]];
    const uint64_t samples = uint64_t(p.width) * part.rows * ncomp;

    // -- Pass 1: each chunk decoded from its start as if a code began there,
    // once for each component that could be due (each has its own table), to
    // past its end.
    const size_t chunks = threads <= 1 ? 1 : std::clamp<size_t>(size_t(threads) * 4, 1, std::max<uint64_t>(1, total_bits / 4096));
    if (chunks == 1)
        return decode_serial(s, samples, d, error);
    const uint64_t chunk_bits = (total_bits + chunks - 1) / chunks;
    std::vector<Exit> guess(chunks * ncomp);
    parallel(
        chunks * ncomp,
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
        },
        threads);
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
        times += " (" + std::to_string(chunks) + " chunks, " + std::to_string(met_count) + " of " + std::to_string(chunks - 1) + " met, " +
                 std::to_string(lockstep) + " steps)";
    // -- Pass 3: each chunk decoded again from its true entry, its differences
    // written where they go (the first sample's place: the counts before it).
    std::vector<uint64_t> first(chunks + 1, 0);
    for (size_t k = 0; k < chunks; ++k)
        first[k + 1] = first[k] + exit[k].count;
    if (first[chunks] < samples)
        return error = "too few samples", false;
    bool bad = false;
    parallel(
        chunks,
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
        },
        threads);
    if (bad)
        return error = "corrupt data", false;

    mark("decoded");
    return true;
}

// LibRaw's prediction from the row above (ljpeg_row's), for predictors 2..7:
// `a` the sample to the left, `b` above, `c` above and to the left.
inline int predict(int predictor, int a, int b, int c)
{
    switch (predictor)
    {
        case 2:
            return b;
        case 3:
            return c;
        case 4:
            return a + b - c;
        case 5:
            return a + ((b - c) >> 1);
        case 6:
            return b + ((a - c) >> 1);
        case 7:
            return (a + b) >> 1;
        default:
            return a;
    }
}

// One stream's differences (its `height` rows) to samples. Each row's first
// samples continue down from those `step` rows above (the first rows' from
// where the stream says they start); the rest from their left, so the rows
// go in parallel.
// Arithmetic modulo 2^16, as the standard has it. A predictor from the row
// above (all but the first row's, LibRaw's way) goes a row after another.
void sum(const Prepared& p, int height, uint16_t* d, unsigned threads)
{
    const int ncomp = p.components;
    const size_t row = size_t(p.width) * ncomp;
    for (int c = 0; c < ncomp; ++c)
        for (int y = 0; y < height; ++y)
        {
            const uint16_t above = y < p.step ? p.initial[y][c] : d[(y - p.step) * row + c];
            d[y * row + c] = uint16_t(above + d[y * row + c]);
        }
    const auto left = [&](size_t y)
    {
        uint16_t* r = d + y * row;
        for (size_t x = ncomp; x < row; ++x)
            r[x] = uint16_t(r[x - ncomp] + r[x]);
    };
    if (p.predictor == 1)
    {
        parallel(size_t(height), left, threads);
        return;
    }
    // A row after another (a row needs the one above it all), each
    // predictor's loop its own.
    left(0);
    const auto rows = [&]<int P>()
    {
        for (size_t y = 1; y < size_t(height); ++y)
        {
            uint16_t* r = d + y * row;
            const uint16_t* u = r - row;
            int last[4];  // each component's sample to the left, kept at hand
            for (int c = 0; c < ncomp; ++c)
                last[c] = r[c];
            for (size_t x = ncomp; x < row; x += ncomp)
                for (int c = 0; c < ncomp; ++c)
                {
                    const uint16_t v = uint16_t(predict(P, last[c], u[x + c], u[x + c - ncomp]) + r[x + c]);
                    r[x + c] = v;
                    last[c] = v;
                }
        }
    };
    switch (p.predictor)
    {
        case 2:
            return rows.template operator()<2>();
        case 3:
            return rows.template operator()<3>();
        case 4:
            return rows.template operator()<4>();
        case 5:
            return rows.template operator()<5>();
        case 6:
            return rows.template operator()<6>();
        default:
            return rows.template operator()<7>();
    }
}

}  // namespace

bool decode(const Prepared& p, Image& out, std::string& error, unsigned threads)
{
    if (threads == 0)
        threads = std::max(1u, std::thread::hardware_concurrency());
    static const bool trace = std::getenv("IFRIT_TRACE") != nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    const int width = p.width, height = p.height, ncomp = p.components;
    if (!width || !height || ncomp < 1 || ncomp > 4 || p.parts.empty() || p.predictor < 1 || p.predictor > 7 || (!p.curve.empty() && p.curve.size() != 65536))
        return error = "not prepared", false;
    for (const Prepared::Part& part : p.parts)
    {
        if (part.start % 8 || part.start / 8 + (part.bits + 7) / 8 + 8 > p.bits.size() || part.rows < 1 || part.rows > height)
            return error = "not prepared", false;
        for (int c = 0; c < ncomp; ++c)
            if (part.table[c] >= p.tables.size())
                return error = "not prepared", false;
    }
    const size_t frame = size_t(width) * height * ncomp;
    out.width = width, out.height = height, out.components = ncomp, out.precision = p.precision;
    out.samples.assign(frame * p.parts.size(), 0);
    // One stream on every thread; a DNG's tiles side by side, a thread each.
    std::string times;
    const auto finish = [&](size_t k, unsigned on)
    {
        uint16_t* d = out.samples.data() + k * frame;
        sum(p, p.parts[k].rows, d, on);
        if (!p.curve.empty())
            for (size_t i = 0; i < frame; ++i)
                d[i] = p.curve[d[i]];
    };
    if (p.parts.size() == 1)
    {
        if (!decode_differences(p, p.parts[0], out.samples.data(), threads, error, times))
            return false;
        const auto t1 = std::chrono::steady_clock::now();
        finish(0, threads);
        if (trace)
            times += ", summed " + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t1).count() / 1000) +
                     " ms";
    }
    else
    {
        std::vector<std::string> errors(p.parts.size());
        std::vector<char> ok(p.parts.size(), 0);
        parallel(
            p.parts.size(),
            [&](size_t k)
            {
                std::string unused;
                if ((ok[k] = decode_differences(p, p.parts[k], out.samples.data() + k * frame, 1, errors[k], unused)))
                    finish(k, 1);
            },
            threads);
        for (size_t k = 0; k < ok.size(); ++k)
            if (!ok[k])
                return error = "tile " + std::to_string(k) + ": " + errors[k], false;
    }
    if (trace)
        std::fprintf(stderr, "ljpeg: %zu x %dx%d x%d, predictor %d, on %u threads in %.1f ms%s%s\n", p.parts.size(), width, height, ncomp, p.predictor, threads,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), times.empty() ? "" : ": ", times.c_str());
    return true;
}

}  // namespace ljpeg
