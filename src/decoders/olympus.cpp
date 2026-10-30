#include "olympus.h"

#include "../bytes.h"
#include "../platform.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace olympus
{
namespace
{

// The tags as olympus_load_raw takes them.
struct Tags
{
    int32_t t640, t641, t642, t643, t644, t645, t646, t647, t648, t649, t650, t651, t652;
    explicit Tags(const unsigned t[14])
        : t640(int32_t(t[0])), t641(int32_t(1u << t[1])), t642(int32_t(t[2])), t643(int32_t(t[3])), t644(int32_t(t[4])),
          t645(int32_t(1u << (t[5] & 31))), t646(int32_t(t[6])), t647(int32_t(t[7])), t648(int32_t(t[8])), t649(int32_t(t[9])),
          t650(int32_t(t[10])), t651(int32_t(t[11])), t652(std::max(1, int32_t(t[12])))
    {
    }
};

// A row's codes: each photosite's difference from its prediction into `g`,
// and its bits under the prediction's (tag 0x640's many) into `low`. Each
// code's width from the codes two back (the same colour): the first two
// columns' fixed, so a row needs nothing from the one before. False for a
// code LibRaw stops at.
bool read_row(const Tags& t, int width, MsbBits& bits, int32_t* g, uint16_t* low)
{
    MsbBits b = bits;  // in registers, not where the rows written might be
    const Tags tags = t;
    // A code at most this long: one fill a code, its bits all held.
    const bool once = 1 + tags.t643 + tags.t640 + tags.t652 + 16 <= 56;
    // Two columns back and one back (the same colour, the other): the code,
    // how many small codes in a row, the part carried for tag 0x643's.
    int32_t code2 = 0, code1 = 0, run2 = 0, run1 = 0, carry2 = 0, carry1 = 0;
    for (int col = 0; col < width; ++col)
    {
        int32_t wbits = tags.t649, run = 0;
        if (col > 1)
        {
            run = tags.t645 >= code2 ? run2 + 1 : 0;
            const bool many = run >= tags.t646;
            wbits = (many ? tags.t647 : tags.t649) + std::max(int32_t(std::bit_width(uint32_t(code2) & 0xffff)) - (many ? tags.t648 : tags.t650), -1) + 1;
        }
        wbits = std::min(wbits, tags.t651);
        if (wbits >= 16)
            return false;
        b.fill();
        const uint32_t sign = b.take(1);
        const uint32_t under = b.take(tags.t643);
        low[col] = uint16_t(b.take(tags.t640));
        // The high bits: a run of zeros ended by a one; that many zeros
        // without one, the high bits as they are (15 less the low ones),
        // then a bit passed over. (Past what's in hand are zeros, but
        // there's more than tag 0x652's many.)
        if (!once)
            b.fill();
        const int32_t zeros = std::countl_zero(b.peek());
        uint32_t high;
        if (zeros < tags.t652)
        {
            high = uint32_t(zeros);
            b.skip(zeros + 1);
        }
        else
        {
            b.skip(tags.t652);
            if (!once)
                b.fill();
            high = b.take(15 - wbits);
            b.skip(1);
        }
        if (!once)
            b.fill();
        const uint32_t whole = b.take(wbits) | high << wbits;
        int32_t diff = int32_t(whole ^ (0u - sign)), part = 0;
        if (tags.t643)
        {
            const int32_t v = col < 2 ? 0 : carry2;
            if (tags.t643 == 1)
            {
                diff = int32_t(under + 2 * uint32_t(v + diff));
                part = v + (diff >> 1);
            }
            else
            {
                diff = int32_t(under + 4 * uint32_t(v + diff));
                part = int32_t((uint32_t(diff >> 1) & 0xFFFFFFFE) + uint32_t(v) + uint32_t(diff >> 2));
            }
        }
        code2 = code1, code1 = int32_t(whole);
        run2 = run1, run1 = run;
        carry2 = carry1, carry1 = tags.t644 == 15 ? 0 : part >> (tags.t644 & 31);
        g[col] = diff;
    }
    bits = b;
    return true;
}

// A row's photosites from its differences, columns `from` to `to`: each
// predicted from the one two columns back (`left`: those two columns'
// predictions plus differences, before any truncating), two rows up (`up`,
// none for the first two rows) and two up and back. olympus_load_raw's
// choices come to LOCO-I's median: the lesser of the two beside it if the
// one diagonal is past both, the greater if it's short of both; between
// them, their mean where all three are near, else the gradient. Chosen
// without branching: the photosites are noise to a branch predictor.
void predict_row(const Tags& t, int from, int to, const int32_t* g, const uint16_t* low, const uint16_t* up, uint16_t* out, int32_t left[2])
{
    const int32_t shift = t.t640, near = t.t641, none = t.t642;
    int32_t back2 = left[from & 1], back1 = left[(from + 1) & 1];  // two columns back, one back
    for (int col = from; col < to; ++col)
    {
        const int32_t W = col < 2 ? none : back2;
        const int32_t N = up ? up[col] >> shift : none;
        const int32_t NW = up && col >= 2 ? up[col - 2] >> shift : none;
        const int32_t lo = std::min(N, W), hi = std::max(N, W);
        const int32_t smooth = std::abs(N - NW) <= near && std::abs(W - NW) <= near ? (N + W) >> 1 : N + W - NW;
        const int32_t pred = NW >= hi ? lo : NW <= lo ? hi : smooth;
        back2 = back1;
        back1 = g[col] + pred;
        out[col] = uint16_t(low[col] + (uint32_t(back1) << shift));
    }
    left[to & 1] = back2, left[(to + 1) & 1] = back1;
}

}  // namespace

bool prepare(const uint8_t* file, size_t size, uint64_t offset, int width, int height, const unsigned tag[14], Prepared& p, std::string& error)
{
    // checkhdr's: tag 0x653's many bytes reading 1, then two zeros.
    const unsigned header = tag[13];
    if (header == 0 || header > 8 || offset > size || size - offset < header + 2)
        return error = "no Olympus header", false;
    uint64_t one = 0;
    for (unsigned k = 0; k < header; ++k)
        one = one << 8 | file[offset + k];
    if (one != 1 || file[offset + header] || file[offset + header + 1])
        return error = "no Olympus header", false;
    if (width < 1 || height < 1 || tag[0] > 15 || tag[1] > 30 || tag[3] > 16 || tag[12] > 56)
        return error = "Olympus tags out of range", false;
    p.data = file + offset + header + 2;
    p.size = size_t(size - offset - header - 2);
    p.width = width, p.height = height;
    std::copy_n(tag, 14, p.tag);
    return true;
}

bool decode(const Prepared& p, std::vector<uint16_t>& readout, std::string& error)
{
    const Tags t(p.tag);
    const int w = p.width, h = p.height;
    readout.resize(size_t(w) * h);
    // The codes read here into a ring of rows, and predicted on other
    // threads a row each in turn, a few columns at a time, each row behind
    // the one two up. Predicting a row takes half as long as reading its
    // codes: three keep up with room to spare.
    constexpr int kRing = 64, kColumns = 256;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<int32_t> diffs(size_t(kRing) * w);
    std::vector<uint16_t> lows(size_t(kRing) * w);
    std::atomic<int> read{0};                     // rows whose codes are in the ring
    std::vector<std::atomic<int>> predicted(h);  // each row's columns predicted
    const auto until = [](std::atomic<int>& a, int least)
    {
        // A while yielding (it's mostly a moment), then asleep.
        int at = a.load(std::memory_order_acquire);
        for (int spins = 0; at < least && spins < 4096; ++spins, at = a.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (; at < least; at = a.load(std::memory_order_acquire))
            a.wait(at, std::memory_order_acquire);
    };
    const unsigned n = std::clamp(std::thread::hardware_concurrency(), 2u, 4u) - 1;
    std::vector<std::thread> pool;
    for (unsigned j = 0; j < n; ++j)
        pool.emplace_back(
            [&, j]
            {
                for (int row = int(j); row < h; row += int(n))
                {
                    until(read, row + 1);
                    const size_t slot = size_t(row % kRing) * w;
                    const uint16_t* up = row < 2 ? nullptr : readout.data() + size_t(row - 2) * w;
                    int32_t left[2] = {};
                    for (int from = 0; from < w; from += kColumns)
                    {
                        const int to = std::min(w, from + kColumns);
                        if (up)
                            until(predicted[row - 2], to);
                        predict_row(t, from, to, diffs.data() + slot, lows.data() + slot, up, readout.data() + size_t(row) * w, left);
                        predicted[row].store(to, std::memory_order_release);
                        predicted[row].notify_all();
                    }
                }
            });
    MsbBits b(p.data, p.size);
    bool ok = true;  // past a code LibRaw stops at, rows of nothing
    for (int row = 0; row < h; ++row)
    {
        if (row >= kRing)
            until(predicted[row - kRing], w);  // its slot free
        const size_t slot = size_t(row % kRing) * w;
        if (ok)
            ok = read_row(t, w, b, diffs.data() + slot, lows.data() + slot);
        if (!ok)
        {
            std::fill_n(diffs.data() + slot, w, 0);
            std::fill_n(lows.data() + slot, w, 0);
        }
        read.store(row + 1, std::memory_order_release);
        read.notify_all();
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (std::thread& th : pool)
        th.join();
    if (platform::tracing())
        platform::trace("olympus: %dx%d, its codes read in %.0f ms, predicted on %u threads by %.0f ms", w, h,
                        std::chrono::duration<double, std::milli>(t1 - t0).count(), n,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    if (!ok)
        return error = "a code too wide", false;
    return true;
}

}  // namespace olympus
