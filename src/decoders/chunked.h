#pragma once

// One serial stream of variable-length codes (a sample each) decoded on
// several threads: the codes resynchronise, so the stream is cut into
// chunks, each decoded from its start as if a code began there; the true
// decode, followed into each chunk from the one before, meets that guess on
// a code boundary within a few codes, and from there the guess was right.
// Then each chunk is decoded again from its true start, its samples written
// where they go. Lossless JPEG's streams (ljpeg.cpp) and Panasonic's
// stripes (pana8.cpp) are decoded so; the GPU's passes (ljpeg.hlsli) are
// the same.
//
// A code's meaning may depend on the component due (each its own table),
// which follows from the one before: `ncomp` of them in turn. `next(pos, c,
// diff)` decodes the code at bit `pos` for component `c` into `diff` and
// moves `pos` past it; false for a code that doesn't exist (a speculative
// decode takes that as a one-bit code and goes on: a deterministic path all
// the same).

#include "../parallel.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace chunked
{

// Where a stretch of the stream ends: the first code boundary at or past its
// end, the component due there, and how many samples it decoded.
struct Exit
{
    uint64_t pos = 0;
    int component = 0;
    uint64_t count = 0;
};

// The stream's first `samples` codes (`total_bits` of it at most) into `d`
// (16 bits each, or 32), on `threads` (one: start to end on this thread).
// `times` (IFRIT_TRACE): how long each pass took.
template <typename Next, typename Sample>
bool decode(const Next& next, uint64_t total_bits, int ncomp, uint64_t samples, Sample* d, unsigned threads, std::string& error, std::string& times)
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
    if (ncomp < 1 || ncomp > 8)
        return error = "not prepared", false;
    const auto following = [ncomp](int c) { return c + 1 == ncomp ? 0 : c + 1; };
    int diff;

    // -- Pass 1: each chunk decoded from its start as if a code began there,
    // once for each component that could be due (each has its own table), to
    // past its end.
    const size_t chunks = threads <= 1 ? 1 : std::clamp<size_t>(size_t(threads) * 4, 1, std::max<uint64_t>(1, total_bits / 4096));
    if (chunks == 1)
    {
        uint64_t pos = 0;
        for (uint64_t j = 0, c = 0; j < samples; ++j, c = following(int(c)))
        {
            if (pos >= total_bits)
                return error = "too few samples", false;
            if (!next(pos, int(c), diff))
                return error = "corrupt data", false;
            d[j] = Sample(diff);
        }
        return true;
    }
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
                next(pos, c, diff);
                c = following(c);
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
        int tc = entry[k].component;
        uint64_t sp[8], b[8];  // each guess (ncomp at most 8)
        int sc[8];
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
                    next(sp[g], sc[g], diff);
                    sc[g] = following(sc[g]);
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
            if (!next(tp, tc, diff))
                return error = "corrupt data", false;
            tc = following(tc);
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
                if (!next(tp, tc, diff))
                    return error = "corrupt data", false;
                tc = following(tc);
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
            Sample* o = d + first[k];
            for (uint64_t j = 0; j < n; ++j)
            {
                if (!next(pos, c, diff))
                {
                    bad = true;
                    return;
                }
                o[j] = Sample(diff);
                c = following(c);
            }
        },
        threads);
    if (bad)
        return error = "corrupt data", false;
    mark("decoded");
    return true;
}

}  // namespace chunked
