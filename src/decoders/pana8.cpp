#include "pana8.h"

#include "../bytes.h"
#include "../parallel.h"
#include "chunked.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace pana8
{
namespace
{

// LibRaw's gammaCurve (its gamma_base 0) at `v`: the segment it's in, by
// where each starts, then as that segment's shift has it: 31 the curve's
// end, 15 none (both LibRaw takes from the starts' high 16 bits, which the
// tag doesn't have), under 16 rounded down that many bits, from 16 up
// shifted up by the low four. Held to `top`.
uint32_t curve_at(const Header& h, uint32_t v)
{
    const uint16_t* start = h.curve_start;
    int s = 0;
    if (v >= start[1])
    {
        s = 1;
        if (v >= start[2])
        {
            s = 2;
            if (v >= start[3])
            {
                s = 3;
                if (v >= start[4])
                    s = v >= start[5] ? 5 : 4;
            }
        }
    }
    const uint32_t k = h.curve_shift[s], low = k & 31;
    uint32_t x = v - start[s];  // modulo 2^32, as LibRaw's
    if (low == 31)
        x = s == 5 ? 0xFFFF : 0;
    else if (!(k & 16))
        x = low == 15 ? 0 : low ? (x + (1u << (low - 1))) >> low : x;
    else
        x <<= k & 15;
    return std::min<uint32_t>(x, h.top);
}

// Each byte's bits the other way round: the stream's are taken from each
// byte's lowest up, and read here from the highest.
const std::array<uint8_t, 256> kReversed = []
{
    std::array<uint8_t, 256> r{};
    for (int b = 0; b < 256; ++b)
        for (int i = 0; i < 8; ++i)
            r[b] |= uint8_t((b >> i & 1) << (7 - i));
    return r;
}();

}  // namespace

bool prepare(const uint8_t* file, size_t size, const Header& h, int readout_w, int readout_h, Prepared& p, std::string& error)
{
    p = Prepared{};
    p.readout_w = readout_w, p.readout_h = readout_h;
    // What LibRaw's panasonicC8_load_raw checks, then what's not taken here.
    if (h.stripes < 1 || h.stripes > 5)
        return error = std::to_string(h.stripes) + " stripes", false;
    int across = 0;
    for (int i = 0; i < h.stripes; ++i)
    {
        Prepared::Stripe s;
        s.left = h.left[i], s.width = h.width[i], s.height = h.height[i];
        across += s.width;
        // LibRaw reads whole 8-byte words: past the file's end, what they'd hold isn't the file's.
        const uint64_t words = (uint64_t(h.bits[i]) + 63) / 64;
        if (s.height != readout_h || s.left + s.width > readout_w || h.offset[i] < 0 || uint64_t(h.offset[i]) > size ||
            words * 8 > size - uint64_t(h.offset[i]))
            return error = "a stripe outside the readout or the file", false;
        if (s.width % 2 || s.height % 2 || !s.width || !s.height || words < 2)
            return error = "a stripe of odd size", false;
        s.data = file + h.offset[i];
        s.bits = words * 64;
        p.stripes.push_back(s);
    }
    if (across != readout_w)
        return error = "stripes not the readout's width", false;
    // Each symbol's code as LibRaw matches it: a 16-bit mask moved to the
    // top, by its length (x86's shift: by 64 not at all).
    uint64_t mask[17], match[17];
    unsigned longest = 0;
    for (int i = 0; i < 17; ++i)
    {
        const unsigned len = h.code_length[i] & 31, at = (64 - len) & 63;
        if ((h.shift[i] & 0xFF) > 31)
            return error = "a shift past 31", false;
        longest = std::max(longest, len);
        mask[i] = uint64_t(0xFFFF) << at;
        match[i] = uint64_t(h.code[i] & ((1u << len) - 1)) << at;
        p.coeff[i] = uint32_t(h.shift[i]) << 24 | uint32_t(h.code_length[i]) << 16 | h.code[i];
    }
    if (longest > 16)
        return error = "codes longer than 16 bits", false;
    // The first that matches, the last's mismatch symbol 17 (LibRaw's GetDBit).
    p.symbol.resize(65536);
    for (uint32_t j = 0; j < 65536; ++j)
    {
        const uint64_t bits = uint64_t(j) << 48;
        int i = 0;
        while (i < 16 && (bits & mask[i]) != match[i])
            ++i;
        p.symbol[j] = uint8_t(i < 16 ? i : ((bits & mask[16]) == match[16]) ^ 0x11);
    }
    p.top = h.top;
    std::copy_n(h.initial, 4, p.initial);
    for (uint32_t v = 0; v < 65536; ++v)
        if (curve_at(h, v) != v)
        {
            p.curve.resize(65536);
            for (uint32_t w = 0; w < 65536; ++w)
                p.curve[w] = uint16_t(curve_at(h, w));
            break;
        }
    return true;
}

bool decode(const Prepared& p, std::vector<uint16_t>& readout, std::string& error, unsigned threads)
{
    if (threads == 0)
        threads = std::max(1u, std::thread::hardware_concurrency());
    static const bool trace = std::getenv("IFRIT_TRACE") != nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    std::string times;
    const auto mark = [&](const char* what)
    {
        if (!trace)
            return;
        const auto now = std::chrono::steady_clock::now();
        times += std::string(", ") + what + " " + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now - last).count() / 1000) + " ms";
        last = now;
    };
    readout.resize(size_t(p.readout_w) * p.readout_h);  // every photosite a stripe's
    mark("readout");
    // A stripe's bits (then a word of zeros, for reading 64 bits anywhere
    // in it) and differences, the largest's, made once.
    size_t most_bytes = 0, most_samples = 0;
    for (const Prepared::Stripe& s : p.stripes)
        most_bytes = std::max(most_bytes, size_t(s.bits / 8)), most_samples = std::max(most_samples, size_t(s.width) * s.height);
    // (Not cleared: each written on every core before it's read.)
    const std::unique_ptr<uint8_t[]> bits(new uint8_t[most_bytes + 8]);
    const std::unique_ptr<int32_t[]> d(new int32_t[most_samples]);
    mark("made");
    for (size_t k = 0; k < p.stripes.size(); ++k)
    {
        const Prepared::Stripe& s = p.stripes[k];
        const size_t bytes = size_t(s.bits / 8);
        parallel(bytes, [&](size_t i) { bits[i] = kReversed[s.data[i]]; }, threads);
        std::fill_n(bits.get() + bytes, 8, 0);
        mark("turned");
        // A sample's difference as DecodeC8 makes it: the code's symbol h
        // says how many of its bits the stream holds (h less its shift),
        // and how many it dropped at the bottom (the shift: half their range
        // added back). The first bit after the code is its sign whether it
        // holds any or not; LibRaw keeps 16 bits of what it holds.
        const auto next = [&](uint64_t& pos, int, int& diff)
        {
            const uint64_t w = be64(bits.get() + (pos >> 3)) << (pos & 7);
            const int h = p.symbol[w >> 48];
            const uint32_t coeff = p.coeff[h];
            const int len = int(coeff >> 16 & 31), shift = int(coeff >> 24 & 31), held = h - shift;
            const uint64_t after = w << len;
            const int value = held > 0 ? uint16_t((after >> (64 - held)) << shift) : 0;
            int delta = int64_t(after) < 0 ? value : h ? value - (1 << h) + (shift ? 0 : 1) : 0;
            if (shift)
                delta += 1 << (shift - 1);
            pos += uint64_t(len + std::max(held, 0));
            diff = delta;
            return true;
        };
        // Row pairs of groups of four: rows 2r + (q & 1), column 2g + (q >> 1).
        const size_t pairs = size_t(s.height) / 2, groups = size_t(s.width) / 2, pair = groups * 4;
        std::string stripe_times;
        if (!chunked::decode(next, s.bits, 1, pairs * pair, d.get(), threads, error, stripe_times))
            return error = "stripe " + std::to_string(k) + ": " + error, false;
        times += "; " + stripe_times;
        last = std::chrono::steady_clock::now();
        // Each sample from the same place in the group before, held to
        // 0..top; the first group's from the pair above's first, a pair
        // after another (the first pair's from `initial`, LibRaw's
        // coefficients in the order they sit in its group: q 1 and 2 swapped).
        const auto held = [&](int32_t base, int32_t delta) { return std::clamp(base + delta, 0, p.top); };
        std::vector<int32_t> first(pairs * 4);
        int32_t base[4];
        for (int q = 0; q < 4; ++q)
            base[q] = p.initial[(q & 1) * 2 + (q >> 1)];
        for (size_t r = 0; r < pairs; ++r)
            for (int q = 0; q < 4; ++q)
                first[r * 4 + q] = base[q] = held(base[q], d[r * pair + q]);
        parallel(
            pairs,
            [&](size_t r)
            {
                int32_t b[4];
                std::copy_n(first.data() + r * 4, 4, b);
                uint16_t* rows[2] = {readout.data() + 2 * r * p.readout_w + s.left, readout.data() + (2 * r + 1) * p.readout_w + s.left};
                const int32_t* from = d.get() + r * pair;
                for (size_t g = 0; g < groups; ++g)
                    for (int q = 0; q < 4; ++q)
                    {
                        if (g)
                            b[q] = held(b[q], from[g * 4 + q]);
                        rows[q & 1][2 * g + (q >> 1)] = p.curve.empty() ? uint16_t(b[q]) : p.curve[b[q]];
                    }
            },
            threads);
        mark("summed");
    }
    if (trace)
        std::fprintf(stderr, "pana8: %zu stripes on %u threads in %.1f ms%s\n", p.stripes.size(), threads,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), times.c_str());
    return true;
}

}  // namespace pana8
