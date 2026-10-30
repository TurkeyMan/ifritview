#include "fuji.h"

#include "../parallel.h"
#include "bits.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace fuji
{
namespace
{

// The line buffers a strip keeps, each a row of one colour (LibRaw's
// _xt_lines): two rows above for each colour, then the line's own (R2..R4,
// G2..G7, B2..B4: three rows of red, six of green, three of blue).
enum : int
{
    R0, R1, R2, R3, R4,
    G0, G1, G2, G3, G4, G5, G6, G7,
    B0, B1, B2, B3, B4,
    kLines
};

int log2ceil(int v)
{
    int n = 0;
    if (v--)
        do
            ++n;
        while (v >>= 1);
    return n;
}

// A quantisation of the gradients (fuji_q_table): -4..4 for a difference
// (by `table`, indexed from -max), how a gradient is made of two of them,
// the differences' range and how many bits one takes outright.
struct QTable
{
    std::vector<int8_t> table;
    int q_base = 0, max_grad = 0, q_grad_mult = 0, total_values = 0, raw_bits = 0;

    // setup_qlut's thresholds, qp[4] the greatest value.
    void thresholds(const int qp[5])
    {
        table.resize(size_t(2 * qp[4] + 1));
        for (int v = -qp[4], i = 0; v <= qp[4]; ++v, ++i)
            table[size_t(i)] = int8_t(v <= -qp[3] ? -4 : v <= -qp[2] ? -3 : v <= -qp[1] ? -2 : v < -qp[0] ? -1 : v <= qp[0] ? 0
                                      : v < qp[1]                                                              ? 1
                                      : v < qp[2]                                                              ? 2
                                      : v < qp[3]                                                              ? 3
                                                                                                               : 4);
    }
    int quant(int max, int v1, int v2) const { return q_grad_mult * table[size_t(max + v1)] + table[size_t(max + v2)]; }
};

// What every strip shares (fuji_compressed_params): the lossy tables for
// flat places (1..3); the main one, [0], is each strip's own when lossy.
struct Common
{
    QTable qt[4];
    int line_width = 0, min_value = 0x40, max_value = 0, max_bits = 0;
    bool lossless = false;
};

// init_main_qtable: the main table for a step `q_base`.
void main_table(Common& c, QTable& qt, int q_base)
{
    int qp[5];
    const int max = c.max_value + 1;
    qp[0] = q_base;
    qp[1] = 3 * q_base + 0x12;
    qp[2] = 5 * q_base + 0x43;
    qp[3] = 7 * q_base + 0x114;
    qp[4] = c.max_value;
    if (qp[1] >= max || qp[1] < q_base + 1)
        qp[1] = q_base + 1;
    if (qp[2] < qp[1] || qp[2] >= max)
        qp[2] = qp[1];
    if (qp[3] < qp[2] || qp[3] >= max)
        qp[3] = qp[2];
    qt.thresholds(qp);
    qt.q_base = q_base;
    qt.max_grad = 0;
    qt.total_values = (qp[4] + 2 * q_base) / (2 * q_base + 1) + 1;
    qt.raw_bits = log2ceil(qt.total_values);
    qt.q_grad_mult = 9;
    c.max_bits = 4 * log2ceil(qp[4] + 1);
}

// init_fuji_compr: the tables every strip starts from.
Common common(const Header& h)
{
    Common c;
    c.lossless = h.lossless;
    c.line_width = h.xtrans ? h.block_width * 2 / 3 : h.block_width / 2;
    c.max_value = (1 << h.bits) - 1;
    if (h.lossless)
    {
        main_table(c, c.qt[0], 0);
        return c;
    }
    // Three more for flat places, steps 0, 1 and 2.
    static const int kThresholds[3][3] = {{0x12, 0x43, 0x114}, {0x15, 0x48, 0x11B}, {0x18, 0x4D, 0x122}};
    for (int k = 0; k < 3; ++k)
    {
        QTable& t = c.qt[k + 1];
        t.q_base = k;
        t.max_grad = 5 + k;
        t.q_grad_mult = 3;
        t.total_values = (c.max_value + 2 * k) / (2 * k + 1) + 1;
        t.raw_bits = log2ceil(t.total_values);
        int qp[5];
        qp[0] = k;
        qp[4] = c.max_value;
        qp[1] = qp[4] >= kThresholds[k][0] ? kThresholds[k][0] : qp[0] + 1;
        qp[2] = qp[4] >= kThresholds[k][1] ? kThresholds[k][1] : qp[1];
        qp[3] = qp[4] >= kThresholds[k][2] ? kThresholds[k][2] : qp[2];
        t.thresholds(qp);
    }
    c.max_bits = 4 * log2ceil(c.max_value + 1);
    return c;
}

// The adaptive statistics for one kind of sample: a sum of magnitudes and a
// count for each gradient (int_pair), the flat places' apart.
struct Pair
{
    int sum, count;
};
struct Grads
{
    Pair grads[41];
    Pair lossy[3][5];
};

void reset(Pair* p, int n, int total_values)
{
    const int diff = std::max(2, (total_values + 0x20) >> 6);
    for (int i = 0; i < n; ++i)
        p[i] = {diff, 1};
}

// A strip, decoded a line at a time as fuji_decode_strip does.
class Strip
{
public:
    Strip(const Prepared& p, const Common& c, int block) : p_(p), h_(p.header), c_(c), block_(block),
        bits_(p.file + p.offsets[size_t(block)], p.sizes[size_t(block)]), stride_(c.line_width + 2),
        lines_(size_t(kLines) * size_t(stride_), 0)
    {
        if (c_.lossless)
            for (int j = 0; j < 3; ++j)
            {
                reset(even_[j].grads, 41, c_.qt[0].total_values);
                reset(odd_[j].grads, 41, c_.qt[0].total_values);
            }
        else
        {
            c_.qt[0].q_base = -1;
            for (int k = 0; k < 3; ++k)
                for (int j = 0; j < 3; ++j)
                {
                    reset(even_[j].lossy[k], 5, c_.qt[k + 1].total_values);
                    reset(odd_[j].lossy[k], 5, c_.qt[k + 1].total_values);
                }
        }
    }

    void decode(uint16_t* readout)
    {
        const int width = block_ + 1 == h_.blocks ? h_.readout_w - h_.block_width * block_ : h_.block_width;
        const uint8_t* q = p_.q_bases ? p_.q_bases + size_t(block_) * size_t(p_.q_stride) : nullptr;
        // Where each of a line's photosites is in the line buffers
        // (copy_line_to_xtrans, copy_line_to_bayer): its colour's row, and
        // X-Trans's two of every three columns of a colour's samples.
        std::vector<uint32_t> from(size_t(6) * size_t(width));
        for (int r = 0; r < 6; ++r)
            for (int x = 0; x < width; ++x)
            {
                const int c = h_.colour[r][x % 6];
                const int l = c == 0 ? R2 + (r >> 1) : c == 2 ? B2 + (r >> 1) : G2 + r;
                const int i = h_.xtrans ? (((x * 2 / 3) & 0x7FFFFFFE) | ((x % 3) & 1)) + ((x % 3) >> 1) : x >> 1;
                from[size_t(r) * size_t(width) + size_t(x)] = uint32_t(l * stride_ + 1 + i);
            }
        for (int line = 0; line < h_.lines; ++line)
        {
            // A lossy line's step: its table, and fresh statistics, when it changes.
            if (!c_.lossless && (!line || q[line] != c_.qt[0].q_base))
            {
                main_table(c_, c_.qt[0], q[line]);
                for (int j = 0; j < 3; ++j)
                {
                    reset(even_[j].grads, 41, c_.qt[0].total_values);
                    reset(odd_[j].grads, 41, c_.qt[0].total_values);
                }
            }
            decode_line();
            // The last two rows of each colour are the next line's two above.
            static const int kKept[6][2] = {{R0, R3}, {R1, R4}, {G0, G6}, {G1, G7}, {B0, B3}, {B1, B4}};
            for (const auto& k : kKept)
                std::memcpy(row(k[0]), row(k[1]), size_t(stride_) * 2);
            uint16_t* out = readout + size_t(line) * 6 * size_t(h_.readout_w) + size_t(h_.block_width) * size_t(block_);
            for (int r = 0; r < 6; ++r, out += h_.readout_w)
                for (int x = 0; x < width; ++x)
                    out[x] = lines_[from[size_t(r) * size_t(width) + size_t(x)]];
            static const int kCleared[3][2] = {{R2, 3}, {G2, 6}, {B2, 3}};
            for (const auto& k : kCleared)
            {
                std::fill_n(row(k[0]), size_t(k[1]) * size_t(stride_), uint16_t(0));
                row(k[0])[0] = row(k[0] - 1)[1];
                row(k[0])[c_.line_width + 1] = row(k[0] - 1)[c_.line_width];
            }
        }
    }

private:
    uint16_t* row(int l) { return lines_.data() + size_t(l) * size_t(stride_); }

    // A coded sample's difference (fuji_zerobits, then fuji_read_code):
    // its zeros and so many bits, or for too many zeros its value outright.
    int difference(const QTable& qt, Pair& g)
    {
        const int limit = c_.max_bits - qt.raw_bits - 1;
        int k = 0;  // bitDiff
        if (g.count < g.sum)
            while (k <= 14 && (g.count << ++k) < g.sum)
                ;
        uint32_t v;
        int code;
        if (limit > 0 && bits_.golomb(k, uint32_t(limit), v))
            code = int(v);
        else
        {
            const int zeros = int(bits_.zeros());
            if (zeros < limit)
                code = (k ? int(bits_.bits(k)) : 0) + (zeros << k);
            else
                code = (qt.raw_bits ? int(bits_.bits(qt.raw_bits)) : 0) + 1;
        }
        code = code & 1 ? -1 - code / 2 : code / 2;
        g.sum += std::abs(code);
        if (g.count == c_.min_value)
        {
            g.sum >>= 1;
            g.count >>= 1;
        }
        ++g.count;
        return code;
    }

    // The table and statistics for a gradient: a flat place's own (lossy).
    const QTable& table(int flatness, Grads& gs, Pair*& grads) const
    {
        grads = gs.grads;
        for (int i = 1; c_.qt[0].q_base >= i && i < 4; ++i)
            if (flatness <= c_.qt[i].max_grad)
            {
                grads = gs.lossy[i - 1];
                return c_.qt[i];
            }
        return c_.qt[0];
    }

    // The value predicted plus the difference, wrapped into range.
    uint16_t value(const QTable& qt, int predicted, int grad, int code) const
    {
        const int step = 2 * qt.q_base + 1;
        int v = grad < 0 ? predicted - code * step : predicted + code * step;
        if (v < -qt.q_base)
            v += qt.total_values * step;
        else if (v > qt.q_base + c_.max_value)
            v -= qt.total_values * step;
        return uint16_t(v >= 0 ? std::min(v, c_.max_value) : 0);
    }

    // An even sample predicted from the two rows above (fuji_decode_sample_even
    // and fuji_decode_interpolation_even); `coded`: a difference follows.
    void even(int l, int pos, Grads& gs, bool coded)
    {
        uint16_t* at = row(l) + 1 + pos;
        const int w = stride_;
        const int Rb = at[-w], Rc = at[-w - 1], Rd = at[-w + 1], Rf = at[-2 * w];
        const int cb = std::abs(Rc - Rb), fb = std::abs(Rf - Rb), db = std::abs(Rd - Rb);
        const int predicted = cb > fb && cb > db ? Rf + Rd + 2 * Rb : db > cb && db > fb ? Rf + Rc + 2 * Rb : Rd + Rc + 2 * Rb;
        if (!coded)
        {
            at[0] = uint16_t(predicted >> 2);
            return;
        }
        Pair* grads;
        const QTable& qt = table(fb + cb, gs, grads);
        const int grad = qt.quant(c_.max_value, Rb - Rf, Rc - Rb);
        const int code = difference(qt, grads[std::abs(grad)]);
        at[0] = value(qt, predicted >> 2, grad, code);
    }

    // An odd sample, between the even ones either side (fuji_decode_sample_odd).
    void odd(int l, int pos, Grads& gs)
    {
        uint16_t* at = row(l) + 1 + pos;
        const int w = stride_;
        const int Ra = at[-1], Rb = at[-w], Rc = at[-w - 1], Rd = at[-w + 1], Rg = at[1];
        Pair* grads;
        const QTable& qt = table(std::abs(Rb - Rc) + std::abs(Rc - Ra), gs, grads);
        const int grad = qt.quant(c_.max_value, Rb - Rc, Rc - Ra);
        const int predicted = (Rb > Rc && Rb > Rd) || (Rb < Rc && Rb < Rd) ? (Rg + Ra + 2 * Rb) >> 2 : (Ra + Rg) >> 1;
        const int code = difference(qt, grads[std::abs(grad)]);
        at[0] = value(qt, predicted, grad, code);
    }

    // Each row's ends from the row above's (fuji_extend_generic).
    void extend(int first, int last)
    {
        for (int l = first; l <= last; ++l)
        {
            row(l)[0] = row(l - 1)[1];
            row(l)[c_.line_width + 1] = row(l - 1)[c_.line_width];
        }
    }

    // A line's six rows, two of its colours' rows at a time
    // (xtrans_decode_block, fuji_bayer_decode_block). X-Trans predicts some
    // even samples without a code: always, never, or where pos & 3 is 0 or 2.
    void decode_line()
    {
        enum { kNever, kAlways, kAt0, kAt2 };
        struct Phase
        {
            int a, b, grads;
            int skip_a, skip_b;  // X-Trans's even samples not coded
        };
        static const Phase kPhases[6] = {{R2, G2, 0, kAlways, kNever}, {G3, B2, 1, kNever, kAlways}, {R3, G4, 2, kAt0, kAlways},
                                         {G5, B3, 0, kNever, kAt2},    {R4, G6, 1, kAt2, kNever},    {G7, B4, 2, kAlways, kAt0}};
        const auto coded = [&](int skip, int pos)
        { return !h_.xtrans || skip == kNever || (skip == kAt0 && (pos & 3)) || (skip == kAt2 && (pos & 3) != 2); };
        const int lw = c_.line_width;
        for (int k = 0; k < 6; ++k)
        {
            const Phase& ph = kPhases[k];
            int e = 0, o = 1;
            while (e < lw || o < lw)
            {
                if (e < lw)
                {
                    even(ph.a, e, even_[ph.grads], coded(ph.skip_a, e));
                    even(ph.b, e, even_[ph.grads], coded(ph.skip_b, e));
                    e += 2;
                }
                if (e > 8)
                {
                    odd(ph.a, o, odd_[ph.grads]);
                    odd(ph.b, o, odd_[ph.grads]);
                    o += 2;
                }
            }
            // Red and green after the even phases, green and blue after the odd.
            if (k % 2 == 0)
                extend(R2, R4);
            extend(G2, G7);
            if (k % 2)
                extend(B2, B4);
        }
    }

    const Prepared& p_;
    const Header& h_;
    Common c_;  // its own main table, when lossy
    int block_;
    Bits bits_;
    int stride_;
    std::vector<uint16_t> lines_;
    Grads even_[3] = {}, odd_[3] = {};
};

}  // namespace

bool prepare(const uint8_t* file, size_t size, const Header& h, Prepared& p, std::string& error)
{
    p.header = h;
    p.file = file, p.size = size;
    if (h.blocks <= 0 || h.lines <= 0 || h.block_width <= 0 || (h.bits != 12 && h.bits != 14 && h.bits != 16) ||
        h.readout_h != 6 * h.lines || h.readout_w > h.block_width * h.blocks || h.readout_w <= h.block_width * (h.blocks - 1) ||
        (h.xtrans ? h.block_width % 3 : h.block_width % 2))
        return error = "a compressed RAF laid out another way", false;
    // The strips' sizes (big-endian), then, lossy, each strip's steps (a
    // line a byte, rows of whole 16s), then the strips one after another.
    const uint64_t sizes_at = h.offset, sizes_len = uint64_t(h.blocks) * 4;
    uint64_t at = h.offset + ((sizes_len + 15) & ~uint64_t(15));
    if (sizes_at + sizes_len > size)
        return error = "strip sizes past the file's end", false;
    if (!h.lossless)
    {
        p.q_stride = (h.lines + 15) & ~15;
        p.q_bases = file + at;
        at += uint64_t(p.q_stride) * uint64_t(h.blocks);
        if (at > size)
            return error = "steps past the file's end", false;
    }
    for (int b = 0; b < h.blocks; ++b)
    {
        const uint32_t n = be32(file + sizes_at + 4 * uint64_t(b));
        if (at >= size)
            return error = "a strip past the file's end", false;
        p.offsets.push_back(at);
        p.sizes.push_back(std::min<uint64_t>(n, size - at));  // as LibRaw reads no further
        at += n;
    }
    return true;
}

void decode(const Prepared& p, std::vector<uint16_t>& readout)
{
    const Header& h = p.header;
    readout.assign(size_t(h.readout_w) * size_t(h.readout_h), 0);
    const Common c = common(h);
    parallel(size_t(h.blocks), [&](size_t b) { Strip(p, c, int(b)).decode(readout.data()); });
}

}  // namespace fuji
