#include "rawpacked.h"

#include "../parallel.h"

namespace rawpacked
{

// As unpack.hlsl does on the GPU, a row a thread, each row's bits read in
// order (rows start on a byte).
void unpack(const Prepared& p, std::vector<uint16_t>& out)
{
    out.resize(size_t(p.readout_w) * p.readout_h);
    const uint8_t* rows = p.file + p.offset;
    const size_t n = p.bytes, g = size_t(p.group);
    // The q-th byte as the loaders take it: within each group of `g`, from its last.
    const auto byte = [&](size_t q) -> uint64_t
    {
        const size_t k = q % g, at = q - k + (g - 1 - k);
        return at < n ? rows[at] : 0;
    };
    const uint64_t mask = (1u << p.bits) - 1;
    const unsigned flip = p.swap ? 1 : 0;
    parallel(size_t(p.readout_h),
             [&](unsigned y)
             {
                 uint16_t* o = out.data() + size_t(y) * p.readout_w;
                 size_t q = size_t(uint64_t(y) * p.row_bits / 8);
                 if (p.bits == 16 && g <= 2 && q % 2 == 0)  // whole 16-bit samples, either order
                 {
                     const uint8_t* s = rows + q;
                     for (unsigned x = 0; x < unsigned(p.readout_w); ++x, s += 2)
                     {
                         const uint32_t v = uint32_t(g == 2 ? s[0] | s[1] << 8 : s[0] << 8 | s[1]) >> p.shift;
                         o[x ^ flip] = p.curve.empty() ? uint16_t(v) : p.curve[v];
                     }
                     return;
                 }
                 uint64_t held = 0;
                 int have = 0;
                 for (unsigned x = 0; x < unsigned(p.readout_w); ++x)
                 {
                     for (; have < p.bits; have += 8)
                         held = held << 8 | (g == 1 ? (q < n ? rows[q] : 0) : byte(q)), ++q;
                     have -= p.bits;
                     const uint32_t v = uint32_t(held >> have & mask) >> p.shift;
                     o[x ^ flip] = p.curve.empty() ? uint16_t(v) : p.curve[v];
                 }
             });
}

}  // namespace rawpacked
