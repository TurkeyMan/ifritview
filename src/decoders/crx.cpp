#include "crx.h"

#include "../parallel.h"
#include "bits.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

namespace crx
{
namespace
{

enum : int
{
    kRight = 1,
    kLeft = 2,
    kBelow = 4,
    kAbove = 8,
};

// LibRaw's exCoefNumTbl: how many samples a subband reaches past its tile's
// edge into the next, by levels, the tile's size mod 8 and level.
constexpr int8_t kReach[144] = {1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
                                1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 2, 2, 1, 0, 0, 1, 1, 1, 1, 0, 0,
                                1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 2, 2, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0,
                                1, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 2, 2, 1, 1, 0, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
// Quantisation steps, a sixth of an octave apart (q_step_tbl).
constexpr int32_t kSteps[6] = {0x28, 0x2D, 0x33, 0x39, 0x40, 0x48};
// A run's length: what each further 1 adds, and the bits after the 0 (JS, J).
constexpr int32_t kRunStep[32] = {1,    1,    1,    1,    2,    2,    2,    2,    4,     4,     4,     4,     8,      8,      8,      8,
                                  0x10, 0x10, 0x20, 0x20, 0x40, 0x40, 0x80, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000};
constexpr int kRunBits[32] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// A Golomb-Rice code with parameter k: its zeros, then k bits under them;
// `escape` bits outright for `limit` zeros or more.
uint32_t code(Bits& b, int k, uint32_t limit = 41, int escape = 21)
{
    if (uint32_t c; b.golomb(k, limit, c))
        return c;
    const uint32_t z = b.zeros();
    if (z >= limit)
        return b.bits(escape);
    return k ? b.bits(k) | z << k : z;
}

int32_t signed_of(uint32_t c)
{
    return -int32_t(c & 1) ^ int32_t(c >> 1);
}

// The parameter after a code (crxPredictKParameter); `most` 0: no limit.
int next_k(int k, int32_t c, int most = 0)
{
    const int n = k - (c < (1 << k >> 1)) + ((c >> k) > 2) + ((c >> k) > 5);
    return !most || n < most ? n : most;
}

// A subband's lines decoded in order (crxDecodeLine and the functions it
// calls): each sample predicted from its left and the line above (or, not
// `partial`, the code's parameter from those), runs of repeats (or zeros)
// coded as their lengths.
class Lines
{
public:
    Lines(const uint8_t* data, uint64_t size, int width, int height, bool partial, int rounded)
        : bits(data, size), width_(width), height_(height), partial_(partial), rounded_(rounded), buf_(size_t(2 * width + 4) + (partial ? 0 : size_t(width)))
    {
        if (rounded_ & ~1)
            while (rounded_ >> rounded_bits_)
                ++rounded_bits_;
    }
    // The next line into `out` (`width` samples). False past the last, or for a run past the line.
    bool next(int32_t* out)
    {
        if (line_ >= height_)
            return false;
        const int len = width_ + 2;
        int32_t* a = buf_.data();
        int32_t* b = a + len;
        p0_ = line_ & 1 ? b : a;  // the line above
        p1_ = line_ & 1 ? a : b;
        p2_ = a + 2 * len;
        const int32_t* first = p1_ + 1;
        bool ok;
        if (line_ == 0)
        {
            s_ = k_ = 0;
            ok = !partial_ ? top_no_ref() : rounded_ <= 0 ? top() : top_rounded();
        }
        else
            ok = !partial_ ? line_no_ref() : rounded_ <= 0 ? line() : line_rounded();
        if (!ok)
            return false;
        std::memcpy(out, first, size_t(width_) * 4);
        ++line_;
        return true;
    }
    Bits bits;

private:
    // How many repeat, at most `length` (-1: past it).
    int run(int length)
    {
        if (!bits.bits(1))
            return 0;
        int n = 1;
        while (bits.bits(1))
        {
            n += kRunStep[s_];
            if (n > length)
            {
                n = length;
                break;
            }
            if (s_ < 31)
                ++s_;
            if (n == length)
                break;
        }
        if (n < length)
        {
            if (kRunBits[s_])
                n += int(bits.bits(kRunBits[s_]));
            if (s_ > 0)
                --s_;
            if (n > length)
                return -1;
        }
        return n;
    }
    // A sample: predicted (`median`: from its left, above and above-left, as
    // LOCO-I's median), and, not at the line's end, the parameter from the
    // gradient ahead too (crxDecodeSymbolL1).
    void symbol(bool median, bool more)
    {
        if (median)
        {
            const int32_t delta = p0_[1] - p0_[0];
            const int32_t s[4] = {delta + p1_[0], delta + p1_[0], p1_[0], p0_[1]};
            p1_[1] = s[(((p0_[0] < p1_[0]) ^ (delta < 0)) << 1) + ((p1_[0] < p0_[1]) ^ (delta < 0))];
        }
        else
            p1_[1] = p0_[1];
        uint32_t c = code(bits, k_);
        p1_[1] += signed_of(c);
        if (more)
        {
            const int32_t ahead = (p0_[2] - p0_[1]) << 1;
            c = (c + uint32_t(std::abs(ahead))) >> 1;
            ++p0_;
        }
        k_ = next_k(k_, int32_t(c), 15);
        ++p1_;
    }
    // The same with the least bits dropped (crxDecodeSymbolL1Rounded).
    void symbol_rounded(bool predict = true, bool more = true)
    {
        int32_t sym = p0_[1];
        if (predict)
        {
            const int32_t delta = p0_[1] - p0_[0];
            const int32_t s[4] = {delta + p1_[0], delta + p1_[0], p1_[0], p0_[1]};
            sym = s[(((p0_[0] < p1_[0]) ^ (delta < 0)) << 1) + ((p1_[0] < p0_[1]) ^ (delta < 0))];
        }
        const uint32_t c = code(bits, k_);
        int32_t v = signed_of(c);
        p1_[1] = rounded_ * 2 * v + (v >> 31) + sym;
        if (more)
        {
            if (p0_[2] > p0_[1])
                v = (p0_[2] - p0_[1] + rounded_ - 1) >> rounded_bits_;
            else
                v = -((p0_[1] - p0_[2] + rounded_) >> rounded_bits_);
            k_ = next_k(k_, int32_t((c + 2 * uint32_t(std::abs(v))) >> 1), 15);
        }
        else
            k_ = next_k(k_, int32_t(c), 15);
        ++p1_;
    }
    // Repeats of the sample to the left: `n` of them.
    void repeat(int n)
    {
        for (; n > 0; --n, ++p1_)
            p1_[1] = p1_[0];
    }

    bool top()  // crxDecodeTopLine
    {
        p1_[0] = 0;
        int length = width_;
        for (; length > 1; --length)
        {
            if (p1_[0])
                p1_[1] = p1_[0];
            else
            {
                const int n = run(length);
                if (n < 0)
                    return false;
                length -= n;
                repeat(n);
                if (length <= 0)
                    break;
                p1_[1] = 0;
            }
            const uint32_t c = code(bits, k_);
            p1_[1] += signed_of(c);
            k_ = next_k(k_, int32_t(c), 15);
            ++p1_;
        }
        if (length == 1)
        {
            p1_[1] = p1_[0];
            const uint32_t c = code(bits, k_);
            p1_[1] += signed_of(c);
            k_ = next_k(k_, int32_t(c), 15);
            ++p1_;
        }
        p1_[1] = p1_[0] + 1;
        return true;
    }
    bool top_rounded()  // crxDecodeTopLineRounded
    {
        p1_[0] = 0;
        int length = width_;
        for (; length > 1; --length)
        {
            if (std::abs(p1_[0]) > rounded_)
                p1_[1] = p1_[0];
            else
            {
                const int n = run(length);
                if (n < 0)
                    return false;
                length -= n;
                repeat(n);
                if (length <= 0)
                    break;
                p1_[1] = 0;
            }
            const uint32_t c = code(bits, k_);
            const int32_t v = signed_of(c);
            p1_[1] += rounded_ * 2 * v + (v >> 31);
            k_ = next_k(k_, int32_t(c), 15);
            ++p1_;
        }
        if (length == 1)
        {
            const uint32_t c = code(bits, k_);
            const int32_t v = signed_of(c);
            p1_[1] += rounded_ * 2 * v + (v >> 31);
            k_ = next_k(k_, int32_t(c), 15);
            ++p1_;
        }
        p1_[1] = p1_[0] + 1;
        return true;
    }
    bool top_no_ref()  // crxDecodeTopLineNoRefPrevLine
    {
        p0_[0] = 0;
        p1_[0] = 0;
        int length = width_;
        for (; length > 1; --length)
        {
            if (p1_[0])
            {
                const uint32_t c = code(bits, k_);
                p1_[1] = signed_of(c);
                k_ = next_k(k_, int32_t(c), 15);
            }
            else
            {
                const int n = run(length);
                if (n < 0)
                    return false;
                length -= n;
                for (int i = 0; i < n; ++i, ++p1_, ++p2_)
                {
                    p2_[0] = 0;
                    p1_[1] = 0;
                }
                if (length <= 0)
                    break;
                const uint32_t c = code(bits, k_);
                p1_[1] = signed_of(c + 1);
                k_ = next_k(k_, int32_t(c), 15);
            }
            p2_[0] = k_;
            ++p2_;
            ++p1_;
        }
        if (length == 1)
        {
            const uint32_t c = code(bits, k_);
            p1_[1] = signed_of(c);
            k_ = next_k(k_, int32_t(c), 15);
            p2_[0] = k_;
            ++p1_;
        }
        p1_[1] = 0;
        return true;
    }
    bool line()  // crxDecodeLine
    {
        int length = width_;
        p1_[0] = p0_[1];
        for (; length > 1; --length)
        {
            if (p1_[0] != p0_[1] || p1_[0] != p0_[2])
                symbol(true, true);
            else
            {
                const int n = run(length);
                if (n < 0)
                    return false;
                length -= n;
                p0_ += n;
                repeat(n);
                if (length > 0)
                    symbol(false, length > 1);
            }
        }
        if (length == 1)
            symbol(true, false);
        p1_[1] = p1_[0] + 1;
        return true;
    }
    bool line_rounded()  // crxDecodeLineRounded
    {
        bool reached = false;
        p0_[0] = p0_[1];
        p1_[0] = p0_[1];
        int length = width_;
        for (; length > 1; --length)
        {
            if (std::abs(p0_[2] - p0_[1]) > rounded_)
            {
                symbol_rounded();
                ++p0_;
                reached = true;
            }
            else if (reached || std::abs(p0_[0] - p1_[0]) > rounded_)
            {
                symbol_rounded();
                ++p0_;
                reached = false;
            }
            else
            {
                const int n = run(length);
                if (n < 0)
                    return false;
                length -= n;
                p0_ += n;
                repeat(n);
                if (length > 1)
                {
                    symbol_rounded(false);
                    ++p0_;
                    reached = std::abs(p0_[1] - p0_[0]) > rounded_;
                }
                else if (length == 1)
                    symbol_rounded(false, false);
            }
        }
        if (length == 1)
            symbol_rounded(true, false);
        p1_[1] = p1_[0] + 1;
        return true;
    }
    // A sample of a line after the first (not `partial`; after a run, never
    // 0: `bias` 1), its parameter from the one above's (crxDecodeLineNoRefPrevLine's).
    void no_ref(int i, uint32_t bias = 0)
    {
        const uint32_t c = code(bits, k_);
        p1_[i + 1] = signed_of(c + bias);
        k_ = next_k(k_, int32_t(c));
        if (p2_[i + 1] - k_ <= 1)
        {
            if (k_ >= 15)
                k_ = 15;
        }
        else
            ++k_;
    }
    bool line_no_ref()  // crxDecodeLineNoRefPrevLine
    {
        int i = 0;
        for (; i < width_ - 1; ++i)
        {
            if (p0_[i + 2] | p0_[i + 1] | p1_[i])
                no_ref(i);
            else
            {
                const int n = run(width_ - i);
                if (n < 0)
                    return false;
                if (n > 0)
                {
                    std::memset(p1_ + i + 1, 0, size_t(n) * 4);
                    std::memset(p2_ + i, 0, size_t(n) * 4);
                    i += n;
                }
                if (i >= width_ - 1)
                {
                    if (i == width_ - 1)
                    {
                        const uint32_t c = code(bits, k_);
                        p1_[i + 1] = signed_of(c + 1);
                        k_ = next_k(k_, int32_t(c), 15);
                        p2_[i] = k_;
                    }
                    continue;
                }
                no_ref(i, 1);
            }
            p2_[i] = k_;
        }
        if (i == width_ - 1)
        {
            const uint32_t c = code(bits, k_);
            p1_[i + 1] = signed_of(c);
            k_ = next_k(k_, int32_t(c), 15);
            p2_[i] = k_;
        }
        return true;
    }

    int width_, height_;
    bool partial_;
    int rounded_, rounded_bits_ = 1;
    std::vector<int32_t> buf_;  // two lines (`width` + 2 each: the one above, this one); not partial, each sample's parameter
    int32_t *p0_ = nullptr, *p1_ = nullptr, *p2_ = nullptr;
    int line_ = 0;
    int s_ = 0, k_ = 0;
};

// A tile's quantisation steps (version 0x200), a table a level, coarsest first.
struct Steps
{
    int width = 0, height[3] = {};
    std::vector<uint32_t> table[3];
};

uint32_t step_of(int32_t q)
{
    return q / 6 >= 6 ? uint32_t(kSteps[q % 6]) << ((q / 6 - 6) & 0x1f) : uint32_t(kSteps[q % 6] >> (6 - q / 6));
}

// The steps of every 8 x 2 samples of the tile, each predicted from its left
// and above as the samples are; then the coarser levels', averaged (crxMakeQStep).
bool decode_steps(const Prepared& p, const Tile& t, Steps& s, std::string& error)
{
    const int levels = p.header.levels;
    if (levels < 1 || levels > 3)
        return error = "quantisation steps without a wavelet", false;
    const int w = (t.width >> 3) + ((t.width & 7) != 0), h = (t.height >> 1) + (t.height & 1);
    const int h4 = (t.height >> 2) + ((t.height & 3) != 0), h8 = (t.height >> 3) + ((t.height & 7) != 0);
    std::vector<int32_t> q(size_t(w) * h), lines(2 * size_t(w + 2));
    Bits b(p.file + t.steps_offset, t.steps_size);
    int k = 0;
    for (int row = 0; row < h; ++row)
    {
        int32_t* l0 = lines.data() + (row & 1 ? w + 2 : 0);
        int32_t* l1 = lines.data() + (row & 1 ? 0 : w + 2);
        int32_t* o = l1 + 1;
        if (row == 0)  // crxDecodeGolombTop
        {
            l1[0] = 0;
            for (int n = w; n-- > 0; ++l1)
            {
                l1[1] = l1[0];
                const uint32_t c = code(b, k, 23, 8);
                l1[1] += signed_of(c);
                k = next_k(k, int32_t(c), 7);
            }
        }
        else  // crxDecodeGolombNormal
        {
            l1[0] = l0[1];
            int32_t dh = l0[1] - l0[0];
            for (int n = w; n-- > 0; ++l1)
            {
                // crxPrediction: from the left, above and the gradients.
                const int32_t left = l1[0], top = l0[1], dv = l0[0] - l1[0];
                const int32_t s4[4] = {left + dh, left + dh, left, top};
                l1[1] = s4[(((dv < 0) ^ (dh < 0)) << 1) + ((left < top) ^ (dh < 0))];
                const uint32_t c = code(b, k, 23, 8);
                l1[1] += signed_of(c);
                if (n)
                {
                    dh = l0[2] - l0[1];
                    k = next_k(k, int32_t((c + 2 * uint32_t(std::abs(dh))) >> 1), 7);
                    ++l0;
                }
                else
                    k = next_k(k, int32_t(c), 7);
            }
        }
        l1[1] = l1[0] + 1;
        for (int col = 0; col < w; ++col)
            q[size_t(row) * w + col] = o[col] + 4;
    }
    s.width = w;
    // Level 0 the coarsest: the steps of 8 x 8 (three levels) or 8 x 4 (two) samples, then 8 x 2.
    int level = 0;
    const auto add = [&](int height, auto average) -> bool
    {
        s.height[level] = height;
        std::vector<uint32_t>& out = s.table[level++];
        out.resize(size_t(w) * height);
        for (int row = 0; row < height; ++row)
            for (int col = 0; col < w; ++col)
            {
                const int32_t v = average(row, col);
                if (v < 0)
                    return error = "a quantisation step out of range", false;
                out[size_t(row) * w + col] = step_of(v);
            }
        return true;
    };
    const auto at = [&](int row, int col) { return q[size_t(std::min(row, h - 1)) * w + col]; };
    if (levels == 3 && !add(h8,
                            [&](int r, int c)
                            {
                                const int32_t v = at(4 * r, c) + at(4 * r + 1, c) + at(4 * r + 2, c) + at(4 * r + 3, c);
                                return ((v < 0) * 3 + v) >> 2;
                            }))
        return false;
    if (levels >= 2 && !add(h4, [&](int r, int c) { return (at(2 * r, c) + at(2 * r + 1, c)) / 2; }))
        return false;
    return add(h, [&](int r, int c) { return at(r, c); });
}

// A subband decoded whole (its lines as LibRaw's crxDecodeLineWithIQuantization
// gives them, the steps applied); none for one without data, all zeros.
struct Coefficients
{
    int width = 0, height = 0;
    std::vector<int32_t> values;
};

bool decode_subband(const Prepared& p, const Subband& sb, bool first, const Tile::Plane& plane, const Steps* steps, int level, Coefficients& out,
                    std::string& error)
{
    out.width = sb.width, out.height = sb.height;
    if (!sb.size)
        return true;
    out.values.resize(size_t(sb.width) * sb.height);
    const bool partial = plane.partial && first;
    Lines lines(p.file + sb.offset, sb.size, sb.width, sb.height, partial, partial ? plane.rounded : 0);
    int q = sb.q, kq = 0;
    for (int row = 0; row < sb.height; ++row)
    {
        int32_t* o = out.values.data() + size_t(row) * sb.width;
        if (sb.partial && !steps)
        {
            // crxUpdateQparam: the step's change, coded before the line.
            const uint32_t c = code(lines.bits, kq, 23, 8);
            q += signed_of(c);
            kq = next_k(kq, int32_t(c));
            if (kq > 7)
                return error = "a subband's code amiss", false;
        }
        if (!lines.next(o))
            return error = "a subband's code amiss", false;
        const auto scale = [](int32_t& v, int32_t s) { v = int32_t(uint32_t(v) * uint32_t(s)); };  // as LibRaw's, wrapping
        if (steps)
        {
            // The tile's steps where this line lies, scaled.
            const int r = row < sb.row_start ? 0 : row < sb.height - sb.row_end ? row - sb.row_end : sb.height - sb.row_end - sb.row_start - 1;
            const int last = (sb.width - sb.col_end - sb.col_start - 1) >> sb.level_shift;
            if (r < 0 || r >= steps->height[level] || last < 0 || last >= steps->width)
                return error = "quantisation steps out of range", false;
            const uint32_t* t = steps->table[level].data() + size_t(steps->width) * r;
            const auto step = [&](uint32_t s) { return std::clamp(int32_t(uint32_t(sb.step_base) + ((s * sb.step_mult) >> 3)), 1, 0x168000); };
            int i = 0;
            for (; i < sb.col_start; ++i)
                scale(o[i], step(t[0]));
            for (; i < sb.width - sb.col_end; ++i)
                scale(o[i], step(t[(i - sb.col_start) >> sb.level_shift]));
            for (; i < sb.width; ++i)
                scale(o[i], step(t[last]));
        }
        else
        {
            if (q < 0 || q / 6 >= 6)  // (LibRaw's shift past 31 for those)
                return error = "a quantisation step out of range", false;
            const int32_t s = int32_t(step_of(q));
            if (s != 1)
                for (int i = 0; i < sb.width; ++i)
                    scale(o[i], s);
        }
    }
    return true;
}

// A tile's plane's inverse 5/3 wavelet, a line out at a time, as LibRaw's
// crxIdwt53Filter* run it (edges and all): each level takes its subbands'
// lines in turn, and the coarser level's output as its LL.
class Wavelet
{
public:
    Wavelet(const Prepared& p, const Tile& t, const Coefficients* bands) : bands_(bands), levels_(p.header.levels), flags_(t.flags)
    {
        size_t total = 0;
        for (int l = 0; l < levels_; ++l)
        {
            Level& w = level_[l];
            const bool last = l == levels_ - 1;
            w.height = last ? t.height : bands[3 * l + 4].height;
            w.width = last ? t.width : bands[3 * l + 5].width;
            total += 8 * size_t(w.width);
        }
        lines_.resize(total);
        int32_t* at = lines_.data();
        int widest = 0;
        for (int l = 0; l < levels_; ++l)
        {
            for (int32_t*& line : level_[l].line)
                line = at, at += level_[l].width;
            widest = std::max(widest, level_[l].width);
        }
        for (int b = 0; b < 3 * levels_ + 1; ++b)
            widest = std::max(widest, bands[b].width);
        zeros_.assign(size_t(widest), 0);
    }
    // The first lines of each level (crxIdwt53FilterInitialize).
    bool start()
    {
        for (int l = 0; l < levels_; ++l)
        {
            Level& w = level_[l];
            const int b = 3 * l;
            if (l)
                w.band[0] = get(l - 1);
            else if (!next_band(b))
                return false;
            // (No level is a line high, LibRaw's other case: tiles are at least 22.)
            if (!next_band(b + 1) || !next_band(b + 2) || !next_band(b + 3))
                return false;
            int32_t* h0 = w.line[w.tap + 3];
            int32_t* l0 = w.line[0];
            int32_t* l1 = w.line[1];
            int32_t* l2 = w.line[2];
            horizontal(l0, w.band[0], w.band[1], w.width);
            if (flags_ & kAbove)
            {
                horizontal(l1, w.band[2], w.band[3], w.width);
                if (!next_band(b + 3) || !next_band(b + 2))
                    return false;
                horizontal(l2, w.band[2], w.band[3], w.width);
                for (int i = 0; i < w.width; ++i)
                    h0[i] = l0[i] - ((l1[i] + l2[i] + 2) >> 2);
            }
            else
            {
                horizontal(l2, w.band[2], w.band[3], w.width);
                for (int i = 0; i < w.width; ++i)
                    h0[i] = l0[i] - ((l2[i] + 1) >> 1);
            }
            if (!decode(l) || !transform(l))
                return false;
        }
        return true;
    }
    // The next line of the tile's plane.
    const int32_t* next()
    {
        if (!decode(levels_ - 1) || !transform(levels_ - 1))
            return nullptr;
        return get(levels_ - 1);
    }

private:
    struct Level
    {
        const int32_t* band[4] = {};  // the lines of LL, HL, LH, HH in hand
        int32_t* line[8] = {};        // the L and H lines (0..2) and those out (3..7, a ring)
        int cur_line = 0, cur_h = 0, tap = 0;
        int width = 0, height = 0;
    };

    // Subband b's next line where its level reads it.
    bool next_band(int b)
    {
        const Coefficients& c = bands_[b];
        const int32_t* line;
        if (c.values.empty())
            line = zeros_.data();
        else if (row_[b] >= c.height)
            return false;
        else
            line = c.values.data() + size_t(row_[b]++) * c.width;
        if (b == 0)
            level_[0].band[0] = line;
        else
            level_[(b - 1) / 3].band[(b - 1) % 3 + 1] = line;
        return true;
    }
    int32_t* get(int l)  // crxIdwt53FilterGetLine
    {
        Level& w = level_[l];
        int32_t* r = w.line[(w.tap - w.cur_h + 5) % 5 + 3];
        --w.cur_h;
        return r;
    }

    // A line from a pair of subbands, horizontally: `low`'s samples (the
    // even ones) and `high`'s (odd) lifted into `out`.
    void horizontal(int32_t* out, const int32_t* low, const int32_t* high, int width) const
    {
        if (width <= 1)
        {
            out[0] = low[0];
            return;
        }
        if (flags_ & kLeft)
        {
            out[0] = low[0] - ((high[0] + high[1] + 2) >> 2);
            ++high;
        }
        else
            out[0] = low[0] - ((high[0] + 1) >> 1);
        ++low;
        for (int i = 0; i < width - 3; i += 2)
        {
            const int32_t delta = low[0] - ((high[0] + high[1] + 2) >> 2);
            out[1] = high[0] + ((delta + out[0]) >> 1);
            out[2] = delta;
            ++low;
            ++high;
            out += 2;
        }
        if (flags_ & kRight)
        {
            const int32_t delta = low[0] - ((high[0] + high[1] + 2) >> 2);
            out[1] = high[0] + ((delta + out[0]) >> 1);
            if (width & 1)
                out[2] = delta;
        }
        else if (width & 1)
        {
            const int32_t delta = low[0] - ((high[0] + 1) >> 1);
            out[1] = high[0] + ((delta + out[0]) >> 1);
            out[2] = delta;
        }
        else
            out[1] = high[0] + out[0];
    }

    bool decode(int l)  // crxIdwt53FilterDecode
    {
        Level& w = level_[l];
        if (w.cur_h)
            return true;
        const int b = 3 * l;
        if (w.height - 3 <= w.cur_line && !(flags_ & kBelow))
        {
            if (w.height & 1)
            {
                if (l ? !decode(l - 1) : !next_band(b))
                    return false;
                if (!next_band(b + 1))
                    return false;
            }
            return true;
        }
        if (l ? !decode(l - 1) : !next_band(b))
            return false;
        return next_band(b + 1) && next_band(b + 2) && next_band(b + 3);
    }

    bool transform(int l)  // crxIdwt53FilterTransform
    {
        Level& w = level_[l];
        if (w.cur_h)
            return true;
        if (w.cur_line >= w.height - 3)
        {
            if (flags_ & kBelow)
                return true;
            if (w.height & 1)
            {
                if (l)
                {
                    if (!level_[l - 1].cur_h && !transform(l - 1))
                        return false;
                    w.band[0] = get(l - 1);
                }
                int32_t* h0 = w.line[w.tap + 3];
                int32_t* h1 = w.line[(w.tap + 1) % 5 + 3];
                int32_t* h2 = w.line[(w.tap + 2) % 5 + 3];
                int32_t* l0 = w.line[0];
                int32_t* l1 = w.line[1];
                w.line[1] = w.line[2];
                w.line[2] = l1;
                horizontal(l0, w.band[0], w.band[1], w.width);
                l1 = w.line[1];
                for (int i = 0; i < w.width; ++i)
                {
                    const int32_t delta = l0[i] - ((l1[i] + 1) >> 1);
                    h1[i] = l1[i] + ((delta + h0[i]) >> 1);
                    h2[i] = delta;
                }
                w.cur_h += 3;
                w.cur_line += 3;
                w.tap = (w.tap + 3) % 5;
            }
            else
            {
                int32_t* l2 = w.line[2];
                int32_t* h0 = w.line[w.tap + 3];
                int32_t* h1 = w.line[(w.tap + 1) % 5 + 3];
                w.line[1] = l2;
                w.line[2] = w.line[1];
                for (int i = 0; i < w.width; ++i)
                    h1[i] = h0[i] + l2[i];
                w.cur_h += 2;
                w.cur_line += 2;
                w.tap = (w.tap + 2) % 5;
            }
            return true;
        }
        if (l)
        {
            if (!level_[l - 1].cur_h && !transform(l - 1))
                return false;
            w.band[0] = get(l - 1);
        }
        int32_t* l0 = w.line[0];
        int32_t* l1 = w.line[1];
        int32_t* h0 = w.line[w.tap + 3];
        int32_t* h1 = w.line[(w.tap + 1) % 5 + 3];
        int32_t* h2 = w.line[(w.tap + 2) % 5 + 3];
        w.line[1] = w.line[2];
        w.line[2] = l1;
        horizontal(l0, w.band[0], w.band[1], w.width);
        horizontal(l1, w.band[2], w.band[3], w.width);
        l1 = w.line[1];
        const int32_t* l2 = w.line[2];
        for (int i = 0; i < w.width; ++i)
        {
            const int32_t delta = l0[i] - ((l2[i] + l1[i] + 2) >> 2);
            h1[i] = l1[i] + ((delta + h0[i]) >> 1);
            h2[i] = delta;
        }
        const int n = w.cur_line >= w.height - 3 && w.height & 1 ? 3 : 2;
        w.cur_h += n;
        w.cur_line += n;
        w.tap = (w.tap + n) % 5;
        return true;
    }

    const Coefficients* bands_;
    int levels_, flags_;
    Level level_[3];
    int row_[10] = {};  // each subband's next line
    std::vector<int32_t> lines_, zeros_;
};

// A line of a tile's plane into `planes` (crxConvertPlaneLine, encoding
// 0): about the middle of the samples' range, into the plane for where this
// one lies in the 2 x 2.
void put(const Prepared& p, int plane, int row, int col, const int32_t* line, int n, uint16_t* planes)
{
    const int bits = p.header.bits;
    const int32_t median = 1 << (bits - 1), most = (1 << bits) - 1;
    const int at = ((plane >> 1) ^ (p.header.cfa >> 1)) << 1 | ((plane & 1) ^ (p.header.cfa & 1));
    uint16_t* o = planes + (size_t(at) * p.plane_h + row) * p.plane_w + col;
    for (int i = 0; i < n; ++i)
        o[i] = uint16_t(std::clamp(median + line[i], 0, most));
}

}  // namespace

bool prepare(const uint8_t* file, size_t size, const Header& h, Prepared& p, std::string& error)
{
    p = Prepared{};
    p.header = h;
    p.file = file, p.size = size;
    // crxParseImageHeader's and crxSetupImageData's checks, and what this takes.
    if (h.planes != 4 || h.encoding != 0)
        return error = h.planes != 4 ? "a CR3 of one plane" : "a CR3 encoding " + std::to_string(h.encoding), false;
    if ((h.version != 0x100 && h.version != 0x200) || h.bits < 9 || h.bits > 14 || h.cfa < 0 || h.cfa > 3 || h.levels < 0 || h.levels > 3 || h.width & 1 ||
        h.height & 1 || h.tile_w & 1 || h.tile_h & 1 || h.tile_w > h.width || h.tile_h > h.height || !h.header_size || h.size < h.header_size ||
        h.offset > size || h.size > size - h.offset)
        return error = "an unexpected CR3 header", false;
    p.plane_w = h.width / 2, p.plane_h = h.height / 2;
    p.tile_w = h.tile_w / 2, p.tile_h = h.tile_h / 2;
    if (p.tile_w < 0x16 || p.tile_h < 0x16 || p.plane_w > 0x7FFF || p.plane_h > 0x7FFF)
        return error = "an unexpected CR3 tiling", false;
    const int across = (p.plane_w + p.tile_w - 1) / p.tile_w, down = (p.plane_h + p.tile_h - 1) / p.tile_h;
    if (across > 0xFF || down > 0xFF || p.plane_w - p.tile_w * (across - 1) < 0x16 || p.plane_h - p.tile_h * (down - 1) < 0x16)
        return error = "an unexpected CR3 tiling", false;
    const int bands = 3 * h.levels + 1;
    const uint64_t data = h.offset + h.header_size;  // where the tiles' data starts
    const uint8_t* at = file + h.offset;
    int64_t left = h.header_size;
    uint64_t tile_offset = 0;
    p.tiles.resize(size_t(across) * down);
    for (int n = 0; n < int(p.tiles.size()); ++n)
    {
        // crxReadImageHeaders: the tile's place and neighbours, then its header.
        Tile& t = p.tiles[size_t(n)];
        const int tx = n % across, ty = n / across;
        t.width = tx < across - 1 ? p.tile_w : p.plane_w - p.tile_w * (across - 1);
        t.height = ty < down - 1 ? p.tile_h : p.plane_h - p.tile_h * (down - 1);
        t.x = tx * p.tile_w, t.y = ty * p.tile_h;
        t.flags = (tx < across - 1 ? kRight : 0) | (tx ? kLeft : 0) | (ty < down - 1 ? kBelow : 0) | (ty ? kAbove : 0);
        if (left < 4)
            return error = "CR3 tile headers amiss", false;
        const unsigned sign = be16(at), hsize = be16(at + 2);
        if ((sign != 0xFF01 || hsize != 8) && (sign != 0xFF11 || (hsize != 8 && hsize != 16)))
            return error = "CR3 tile headers amiss", false;
        if (left < int64_t(hsize) + 4)
            return error = "CR3 tile headers amiss", false;
        const unsigned tail = be16(at + 10);
        if ((hsize == 8 && tail) || (hsize == 16 && tail != 0x4000) || be16(at + 8) != unsigned(n))
            return error = "CR3 tile headers amiss", false;
        const uint32_t tile_size = be32(at + 4);
        uint32_t extra = 0;
        if (hsize == 16)
        {
            if (be16(at + 18))
                return error = "CR3 tile headers amiss", false;
            t.steps_size = h.version == 0x200 ? be32(at + 12) : 0;  // only 0x200's are used
            if (h.version == 0x200 && !t.steps_size)
                return error = "CR3 tile headers amiss", false;
            extra = be32(at + 12) + be16(at + 16);
        }
        t.steps_offset = data + tile_offset;
        left -= hsize + 4;
        at += hsize + 4;
        uint64_t plane_offset = tile_offset + extra;
        tile_offset += tile_size;
        for (int c = 0; c < 4; ++c)
        {
            // Its planes' headers, then each one's subbands'.
            Tile::Plane& pl = t.planes[c];
            if (left < 12)
                return error = "CR3 plane headers amiss", false;
            const unsigned psign = be16(at), psize = be16(at + 2);
            if ((psign != 0xFF02 && psign != 0xFF12) || psize != 8 || at[8] >> 4 != c || at[9] || at[10] || at[11])
                return error = "CR3 plane headers amiss", false;
            const uint32_t plane_size = be32(at + 4);
            const int rounded = (at[8] >> 1) & 3;
            pl.partial = (at[8] & 8) != 0;
            if (rounded && (h.levels || !pl.partial))
                return error = "CR3 plane headers amiss", false;
            pl.rounded = rounded ? 1 << (rounded - 1) : 0;
            left -= 12;
            at += 12;
            pl.bands.resize(size_t(bands));
            uint64_t band_offset = 0;
            for (int b = 0; b < bands; ++b)
            {
                // crxReadSubbandHeaders
                Subband& sb = pl.bands[size_t(b)];
                if (left < 4)
                    return error = "CR3 subband headers amiss", false;
                const unsigned bsign = be16(at), bsize = be16(at + 2);
                if (left < int64_t(bsize) + 4 || ((bsign != 0xFF03 || bsize != 8) && (bsign != 0xFF13 || bsize != 16)))
                    return error = "CR3 subband headers amiss", false;
                const uint32_t band_size = be32(at + 4);
                if ((at[8] & 0xF0) >> 4 != b)
                    return error = "CR3 subband headers amiss", false;
                int64_t used;
                if (bsign == 0xFF03)
                {
                    const uint32_t v = be32(at + 8);
                    used = int64_t(band_size) - (v & 0x7FFFF);
                    sb.partial = (v & 0x8000000) != 0;
                    sb.q = int((v >> 19) & 0xFF);
                }
                else
                {
                    if ((be16(at + 8) & 0xFFF) || be16(at + 18))
                        return error = "CR3 subband headers amiss", false;
                    sb.q = 0;
                    used = int64_t(band_size) - be16(at + 16);
                    sb.step_base = int32_t(be32(at + 12));
                    sb.step_mult = be16(at + 10);
                }
                sb.offset = data + plane_offset + band_offset;
                if (used < 0 || sb.offset > size || uint64_t(used) > size - sb.offset)
                    return error = "a CR3 subband past the file's end", false;
                sb.size = uint64_t(used);
                band_offset += band_size;
                left -= bsize + 4;
                at += bsize + 4;
            }
            plane_offset += plane_size;
            if (!h.levels)
            {
                pl.bands[0].width = t.width, pl.bands[0].height = t.height;
                if (!pl.bands[0].size)
                    return error = "a CR3 tile without data", false;
                continue;
            }
            // crxProcessSubbands: each level's sizes, finest first, with what
            // they reach past the tile's edges into its neighbours.
            const int8_t* row_reach = kReach + 0x30 * (h.levels - 1) + 6 * (t.width & 7);
            const int8_t* col_reach = kReach + 0x30 * (h.levels - 1) + 6 * (t.height & 7);
            const bool v2 = h.version == 0x200;
            const auto place = [&](Subband& sb, int level, int col_start, int col_end, int row_start, int row_end)
            {
                if (!v2)
                    return;
                sb.row_start = row_start, sb.row_end = row_end, sb.col_start = col_start, sb.col_end = col_end;
                sb.level_shift = 3 - level;
            };
            int w = t.width, ht = t.height;
            Subband* sb = pl.bands.data() + bands - 1;
            for (int level = 0; level < h.levels; ++level, sb -= 3)
            {
                const int odd_w = w & 1, odd_h = ht & 1;
                w = (odd_w + w) >> 1;
                ht = (odd_h + ht) >> 1;
                int w0 = 0, w1 = 0, h0 = 0, h1 = 0, col0 = 0, row0 = 0;
                if (t.flags & kRight)
                    w0 = row_reach[2 * level], w1 = row_reach[2 * level + 1];
                if (t.flags & kLeft)
                    ++w0, col0 = 1;
                if (t.flags & kBelow)
                    h0 = col_reach[2 * level], h1 = col_reach[2 * level + 1];
                if (t.flags & kAbove)
                    ++h0, row0 = 1;
                sb[0].width = w + w0 - odd_w, sb[0].height = ht + h0 - odd_h;
                place(sb[0], level + 1, col0, w0 - col0, row0, h0 - row0);
                sb[-1].width = w + w1, sb[-1].height = ht + h0 - odd_h;
                place(sb[-1], level + 1, 0, w1, row0, h0 - row0);
                sb[-2].width = w + w0 - odd_w, sb[-2].height = ht + h1;
                place(sb[-2], level + 1, col0, w0 - col0, 0, h1);
            }
            const int wx = t.flags & kRight ? row_reach[2 * h.levels - 1] : 0, hx = t.flags & kBelow ? col_reach[2 * h.levels - 1] : 0;
            sb->width = wx + w, sb->height = hx + ht;
            place(*sb, h.levels, 0, wx, 0, hx);
            for (const Subband& s : pl.bands)
                if (s.width <= 0 || s.height <= 0)
                    return error = "an unexpected CR3 tiling", false;
        }
        if (t.steps_size && (t.steps_offset > size || t.steps_size > size - t.steps_offset))
            return error = "CR3 quantisation steps past the file's end", false;
    }
    return true;
}

bool decode(const Prepared& p, std::vector<uint16_t>& planes, std::string& error)
{
    const size_t tiles = p.tiles.size();
    planes.resize(size_t(p.header.width) * p.header.height);
    std::vector<std::string> errors(tiles * 4);  // each tile's plane's
    // Lossless: each tile's plane's one subband, a thread each, its lines
    // straight into its plane.
    if (!p.header.levels)
        parallel(
            tiles * 4,
            [&](size_t u)
            {
                const Tile& t = p.tiles[u / 4];
                const int c = int(u % 4);
                const Subband& sb = t.planes[c].bands[0];
                Lines lines(p.file + sb.offset, sb.size, sb.width, sb.height, t.planes[c].partial, t.planes[c].rounded);
                std::vector<int32_t> line(size_t(sb.width));
                for (int row = 0; row < t.height; ++row)
                {
                    if (!lines.next(line.data()))
                        return void(errors[u] = "a subband's code amiss");
                    put(p, c, t.y + row, t.x, line.data(), t.width, planes.data());
                }
            },
            unsigned(tiles * 4));
    // cRAW: a thread a tile, for its quantisation steps (version 0x200); then
    // a thread a plane, which decodes its subbands a thread each, then runs
    // its wavelet into its plane.
    else
        parallel(
            tiles,
            [&](size_t n)
            {
                const Tile& t = p.tiles[n];
                Steps steps;
                if (t.steps_size && !decode_steps(p, t, steps, errors[n * 4]))
                    return;
                const size_t bands = size_t(3 * p.header.levels + 1);
                parallel(
                    4,
                    [&](size_t c)
                    {
                        const Tile::Plane& pl = t.planes[c];
                        std::vector<Coefficients> coefficients(bands);
                        std::vector<std::string> failed(bands);
                        parallel(
                            bands,
                            [&](size_t b)
                            {
                                decode_subband(p, pl.bands[b], b == 0, pl, t.steps_size ? &steps : nullptr, b ? int(b - 1) / 3 : 0, coefficients[b], failed[b]);
                            },
                            unsigned(bands));
                        std::string& e = errors[n * 4 + c];
                        for (const std::string& f : failed)
                            if (!f.empty())
                                return void(e = f);
                        Wavelet w(p, t, coefficients.data());
                        if (!w.start())
                            return void(e = "a wavelet's subbands run short");
                        for (int row = 0; row < t.height; ++row)
                        {
                            const int32_t* line = w.next();
                            if (!line)
                                return void(e = "a wavelet's subbands run short");
                            put(p, int(c), t.y + row, t.x, line, t.width, planes.data());
                        }
                    },
                    4);
            },
            unsigned(tiles));
    for (const std::string& e : errors)
        if (!e.empty())
            return error = e, false;
    return true;
}

void readout(const Prepared& p, const std::vector<uint16_t>& planes, std::vector<uint16_t>& out)
{
    const int w = p.header.width, pw = p.plane_w;
    out.resize(size_t(w) * p.header.height);
    parallel(size_t(p.header.height),
             [&](size_t y)
             {
                 // The row's two planes, each every other photosite.
                 const uint16_t* a = planes.data() + ((y & 1) * 2 * size_t(p.plane_h) + y / 2) * pw;
                 const uint16_t* b = a + size_t(p.plane_h) * pw;
                 uint16_t* o = out.data() + y * w;
                 for (int x = 0; x < pw; ++x)
                 {
                     o[2 * x] = a[x];
                     o[2 * x + 1] = b[x];
                 }
             });
}

}  // namespace crx
