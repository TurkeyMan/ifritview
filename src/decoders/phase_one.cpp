#include "phase_one.h"

#include "bytes.h"

#include <algorithm>
#include <string>

namespace phase_one
{
namespace
{

// A flat field (phase_one_flat_field): gains at points `step` apart from
// `at` (x, y in the readout) over `span`, a gain a point (luma) or two (red's
// and blue's: chroma); between them bilinear, as LibRaw steps through them.
// It makes them over [at, at + span - step), as far as the readout goes.
void flat_field(const FileReader& r, size_t d, bool is_float, int nc, const Sensor& s, std::vector<dng::GainMap>& maps)
{
    uint32_t head[8];
    for (int i = 0; i < 8; ++i)
        head[i] = r.u16(d + i * 2);
    if (head[2] == 0 || head[3] == 0 || head[4] == 0 || head[5] == 0)
        return;
    const uint32_t wide = head[2] / head[4] + (head[2] % head[4] != 0), high = head[3] / head[5] + (head[3] % head[5] != 0);
    const uint32_t per = uint32_t(nc / 2);
    std::vector<float> gains(size_t(wide) * high * per);
    for (size_t i = 0; i < gains.size(); ++i)
        gains[i] = is_float ? r.f32(d + 16 + i * 4) : float(r.u16(d + 16 + i * 2)) / 32768.f;
    if (wide < 2 || high < 2)
        return;
    // What it covers, in the active area.
    const int x0 = int(head[0]) - s.left, y0 = int(head[1]) - s.top;
    const int right = std::min(int(head[0] + head[2]) - int(head[4]), s.raw_w) - s.left;
    const int bottom = std::min(int(head[1] + head[3]) - int(head[5]), s.raw_h) - s.top;
    const int area[4] = {std::max(y0, 0), std::max(x0, 0), std::min(bottom, s.height), std::min(right, s.width)};
    if (area[2] <= area[0] || area[3] <= area[1])
        return;
    for (uint32_t plane = 0; plane < per; ++plane)
    {
        dng::GainMap m;
        int top = area[0], left = area[1];
        if (nc > 2)
        {
            // Red's (plane 0) or blue's: at their photosites, every other row and column.
            const int colour = plane ? 2 : 0;
            int at = 0;
            while (at < 3 && s.filter[at] != colour)
                ++at;
            top += ((at >> 1) - top) & 1;
            left += ((at & 1) - left) & 1;
            m.pitch[0] = m.pitch[1] = 2;
            if (top >= area[2] || left >= area[3])
                continue;
        }
        m.area[0] = uint32_t(top), m.area[1] = uint32_t(left), m.area[2] = uint32_t(area[2]), m.area[3] = uint32_t(area[3]);
        m.points[0] = high, m.points[1] = wide;
        m.spacing[0] = float(head[5]), m.spacing[1] = float(head[4]);
        m.origin[0] = float(y0 - top) + 0.5f, m.origin[1] = float(x0 - left) + 0.5f;  // the first point at the first photosite's centre
        m.gains.resize(size_t(wide) * high);
        for (size_t i = 0; i < m.gains.size(); ++i)
            m.gains[i] = gains[i * per + plane];
        maps.push_back(std::move(m));
    }
}

}  // namespace

dng::Corrections corrections(const Sensor& s)
{
    dng::Corrections c;
    dng::Readout readout;
    std::vector<dng::GainMap> maps;
    std::string applied, skipped;
    const auto said = [](std::string& to, const std::string& what) { to += (to.empty() ? "" : ", ") + what; };

    // phase_one_subtract_black's offsets, both or neither.
    if (s.row_black && s.column_black)
    {
        readout.row_black.resize(size_t(s.height) * 2);
        readout.column_black.resize(size_t(s.width) * 2);
        for (int y = 0; y < s.height; ++y)
            for (int k = 0; k < 2; ++k)
                readout.row_black[size_t(y) * 2 + k] = s.row_black[y + s.top][k];
        for (int x = 0; x < s.width; ++x)
            for (int k = 0; k < 2; ++k)
                readout.column_black[size_t(x) * 2 + k] = s.column_black[x + s.left][k];
        said(applied, "black per row and column");
    }
    readout.split[0] = s.split[0] - s.left, readout.split[1] = s.split[1] - s.top;
    readout.black = s.black;

    // The curves, composed: each quarter's table (above left, above right,
    // below left, below right) through each in turn, as they come.
    std::vector<uint16_t>& tables = readout.curves;
    const auto through = [&](unsigned quarters, const auto& f)
    {
        if (tables.empty())
        {
            tables.resize(4 * 65536);
            for (size_t i = 0; i < tables.size(); ++i)
                tables[i] = uint16_t(i);
        }
        for (unsigned q = 0; q < 4; ++q)
            if (quarters >> q & 1)
                for (size_t i = 0; i < 65536; ++i)
                    tables[q * 65536 + i] = f(tables[q * 65536 + i], q);
    };
    const bool quartered = s.split[0] > 0 && s.split[0] < s.raw_w && s.split[1] > 0 && s.split[1] < s.raw_h;
    std::vector<uint16_t> curve(65536);
    std::vector<unsigned> bad_columns;
    int curves = 0, flats = 0;
    size_t defects = 0;

    // phase_one_correct's walk through the tags.
    const FileReader r{s.file, s.size, !(s.meta < s.size && s.file[s.meta] == 'I')};  // the tags' first byte says which
    const size_t dir = s.meta + r.u32(s.meta + 8);
    uint32_t entries = s.meta_length ? r.u32(dir) : 0;
    bool qmult_applied = false, qlin_applied = false;
    for (size_t e = dir + 8; entries--; e += 12)
    {
        const uint32_t tag = r.u32(e);
        const int32_t len = int32_t(r.u32(e + 4));
        const size_t d = s.meta + r.u32(e + 8);
        if (d >= s.size || len < 0 || (len > 8 && d + size_t(len) > 2 * s.size))
            continue;
        if (tag == 0x0400)
        {
            // Sensor defects: columns, put right last; photosites, now.
            for (int32_t left = len - 8, at = 0; left >= 0; left -= 8, at += 8)
            {
                const unsigned col = r.u16(d + at), row = r.u16(d + at + 2), type = r.u16(d + at + 4);
                if (col >= unsigned(s.raw_w))
                    continue;
                if (type == 131 || type == 137)
                    bad_columns.push_back(col);
                else if (type == 129 && row < unsigned(s.raw_h))
                {
                    const int x = int(col) - s.left, y = int(row) - s.top;
                    if (x < 0 || y < 0 || x >= s.width || y >= s.height)
                        continue;
                    if (readout.defects.empty())
                        readout.defects.resize(size_t(s.height));
                    readout.defects[size_t(y)].push_back(uint16_t(x));
                    ++defects;
                }
            }
        }
        else if (tag == 0x0419 || tag == 0x041a)
        {
            // A polynomial curve: 0x419 the right half's (output
            // calibration), 0x41a the whole's.
            float poly[8];
            if (tag == 0x0419)
            {
                for (int i = 0; i < 8; ++i)
                    poly[i] = r.f32(d + 4 + i * 4);
                poly[3] += (s.temperature - poly[7]) * poly[6] + 1;
                for (int i = 0; i < 0x10000; i++)
                {
                    const float num = (poly[5] * i + poly[3]) * i + poly[1];
                    curve[i] = uint16_t(std::clamp(num, 0.f, 65535.f));
                }
            }
            else
            {
                for (int i = 0; i < 4; ++i)
                    poly[i] = r.f32(d + i * 4);
                for (int i = 0; i < 0x10000; i++)
                {
                    float num = 0;
                    for (int j = 4; j--;)
                        num = num * i + poly[j];
                    curve[i] = uint16_t(std::clamp(num + i, 0.f, 65535.f));
                }
            }
            through(tag & 1 ? 0b1010u : 0b1111u, [&](uint16_t v, unsigned) { return curve[v]; });
            ++curves;
        }
        else if (tag == 0x0401)
            flat_field(r, d, true, 2, s, maps), ++flats;  // all colours (luma), as floats
        else if (tag == 0x0416 || tag == 0x0410)
            flat_field(r, d, false, 2, s, maps), ++flats;  // all colours (luma)
        else if (tag == 0x040b)
            flat_field(r, d, false, 4, s, maps), ++flats;  // red and blue (chroma)
        else if (tag == 0x0412)
            said(skipped, "0x412 (a correction by row and level)");
        else if (tag == 0x041f && !qlin_applied && quartered)
        {
            // Each quarter linearised: a spline through its levels to their mean.
            uint16_t lc[2][2][16], ref[16];
            bool baddiv = false;
            for (int q = 0; q < 4; ++q)
            {
                for (int i = 0; i < 16; ++i)
                    lc[q >> 1][q & 1][i] = uint16_t(r.u32(d + (q * 16 + i) * 4));
                baddiv |= lc[q >> 1][q & 1][15] == 0;
            }
            if (baddiv)
                continue;
            for (int i = 0; i < 16; ++i)
                ref[i] = uint16_t((lc[0][0][i] + lc[0][1][i] + lc[1][0][i] + lc[1][1][i] + 2) >> 2);
            for (unsigned q = 0; q < 4; ++q)
            {
                int cx[19], cf[19];
                for (int i = 0; i < 16; ++i)
                    cx[1 + i] = lc[q >> 1][q & 1][i], cf[1 + i] = ref[i];
                cx[0] = cf[0] = 0;
                cx[17] = cf[17] = int((unsigned(ref[15]) * 65535) / lc[q >> 1][q & 1][15]);
                cf[18] = cx[18] = 65535;
                s.spline(cx, cf, 19, curve.data());
                through(1u << q, [&](uint16_t v, unsigned) { return curve[v]; });
            }
            qlin_applied = true;
            ++curves;
        }
        else if (tag == 0x041e && !qmult_applied)
        {
            // Quarter multipliers (output calibration).
            const float qmult[4] = {1.0f + r.f32(d + 16), 1.0f + r.f32(d + 40), 1.0f + r.f32(d + 56), 1.0f + r.f32(d + 72)};
            through(0b1111u, [&](uint16_t v, unsigned q) { return uint16_t(std::clamp(int(qmult[q] * v), 0, 65535)); });
            qmult_applied = true;
            ++curves;
        }
        else if (tag == 0x0431 && !qmult_applied && quartered)
        {
            // Each quarter's gain at seven levels, a spline through them.
            uint16_t lc[2][2][7], ref[7];
            for (int i = 0; i < 7; ++i)
                ref[i] = uint16_t(r.u32(d + i * 4));
            for (int q = 0; q < 4; ++q)
                for (int i = 0; i < 7; ++i)
                    lc[q >> 1][q & 1][i] = uint16_t(r.u32(d + (7 + q * 7 + i) * 4));
            for (unsigned q = 0; q < 4; ++q)
            {
                int cx[9], cf[9];
                for (int i = 0; i < 7; ++i)
                    cx[1 + i] = ref[i], cf[1 + i] = int((unsigned(ref[i]) * lc[q >> 1][q & 1][i]) / 10000);
                cx[0] = cf[0] = 0;
                cx[8] = cf[8] = 65535;
                s.spline(cx, cf, 9, curve.data());
                through(1u << q, [&](uint16_t v, unsigned) { return curve[v]; });
            }
            qmult_applied = qlin_applied = true;
            ++curves;
        }
    }
    // Bad columns: one on its own (none other within 4) estimated along the
    // gradient, others from the average of those around.
    std::sort(bad_columns.begin(), bad_columns.end());
    bool prev_isolated = true;
    for (size_t i = 0; i < bad_columns.size(); ++i)
    {
        const bool next_isolated = i + 1 == bad_columns.size() || bad_columns[i + 1] > bad_columns[i] + 4;
        const int x = int(bad_columns[i]) - s.left;
        if (x >= 0 && x < s.width)
        {
            readout.bad_columns.resize(size_t(s.width));
            readout.bad_columns[size_t(x)] = prev_isolated && next_isolated ? 3 : 1;
        }
        prev_isolated = next_isolated;
    }
    if (curves)
        said(applied, std::to_string(curves) + (curves == 1 ? " curve" : " curves"));
    if (defects)
        said(applied, std::to_string(defects) + " defective photosites");
    if (!bad_columns.empty())
        said(applied, std::to_string(bad_columns.size()) + " bad columns");
    if (flats)
        said(applied, std::to_string(flats) + " flat fields");
    c.summary = applied.empty() ? "" : "applied: " + applied;
    if (!skipped.empty())
        c.summary += (c.summary.empty() ? "" : "; ") + std::string("not applied: ") + skipped;
    dng::lay_out(c, maps, {}, &readout);
    return c;
}

}  // namespace phase_one
