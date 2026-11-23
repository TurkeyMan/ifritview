// A raw's censored colours estimated, one pixel: develop.hlsl's clipped()
// and, compiled as C++, --check-highlight's cases. Scalars and arrays only,
// so it reads the same in both.
//
// The trust order, each estimate giving way only to what's more trusted:
// the measurements (colours under their linearity limits, nothing censored
// mixed in), then the lower bounds (a censored colour is at least what was
// read of it), then the colour field's chromaticity (interpolated from the
// measurements around), then the field's magnitude (continued across where
// nothing's measured).
//
// A reading past its colour's linearity limit is censored: the sensor's
// response there has rolled off or stopped, so the true value is only known
// to be at least the reading. At the clip the reading is the limit; under
// it (a colour that rolls off before it clips, a Canon mRAW's green), it's
// what was read. Either way it's no measurement and no evidence of the
// colour, only its bound (estimate_censor()).
//
// v: the pixel's colours, balanced. share: of each, the share of what it's
// made from (the demosaic's photosites, an sRAW's blocks) that's censored:
// 0 measured, 1 censored, between mixed. bound: each censored colour's
// lower bound, the least of its censored readings. q: the field's
// chromaticity, its brightness (estimate_luma()) 1.
// sure: how sure each colour is in log terms (its log's inverse variance).
// continuation: the field's magnitude here (linear), continued from around.
//
// The magnitude m, with the colour q: where colours are measured, the least
// squares fit of m q to them in log terms (weighed by sureness; a mixed
// colour, part censored, is no measurement of it); a censored colour is
// then m q, raised to its bound if that's under it (the chromaticity gives
// way to the bound; the measured colours stay as they are). A mixed colour
// is a bound too: its censored part is at least what was read of it, so the
// whole is at least its reading. Where none is measured, m is the
// continuation, raised to the least that puts every censored or mixed
// colour at its bound or reading or over (the bounds win over the
// continuation), and each colour is m q. A mixed colour keeps its own value,
// blended toward m q by its censored share, and never under that value.
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#define EST_OUT
#define EST_OUTV(type, name) type& name
#define EST_IN const
#define EST_LOG2(x) std::log2(x)
#define EST_EXP2(x) std::exp2(x)
#define EST_MAX(a, b) std::max(a, b)
#else
#define EST_OUT out
#define EST_OUTV(type, name) out type name
#define EST_IN
#define EST_LOG2(x) log2(x)
#define EST_EXP2(x) exp2(x)
#define EST_MAX(a, b) max(a, b)
#endif

// One colour's reading v against its linearity limit: censored (share 1)
// at or past it, the reading its bound; measured (share 0) under it. (The
// demosaic's and an sRAW's footprints take this over what they read.)
void estimate_censor(float v, float limit, EST_OUTV(float, share), EST_OUTV(float, bound))
{
    share = v >= limit ? 1.0f : 0.0f;
    bound = EST_MAX(v, limit);
}

void estimate_colours(EST_IN float v[3], EST_IN float share[3], EST_IN float bound[3], EST_IN float q[3], EST_IN float sure[3],
                      float continuation, EST_OUT float result[3])
{
    float sum = 0, weight = 0, least = 0;
    bool touched = false;
    for (int i = 0; i < 3; ++i)
    {
        result[i] = v[i];
        if (share[i] > 0)
            touched = true;
        if (share[i] <= 0 && v[i] > 0 && q[i] > 0)
        {
            sum += sure[i] * (EST_LOG2(v[i]) - EST_LOG2(q[i]));
            weight += sure[i];
        }
        if (share[i] >= 1 && q[i] > 0)
            least = EST_MAX(least, bound[i] / q[i]);
        else if (share[i] > 0 && q[i] > 0)
            least = EST_MAX(least, v[i] / q[i]);  // a mixed colour: at least what was read of it
    }
    if (!touched)
        return;

    const bool measured = weight > 0;
    const float m = measured ? EST_EXP2(sum / weight) : EST_MAX(least, continuation);
    for (int k = 0; k < 3; ++k)
    {
        if (share[k] >= 1)
            result[k] = measured ? EST_MAX(bound[k], m * q[k]) : m * q[k];
        else if (share[k] > 0)
            result[k] = EST_MAX(v[k], v[k] + share[k] * (m * q[k] - v[k]));
    }
}

// A Bayer output pixel's censored shares, by what its demosaic (develop.hlsl's
// bayer(): Malvar, He and Cutler's 5 x 5 kernels) actually reads: each
// colour's share the part of its kernel (by the size of each coefficient)
// that falls on censored photosites, of whatever colour (an interpolated
// colour's corrections read the photosite's own colour too). The pixel's own
// colour is its own photosite alone. censored[25]: each photosite's 0 or 1,
// row by row from (-2, -2); c the centre's colour (0 R, 1 G, 2 B), red_row
// whether a green's row holds reds.
void estimate_bayer_shares(EST_IN float censored[25], int c, bool red_row, EST_OUT float share[3])
{
#define CEN(i, j) censored[((j) + 2) * 5 + (i) + 2]
    const float m = CEN(0, 0);
    const float cross = CEN(-1, 0) + CEN(1, 0) + CEN(0, -1) + CEN(0, 1);
    const float far = CEN(-2, 0) + CEN(2, 0) + CEN(0, -2) + CEN(0, 2);
    const float diag = CEN(-1, -1) + CEN(1, -1) + CEN(-1, 1) + CEN(1, 1);
    share[0] = 0, share[1] = 0, share[2] = 0;
    if (c == 1)
    {
        // Along the row: 5 m, 4 each side, 1 each diagonal, 1 each two along, 0.5 each two across.
        const float h = (5 * m + 4 * (CEN(-1, 0) + CEN(1, 0)) + diag + CEN(-2, 0) + CEN(2, 0) + 0.5f * (CEN(0, -2) + CEN(0, 2))) / 20;
        const float v = (5 * m + 4 * (CEN(0, -1) + CEN(0, 1)) + diag + CEN(0, -2) + CEN(0, 2) + 0.5f * (CEN(-2, 0) + CEN(2, 0))) / 20;
        share[0] = red_row ? h : v;
        share[1] = m;
        share[2] = red_row ? v : h;
    }
    else
    {
        const float g = (4 * m + 2 * cross + far) / 16;           // green: 4 m, 2 each across, 1 each two along
        const float other = (6 * m + 2 * diag + 1.5f * far) / 20; // the other: 6 m, 2 each diagonal, 1.5 each two along
        share[c] = m;
        share[1] = g;
        share[2 - c] = other;
    }
#undef CEN
}
// The colour field's chromaticity (develop.hlsl's chromaticity_at()), its
// brightness 1 (camera RGB weighed 1/4, 1/2, 1/4): the log colours (green's
// 0) that best fit three log2 ratios, R/G, B/G and R/B, each weighed by its
// confidence (measured, or filled in from what was measured around); a
// ratio with none counts for next to nothing (1e-3 of the total), so where
// only R/G is known the colour has that R/G, not one pulled towards grey.
// With no confidence at all, grey.
void estimate_chromaticity(float rg, float wrg, float bg, float wbg, float rb, float wrb, EST_OUT float q[3])
{
    const float total = wrg + wbg + wrb;
    float a = 1, b = 1, c = 1;
    if (total > 0)
        a = wrg / total + 1e-3f, b = wbg / total + 1e-3f, c = wrb / total + 1e-3f;
    // The weighted least squares of x - 0 = rg, y - 0 = bg, x - y = rb.
    const float p11 = a + c, p12 = -c, p22 = b + c;
    const float r1 = a * rg + c * rb, r2 = b * bg - c * rb;
    const float det = p11 * p22 - p12 * p12;
    float x = (r1 * p22 - p12 * r2) / det, y = (p11 * r2 - p12 * r1) / det;
    // The hue held: R/G and B/G together are a hue and a saturation, and
    // R/B, where it disagrees with them, says how saturated the colour is,
    // not which hue (a highlight whitens towards neutral along its hue, as a
    // flame's core does from its glow). So the colour is R/G and B/G scaled
    // together by the one factor t that best fits all three, wherever both
    // are known at all (h: near 1 once the lesser of their confidences is a
    // few percent of the whole; with only one, there's no hue to hold, and
    // the plain fit stands).
    const float den = a * rg * rg + b * bg * bg + c * (rg - bg) * (rg - bg);
    if (den > 1e-6f)
    {
        const float t = EST_MAX((a * rg * rg + b * bg * bg + c * (rg - bg) * rb) / den, 0.0f);
        const float least = a < b ? a : b, h = least / (least + 0.02f);
        x += h * (t * rg - x), y += h * (t * bg - y);
    }
    const float cr = EST_EXP2(x), cb = EST_EXP2(y), l = 0.25f * cr + 0.5f + 0.25f * cb;
    q[0] = cr / l, q[1] = 1 / l, q[2] = cb / l;
}
// A pixel whose readings are bounds only, as a whole (an sRAW's, made from
// blocks at the camera's white: that white is no colour of the scene's, and
// what was read there is no part of a measurement, only the least the pixel
// could be): the field's colour q, its magnitude the continuation raised to
// the least that puts each colour it touches at its reading or over.
void estimate_whole(EST_IN float v[3], EST_IN float share[3], EST_IN float q[3], float continuation, EST_OUT float result[3])
{
    float m = continuation;
    for (int i = 0; i < 3; ++i)
        if (share[i] > 0 && q[i] > 0)
            m = EST_MAX(m, v[i] / q[i]);
    for (int k = 0; k < 3; ++k)
        result[k] = m * q[k];
}
// The field's magnitude (log2) across where nothing measured it, as a thin
// plate (highlight.hlsl's RELAX): one damped Jacobi step of the biharmonic
// equation from the 13-point stencil's sums around (the four nearest, the
// four corners, the four two away). The plate is the trend of what was
// measured around, carried on in; the censored values' bounds are each
// pixel's own floor, taken where it's read (estimate_colours()'s least),
// not held in the plate: held there, the step a bound makes at an area's
// edge would be carried on in as a slope, ever steeper.
float plate_step(float own, float nearest, float corners, float two_away)
{
    const float plate = (8 * nearest - 2 * corners - two_away) / 20;
    return own + 0.5f * (plate - own);
}
