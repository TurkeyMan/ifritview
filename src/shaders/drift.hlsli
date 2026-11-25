// Where a raw's colour stops being linear, from how its ratio to the others
// drifts as the light on one surface rises: develop.hlsl's DRIFT (the
// tiles), clip.hlsl's DRIFT (the runs) and COMBINE (the limit) and, compiled
// as C++, --check-highlight's cases. Scalars and arrays only, so it reads
// the same in both.
//
// Over one smooth surface the colour's ratios hold as the light on it
// rises, while every colour is linear (estimate.hlsli's chromaticity:
// independent of brightness). A colour that's past its linearity limit
// rises slower than the light: its ratio to the others falls with
// brightness. The image is read as tiles of kDriftTile x kDriftTile pixels
// (every other one each way), a tile kept only where all its colours are
// measured (nothing censored) and its ratios are even across it (within
// kDriftSpread: no edge, no texture). Three tiles in a row, along either
// axis, make a run: kept where the middle one's ratios are halfway between
// the ends' (within kDriftMid: the ratios change smoothly along it, so it's
// one surface, where at the edge of two objects they'd jump) and where the
// light rises along it by kDriftRise at most (however little: a run isn't
// chosen by its noisy rise). For a colour c, how far its log rose against
// how far the logs of the colours further from their clips than c (at the
// brighter end) rose: the light's rise d, and c's departure e from it (0
// while c's linear; e = (s - 1) d where its slope is s). The nearest its
// clip is read against the other two's mean; the middle one against the
// furthest's alone, as the nearest may have rolled off with it (an sRAW's
// red and green, clipped together before encoding); the furthest isn't read.
// Each run counts at c's level in the brighter end,
// as a fraction of its clip, into kDriftBins levels from kPileFrom up: the
// sums of e d, d d, e e and how many, across the whole image, and of the
// noise's variances in d and in e.
//
// The noise: d and e are both read from noisy tiles, and e's error holds
// d's with its sign turned (e is c's rise less d), so the plain least
// squares slope of e on d would fall with the noise alone, a linear colour
// seeming to roll off (regression dilution). Each tile's noise is measured
// from its own samples: the spread of its log ratios (which hold across a
// surface, so their spread is noise only) gives each colour's log variance,
// and the mean's is that over how many. The slope's then the errors-in-
// variables one with those variances known (the method of moments, Fuller's):
// b = (sum e d + sum vd) / (sum d d - sum vd), vd the variance of d's noise,
// a linear colour's 0 in expectation whatever the noise; its standard error
// from the residuals over the noise-free spread of d. A level counts as measured
// with kDriftRuns runs or more and that error kDriftSure or less. The colour
// departs at the lowest measured level whose b is -kDriftDepart or less (it
// rises at most 85% as fast as the light), significantly (3 standard errors
// from 0), where it went on departing (two measured levels from it up at
// least, and half of them, by half that at least: near its clip every
// colour can roll off together, and none drifts against the others) and
// where some measured level below it was still linear (b over
// -kDriftDepart / 3: it's seen to start): its limit is the top of the highest
// such level under it. That's sure. With none departing, it's
// sure there's no limit under its clip if measured levels reach 0.9 of it;
// otherwise unsure, and an unsure image takes the camera's remembered
// limit (raw_linearity.h). A brighter pixel of a different object isn't a
// drift: its run's middle isn't halfway, or its tile isn't even.
#ifndef DRIFT_HLSLI
#define DRIFT_HLSLI

#include "clip_layout.hlsli"

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#define DRIFT_FN inline
#define DRIFT_IN const
#define DRIFT_ARRAY_OUT
#define DRIFT_INOUT
#define DRIFT_OUT(type, name) type& name
#define DRIFT_LOG2(x) std::log2(x)
#define DRIFT_ABS(x) std::fabs(x)
#define DRIFT_SQRT(x) std::sqrt(x)
#define DRIFT_MIN(a, b) std::min(a, b)
#define DRIFT_MAX(a, b) std::max(a, b)
#else
#define DRIFT_FN
#define DRIFT_IN
#define DRIFT_ARRAY_OUT out
#define DRIFT_INOUT inout
#define DRIFT_OUT(type, name) out type name
#define DRIFT_LOG2(x) log2(x)
#define DRIFT_ABS(x) abs(x)
#define DRIFT_SQRT(x) sqrt(x)
#define DRIFT_MIN(a, b) min(a, b)
#define DRIFT_MAX(a, b) max(a, b)
#endif

#define kDriftTile 8
static const float kDriftFloor = 0.01;     // a colour this dark (balanced) is too noisy to read a ratio from
static const float kDriftSpread = 0.1;     // a tile's log2 ratios within this across it
static const float kDriftMid = 0.02;       // a run's middle's log2 ratios within this of halfway
static const float kDriftRise = 0.5;       // the light rises along a run by this (log2) at most
static const float kDriftRuns = 32;        // runs at a level for it to count
static const float kDriftSure = 0.05;      // the slope's standard error at a level for it to count
static const float kDriftDepart = 0.15;    // a slope 0.85 or less departs
static const float kDriftReach = 0.9;      // measured to here (of the clip), no departure: no limit

// A tile's samples: t[0..2] their sums (balanced), t[3] and t[4] the least
// and most log2 R/G, t[5] and t[6] of B/G, t[7] how many (-1 once one isn't
// usable: censored, or too dark); t[8..12] the sums of log2 R/G, its square,
// log2 B/G, its square, and their product (the noise).
#define kDriftTileSums 13
DRIFT_FN void drift_start(DRIFT_INOUT float t[kDriftTileSums])
{
    t[0] = 0, t[1] = 0, t[2] = 0;
    t[3] = 1e30, t[4] = -1e30, t[5] = 1e30, t[6] = -1e30;
    t[7] = 0;
    t[8] = 0, t[9] = 0, t[10] = 0, t[11] = 0, t[12] = 0;
}

DRIFT_FN void drift_sample(DRIFT_INOUT float t[kDriftTileSums], float r, float g, float b, bool usable)
{
    if (t[7] < 0)
        return;
    if (!usable || !(r > kDriftFloor && g > kDriftFloor && b > kDriftFloor))
    {
        t[7] = -1;
        return;
    }
    t[0] += r, t[1] += g, t[2] += b;
    const float rg = DRIFT_LOG2(r / g), bg = DRIFT_LOG2(b / g);
    t[3] = DRIFT_MIN(t[3], rg), t[4] = DRIFT_MAX(t[4], rg);
    t[5] = DRIFT_MIN(t[5], bg), t[6] = DRIFT_MAX(t[6], bg);
    t[7] += 1;
    t[8] += rg, t[9] += rg * rg, t[10] += bg, t[11] += bg * bg, t[12] += rg * bg;
}

// The tile's mean, and whether it's kept (every sample usable, its ratios
// even); and the noise: the variance of each colour's log2 mean, from the
// spread of the samples' log ratios (R/G, B/G and so R/B: each the sum of
// its two colours' variances, which three of them pin down).
DRIFT_FN bool drift_tile(DRIFT_IN float t[kDriftTileSums], DRIFT_ARRAY_OUT float mean[3], DRIFT_ARRAY_OUT float noise[3])
{
    const float n = DRIFT_MAX(t[7], 1.0f);
    mean[0] = t[0] / n, mean[1] = t[1] / n, mean[2] = t[2] / n;
    const float mrg = t[8] / n, mbg = t[10] / n, k = n / DRIFT_MAX(n - 1, 1.0f);
    const float vrg = DRIFT_MAX(t[9] / n - mrg * mrg, 0.0f) * k, vbg = DRIFT_MAX(t[11] / n - mbg * mbg, 0.0f) * k;
    const float cov = (t[12] / n - mrg * mbg) * k, vrb = DRIFT_MAX(vrg + vbg - 2 * cov, 0.0f);
    noise[0] = DRIFT_MAX(vrg + vrb - vbg, 0.0f) / 2 / n;
    noise[1] = DRIFT_MAX(vrg + vbg - vrb, 0.0f) / 2 / n;
    noise[2] = DRIFT_MAX(vbg + vrb - vrg, 0.0f) / 2 / n;
    return t[7] > 1 && t[4] - t[3] <= kDriftSpread && t[6] - t[5] <= kDriftSpread;
}

// A run of three kept tiles' means (balanced) and the ends' noise (drift_tile()),
// for colour c whose clip is stops[c]: whether it counts, at which level, and
// its sums (e d, d d, e e, 1, d's noise variance, e's).
DRIFT_FN bool drift_run(int c, DRIFT_IN float lo[3], DRIFT_IN float mid[3], DRIFT_IN float hi[3], DRIFT_IN float lo_noise[3],
               DRIFT_IN float hi_noise[3], DRIFT_IN float stops[3], DRIFT_OUT(int, bin), DRIFT_ARRAY_OUT float s[kDriftSums])
{
    bin = 0;
    for (int z = 0; z < int(kDriftSums); ++z)
        s[z] = 0;
    float a[3], m[3], h[3];
    for (int k = 0; k < 3; ++k)
    {
        a[k] = DRIFT_LOG2(lo[k]);
        m[k] = DRIFT_LOG2(mid[k]);
        h[k] = DRIFT_LOG2(hi[k]);
    }
    // One surface: the middle's ratios halfway between the ends'.
    if (DRIFT_ABS((m[0] - m[1]) - 0.5f * ((a[0] - a[1]) + (h[0] - h[1]))) > kDriftMid ||
        DRIFT_ABS((m[2] - m[1]) - 0.5f * ((a[2] - a[1]) + (h[2] - h[1]))) > kDriftMid)
        return false;
    const int o1 = c == 0 ? 1 : 0, o2 = c == 2 ? 1 : 2;
    // The light's rise is read from the colours further from their clips
    // than c at the brighter end: the other two's mean where c is the
    // nearest its clip; the one under it alone where c is the middle one
    // (the nearest may have rolled off with c, as an sRAW's red and green
    // do together, or before it); none, the furthest of the three, whose
    // rise is the light's for the others.
    bool up = true;
    int under = -1;
    float d = 0, e = 0, vd = 0;
    for (int turn = 0; turn < 2; ++turn)
    {
        if (turn == 0)
        {
            d = 0.5f * ((h[o1] + h[o2]) - (a[o1] + a[o2]));
            vd = 0.25f * (lo_noise[o1] + lo_noise[o2] + hi_noise[o1] + hi_noise[o2]);
        }
        else
        {
            d = h[under] - a[under];
            vd = lo_noise[under] + hi_noise[under];
        }
        up = d >= 0;
        // Each colour's level at the brighter end, as a fraction of its clip.
        const float level = (up ? hi[c] : lo[c]) / stops[c], l1 = (up ? hi[o1] : lo[o1]) / stops[o1],
                    l2 = (up ? hi[o2] : lo[o2]) / stops[o2];
        if (turn == 0 && level >= l1 && level >= l2)
            break;  // the nearest its clip: against the other two
        if (turn == 1 && level >= (under == o1 ? l1 : l2))
            break;  // still over the one it's read against
        if (turn == 1 || (level < l1 && level < l2))
            return false;  // the furthest from its clip, or no longer the middle one
        under = level >= l1 ? o1 : o2;
    }
    e = (h[c] - a[c]) - d;  // c's departure from the light's rise
    if (!up)
        d = -d, e = -e;
    // No run chosen by how far the light rose (by d, or e against it): both
    // are noisy, and e's noise is d's turned, so choosing by them would bias
    // the slope as the noise does; only a rise too great to be one smooth
    // stretch of a surface is left out.
    if (d > kDriftRise)
        return false;
    // Its level at the brighter end.
    const float level = (up ? hi[c] : lo[c]) / stops[c];
    if (level < kPileFrom || level >= 1)
        return false;
    bin = DRIFT_MIN(int((level - kPileFrom) / (1 - kPileFrom) * float(kDriftBins)), int(kDriftBins) - 1);
    s[0] = e * d, s[1] = d * d, s[2] = e * e, s[3] = 1;
    s[4] = vd;
    s[5] = lo_noise[c] + hi_noise[c] + vd;
    return true;
}

// A slope (drift_limit()'s errors-in-variables one) and its standard error
// from a level's sums, or levels' summed; whether it's measured.
DRIFT_FN bool drift_slope(float ed, float dd, float ee, float n, float vd, DRIFT_OUT(float, beta), DRIFT_OUT(float, err))
{
    beta = 0, err = 1;
    // d's spread, its noise taken out: only where most of it is the light's.
    const float spread = dd - vd;
    if (!(n >= kDriftRuns && spread > 0.5f * dd))
        return false;
    beta = (ed + vd) / spread;
    const float rest = DRIFT_MAX(ee - 2 * beta * ed + beta * beta * dd, 0.0f);
    err = DRIFT_SQRT(rest / DRIFT_MAX(n - 1, 1.0f) / spread);
    return err <= kDriftSure;
}

// A colour's linearity limit (a fraction of its clip; 1: none under it)
// from its sums (kDriftSums a level, kDriftBins levels, kDriftGroups
// quadrants of the image, each a quadrant's runs), and whether that's sure.
// The levels' slopes are the quadrants' runs together. A roll-off is the
// sensor's: a function of the colour's level, the same wherever in the image
// that level is. A colour that changes across the scene as the light does
// (a gradient) departs only where that gradient is: so a departure counts
// where two quadrants or more each depart from that level up (by half
// kDriftDepart at least) and none measured there holds (its slope over
// -kDriftDepart / 3, significantly).
DRIFT_FN float drift_limit(DRIFT_IN float s[kDriftGroups * kDriftBins * kDriftSums], DRIFT_OUT(int, sure))
{
    sure = 0;
    float slope[kDriftBins], error[kDriftBins];
    bool measured[kDriftBins];
    int top = -1;
    for (int b = 0; b < int(kDriftBins); ++b)
    {
        float sum[kDriftSums];
        for (int k = 0; k < int(kDriftSums); ++k)
        {
            sum[k] = 0;
            for (int g = 0; g < int(kDriftGroups); ++g)
                sum[k] += s[(g * int(kDriftBins) + b) * int(kDriftSums) + k];
        }
        float beta, err;
        measured[b] = drift_slope(sum[0], sum[1], sum[2], sum[3], sum[4], beta, err);
        slope[b] = beta, error[b] = err;
        if (measured[b])
            top = b;
    }
    const float width = (1 - kPileFrom) / float(kDriftBins);
    bool doubt = false;
    for (int k = 0; k < int(kDriftBins); ++k)
    {
        if (!measured[k] || slope[k] > -kDriftDepart || slope[k] + 3 * error[k] >= 0)
            continue;
        doubt = true;
        int departing = 0, above = 0;
        for (int u = k; u < int(kDriftBins); ++u)
            if (measured[u])
            {
                ++above;
                if (slope[u] <= -0.5f * kDriftDepart)
                    ++departing;
            }
        const bool persists = departing >= 2 && 2 * departing >= above;
        int steady = -1;  // the highest measured level under k still linear
        for (int v = 0; v < k; ++v)
            if (measured[v] && slope[v] > -kDriftDepart / 3)
                steady = v;
        // The same everywhere: each quadrant's runs from k up.
        int agree = 0, holds = 0;
        for (int g = 0; g < int(kDriftGroups); ++g)
        {
            float sum[kDriftSums];
            for (int z = 0; z < int(kDriftSums); ++z)
                sum[z] = 0;
            for (int u = k; u < int(kDriftBins); ++u)
                for (int z = 0; z < int(kDriftSums); ++z)
                    sum[z] += s[(g * int(kDriftBins) + u) * int(kDriftSums) + z];
            float beta, err;
            if (!drift_slope(sum[0], sum[1], sum[2], sum[3], sum[4], beta, err))
                continue;
            if (beta <= -0.5f * kDriftDepart)
                ++agree;
            else if (beta - 2 * err > -kDriftDepart / 3)
                ++holds;
        }
        if (persists && steady >= 0 && agree >= 2 && holds == 0)
        {
            sure = 1;
            return kPileFrom + float(steady + 1) * width;
        }
    }
    if (!doubt && top >= 0 && kPileFrom + float(top + 1) * width >= kDriftReach)
        sure = 1;
    return 1;
}
#endif
