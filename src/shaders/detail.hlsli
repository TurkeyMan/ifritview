// Noise reduction and sharpening (detail.h): what the filter's passes
// (detail.hlsl) and its noise measure (noise.hlsl) share. As detail.cpp's
// encode() and decode(), the CPU's.
#ifndef DETAIL_HLSLI
#define DETAIL_HLSLI

static const float kDetailFloor = 0.002;  // DetailFilter::kFloor
static const float kB3[5] = {1.0 / 16, 4.0 / 16, 6.0 / 16, 4.0 / 16, 1.0 / 16};
// The noise measure: a histogram of the finest scale's absolute detail,
// luminance's then colour's, kNoiseBins each over 0..kNoiseRange, then the
// two standard deviations (floats) after them; then the model (kNoiseModel:
// the encoding's offset, each colour's; the fit's a and b for each, in the
// blocks' measure; which colours fitted (bits R G B); whether b is the
// masked border's; each colour's inverse table's first and last average;
// each colour's pixel law, a then b (kNoiseLaw); the colours' correlations,
// R-G R-B G-B, the light's then the readout's);
// then the fit's counts (kNoiseFit: per colour, kFitLevels levels by
// kFitSpreads variances); then each colour's inverse table (kNoiseInverse);
// then the noise's spatial correlation (kNoiseSpatial: noise.hlsl's modes 4
// to 6, detail.cpp's measure_spatial()): the blocks' luminance variances
// counted (kSpatialBins, log2 in quarters from 2^-40), the flat blocks
// counted, each colour against each at each lag summed over them (4096 a
// unit, as integers: detail_series(), kSpatialLags each); and from those
// (kNoiseRho) each series' correlation, each colour's pixel variance
// against the fit's measure, and whether it was measured.
static const uint kNoiseBins = 1024;
static const float kNoiseRange = 0.064;
static const uint kNoiseSigma = 2 * kNoiseBins;  // words: luminance, colour
static const int kNoiseStride = 4;               // the lattice it's measured on
static const uint kNoiseModel = kNoiseSigma + 2;
static const uint kNoiseLaw = kNoiseModel + 17;  // a R G B, b R G B, correlations of a, of b
static const uint kNoiseFit = kNoiseModel + 32;
// The noise model's fit (detail.cpp's fit_noise): blocks of kFitBlock x
// kFitBlock pixels, one every kFitLattice across and down, by their mean
// (kFitLevels steps of sqrt(mean / kFitTop)) and the log2 of their variance
// (kFitSpreads steps of 1/4 from 2^-32).
static const int kFitBlock = 8;
static const int kFitLattice = 16;
static const uint kFitLevels = 64;
static const uint kFitSpreads = 128;
static const float kFitTop = 0.25;
static const uint kNoiseInverse = kNoiseFit + 3 * kFitLevels * kFitSpreads;
static const uint kInverseSize = 1024;  // each colour's inverse table
static const uint kForwardSize = 512;   // the averages it's made from
static const int kSpatialReach = 6;     // NoiseModel's: lags to 6 either way (a half plane: detail_lag())
static const uint kSpatialLags = 85;
static const uint kSpatialSeries = 9;   // each colour against each: detail_series()
static const uint kSpatialBins = 256;
static const uint kNoiseSpatial = kNoiseInverse + 3 * kInverseSize;
static const uint kNoiseSpatialCount = kNoiseSpatial + kSpatialBins;
static const uint kNoiseSpatialSums = kNoiseSpatialCount + 1;
static const uint kNoiseRho = kNoiseSpatialSums + kSpatialSeries * kSpatialLags;
static const uint kNoisePixel = kNoiseRho + kSpatialSeries * kSpatialLags;  // R G B, then 1 if measured
static const uint kNoiseWords = kNoisePixel + 4;

// The series of colour c at a pixel against colour d a lag on (detail.cpp's
// spatial_series()): R G B against themselves, then R-G G-R R-B B-R G-B B-G.
uint detail_series(uint c, uint d)
{
    return c == d ? c : c == 0 ? (d == 1 ? 3 : 5) : c == 1 ? (d == 0 ? 4 : 7) : (d == 0 ? 6 : 8);
}
uint detail_series_from(uint s)
{
    return s < 3 ? s : s == 3 || s == 5 ? 0 : s == 4 || s == 7 ? 1 : 2;
}
uint detail_series_to(uint s)
{
    return s < 3 ? s : s == 4 || s == 6 ? 0 : s == 3 || s == 8 ? 1 : 2;
}

// Lag i of the spatial correlation: (dx, dy), dy >= 0 (detail.cpp's spatial_lag()).
int2 detail_lag(uint i)
{
    const int r = kSpatialReach, row = 2 * r + 1;
    return i <= uint(r) ? int2(i, 0) : int2(int(i - r - 1) % row - r, 1 + int(i - r - 1) / row);
}

// Its index, for any lag within the reach.
uint detail_lag_index(int dx, int dy)
{
    if (dy < 0 || (dy == 0 && dx < 0))
        dx = -dx, dy = -dy;
    return dy == 0 ? uint(dx) : uint(kSpatialReach + 1 + (dy - 1) * (2 * kSpatialReach + 1) + dx + kSpatialReach);
}

// A block's luminance variance's bin.
uint detail_spatial_bin(float var)
{
    return uint(clamp(floor((log2(max(var, 1e-30)) + 40) * 4), 0.0, kSpatialBins - 1.0));
}

// The variance-stabilising transform, a generalised Anscombe transform for
// noise of variance a x + b at level x (Poisson's from the light, Gaussian's
// from the readout): up to its scale, sqrt(x + offset), offset = 3a/8 + b/a.
// Below 0 (the readout's noise around black, kept to here) the same curve
// turned about the point at 0, so it's monotonic, smooth (its slope
// continues) and exactly invertible, and noise around black is as
// symmetric after it as before.
float detail_forward(float x, float offset)
{
    const float r = sqrt(offset);
    return x >= 0 ? sqrt(x + offset) : 2 * r - sqrt(offset - x);
}

precise float detail_inverse(float e, float offset)
{
    const float r = sqrt(offset);
    return e >= r ? e * e - offset : offset - (2 * r - e) * (2 * r - e);
}

// White-balanced scene-linear RGB to its encoding: each channel stabilised
// (its noise about the same size at every brightness), as luminance and
// the two colour differences. `offset` per colour (the noise model's;
// kDetailFloor without one).
float3 detail_encode(float3 rgb, float3 offset)
{
    const float3 e = float3(detail_forward(rgb.r, offset.r), detail_forward(rgb.g, offset.g), detail_forward(rgb.b, offset.b));
    return float3((e.r + 2 * e.g + e.b) / 4, e.r - e.g, e.b - e.g);
}

// The encoding's three stabilised colours.
float3 detail_channels(float3 c)
{
    const float g = c.x - (c.y + c.z) / 4;
    return float3(c.y + g, g, c.z + g);
}

float3 detail_decode(float3 c, float3 offset)
{
    const float3 e = detail_channels(c);
    return float3(detail_inverse(e.r, offset.r), detail_inverse(e.g, offset.g), detail_inverse(e.b, offset.b));
}

// What noise reduction's output is decoded by (detail.hlsl, detail.cpp).
// A pixel of colour c at level y: a Poisson-Gaussian mixture, a_c times a
// Poisson count of mean y / a_c plus Gaussian noise of variance b_c (a_c,
// b_c the fit's for that colour, taken from the blocks' measure to a
// pixel's by the develop's own factor: GpuView's, detail.h's). Below 0
// there's no light: the readout's noise around y. Its average through the
// transform, E[f(X) | y], is Makitalo and Foi's ("Optimal inversion of the
// generalized Anscombe transformation for Poisson-Gaussian noise", IEEE
// Trans. Image Processing 22, 2013; the mixture's sum as in their ICASSP
// 2012 paper, eqs. 7-8): over the counts k, P(k) E[f(a k + N(0, b))],
// summed to 10 standard deviations of the count past its mean (the rest
// under 1e-20), each Gaussian by 21 points over 5 sigma either side (to
// 2e-7 of the 41-point sum). Past kMixtureMost expected counts, the
// heteroscedastic Gaussian approximation (variance a y + b), within 0.03%
// of the noise in the level it gives (0.9% at 8 counts, pure Poisson).
static const float kMixtureMost = 64;

// E[f(m + s N(0, 1))].
float detail_gauss(float m, float offset, float s)
{
    if (s <= 0)
        return detail_forward(m, offset);
    float sum = 0, weights = 0;
    for (int k = -10; k <= 10; ++k)
    {
        const float z = k * 0.5, w = exp(-0.5 * z * z);
        sum += w * detail_forward(m + s * z, offset);
        weights += w;
    }
    return sum / weights;
}

// E[f(X) | y] for the mixture above.
float detail_expected(float y, float offset, float a, float b)
{
    const float s = sqrt(max(b, 0.0));
    if (y <= 0 || a <= 0)
        return detail_gauss(y, offset, s);
    const float lambda = y / a;
    if (lambda > kMixtureMost)
        return detail_gauss(y, offset, sqrt(a * y + b));
    const int last = int(lambda + 10 * sqrt(lambda) + 10);
    float p = exp(-lambda), sum = 0;
    for (int k = 0; k <= last; ++k)
    {
        if (p > 1e-20)
            sum += p * detail_gauss(a * k, offset, s);
        p *= lambda / (k + 1);
    }
    return sum;
}

// What noise reduction and the sharpening keep of each colour's noise, in
// the transformed domain, for the decode (detail_unbiased()): a model of the
// filter, not a measure of it. At level y the colours' noise is the pixel
// law's covariance (each colour's a y + b, the light's and the readout's
// correlated as the develop leaves them: GpuView's develop_noise(), or
// measured: noise.hlsl's mode 6) through the transform's slope there,
// squared: 1 / (4 (|y| + offset)), y each colour's own level.
// Into luminance and the colour differences, L = (R + 2G + B) / 4, Cr = R -
// G, Cb = B - G; each scale's coefficients that covariance times the
// scale's share of noise of the measured spatial correlation
// (detail_shaping(): a develop's interpolation, an mRAW's in-camera scaling
// and shared colour make neighbours alike, which moves the noise to the
// coarser scales), luminance's and the colour pair's each their own (the
// scales taken as independent, so their shares normalised to 1). Each
// scale's thresholds (DetailFilter's, times the measured noise) then keep,
// exactly, of luminance's variance 2((1 + tau^2) Q(tau) - tau phi(tau)); of
// the colour pair's, shrunk as a 2-vector by its length, its moments in
// its covariance's eigenframe (kShrinkMoments: tools/shrink_table.py, by
// the tau and the eigenvalues' ratio); of their covariance, by Stein's
// lemma, luminance's mean slope (2 Q(tau)) times the pair's mean Jacobian
// times the covariance (exact where either is left alone; to first order
// in the correlation where both are shrunk, within 0.021 of the share kept
// against the shrinkers themselves: check_detail()). Then the sharpening
// adds to luminance's (detail.cpp's noise_kept_of()). Back to each colour: R
// = L + (3Cr - Cb) / 4, G = L - (Cr + Cb) / 4, B = L + (3Cb - Cr) / 4.
// Left out: the edge support, which spares edges (flat areas, where the
// decode's bias shows, have none), the colour's guide (on noise alone its
// weights barely vary, and the bias is the same without it), and the
// sharpening's mask (Masking above 0 spares flat areas the sharpening this
// takes as everywhere). It's worked out once per image and setting, not per
// pixel (detail.hlsl's DETAIL_TABLE): at each inverse table entry's level,
// taken for all three colours, a neutral pixel's. A coloured pixel's shares
// differ from that; against the model at its own levels its decode is
// within 0.001 of its noise for colours within 4 times each other, 0.005
// for any (check_detail()).
// kBandAutocorr: the wavelet's scales as one-dimensional filters (tools/band_table.py).
static const int kBandLags = 25;
static const float kBandAutocorr[11][kBandLags] = {
    {1.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000},  // A_00
    {0.27343750, 0.21875000, 0.10937500, 0.03125000, 0.00390625, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000},  // A_11
    {0.12347412, 0.11791992, 0.10266113, 0.08129883, 0.05833435, 0.03771973, 0.02178955, 0.01110840, 0.00491333, 0.00183105, 0.00054932, 0.00012207, 0.00001526, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000},  // A_22
    {0.06035948, 0.05969810, 0.05775547, 0.05465126, 0.05057114, 0.04574919, 0.04044700, 0.03493166, 0.02945459, 0.02423334, 0.01943851, 0.01518774, 0.01154572, 0.00852871, 0.00611258, 0.00424290, 0.00284600, 0.00183964, 0.00114202, 0.00067806, 0.00038308, 0.00020456, 0.00010228, 0.00004721, 0.00001967},  // A_33
    {0.03001472, 0.02993302, 0.02968920, 0.02928706, 0.02873280, 0.02803489, 0.02720383, 0.02625192, 0.02519295, 0.02404189, 0.02281459, 0.02152738, 0.02019679, 0.01883920, 0.01747048, 0.01610575, 0.01475909, 0.01344330, 0.01216977, 0.01094829, 0.00978705, 0.00869259, 0.00766977, 0.00672192, 0.00585083},  // A_44
    {0.01498695, 0.01497676, 0.01494625, 0.01489554, 0.01482482, 0.01473436, 0.01462452, 0.01449572, 0.01434845, 0.01418327, 0.01400081, 0.01380174, 0.01358681, 0.01335682, 0.01311258, 0.01285499, 0.01258497, 0.01230345, 0.01201142, 0.01170986, 0.01139981, 0.01108226, 0.01075827, 0.01042884, 0.01009502},  // A_55
    {0.37500000, 0.25000000, 0.06250000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000},  // A_01
    {0.15771484, 0.14648438, 0.11718750, 0.08007812, 0.04589844, 0.02148438, 0.00781250, 0.00195312, 0.00024414, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000, 0.00000000},  // A_12
    {0.07608414, 0.07478333, 0.07100677, 0.06510925, 0.05761909, 0.04916382, 0.04039001, 0.03189087, 0.02414703, 0.01748657, 0.01206970, 0.00790405, 0.00487995, 0.00281525, 0.00149918, 0.00072479, 0.00031090, 0.00011444, 0.00003433, 0.00000763, 0.00000095, 0.00000000, 0.00000000, 0.00000000, 0.00000000},  // A_23
    {0.03771785, 0.03755820, 0.03708309, 0.03630388, 0.03523894, 0.03391296, 0.03235593, 0.03060216, 0.02868898, 0.02665561, 0.02454188, 0.02238709, 0.02022890, 0.01810241, 0.01603925, 0.01406693, 0.01220828, 0.01048112, 0.00889814, 0.00746703, 0.00619080, 0.00506821, 0.00409444, 0.00326166, 0.00255979},  // A_34
    {0.01881904, 0.01879918, 0.01873970, 0.01864096, 0.01850356, 0.01832831, 0.01811625, 0.01786862, 0.01758686, 0.01727260, 0.01692762, 0.01655385, 0.01615338, 0.01572839, 0.01528116, 0.01481405, 0.01432947, 0.01382988, 0.01331776, 0.01279558, 0.01226580, 0.01173085, 0.01119311, 0.01065488, 0.01011840},  // A_45
};
static const float kShrinkTauStep = 0.25;
static const float4 kShrinkMoments[297] = {
    float4(1.000000, 0.000000, 1.000000, 1.000000),
    float4(1.000000, 0.125000, 1.000000, 1.000000),
    float4(1.000000, 0.250000, 1.000000, 1.000000),
    float4(1.000000, 0.375000, 1.000000, 1.000000),
    float4(1.000000, 0.500000, 1.000000, 1.000000),
    float4(1.000000, 0.625000, 1.000000, 1.000000),
    float4(1.000000, 0.750000, 1.000000, 1.000000),
    float4(1.000000, 0.875000, 1.000000, 1.000000),
    float4(1.000000, 1.000000, 1.000000, 1.000000),
    float4(0.659415, 0.000000, 0.792926, 0.511315),
    float4(0.673945, 0.064002, 0.813927, 0.695517),
    float4(0.683931, 0.144786, 0.821663, 0.750003),
    float4(0.691807, 0.232639, 0.826374, 0.779500),
    float4(0.698394, 0.324821, 0.830434, 0.798847),
    float4(0.704091, 0.420044, 0.835053, 0.814607),
    float4(0.709127, 0.517567, 0.838213, 0.826341),
    float4(0.713650, 0.616915, 0.840563, 0.835619),
    float4(0.717760, 0.717760, 0.843420, 0.843420),
    float4(0.419279, 0.000000, 0.611786, 0.293279),
    float4(0.434246, 0.030173, 0.633845, 0.429089),
    float4(0.446849, 0.077062, 0.646934, 0.515342),
    float4(0.457559, 0.134251, 0.657003, 0.567585),
    float4(0.466897, 0.198346, 0.665067, 0.605088),
    float4(0.475199, 0.267506, 0.672445, 0.633516),
    float4(0.482688, 0.340604, 0.678797, 0.655860),
    float4(0.489518, 0.416881, 0.683679, 0.673948),
    float4(0.495802, 0.495802, 0.689159, 0.689159),
    float4(0.256504, 0.000000, 0.451727, 0.167866),
    float4(0.267798, 0.013732, 0.469653, 0.240178),
    float4(0.278744, 0.038053, 0.485084, 0.320912),
    float4(0.288851, 0.072034, 0.498342, 0.382258),
    float4(0.298123, 0.113514, 0.508354, 0.427357),
    float4(0.306660, 0.160902, 0.518366, 0.464958),
    float4(0.314560, 0.213081, 0.527261, 0.495539),
    float4(0.321912, 0.269244, 0.534503, 0.521138),
    float4(0.328787, 0.328787, 0.541340, 0.541340),
    float4(0.150679, 0.000000, 0.318497, 0.094017),
    float4(0.158118, 0.006220, 0.331199, 0.128687),
    float4(0.165992, 0.017715, 0.345745, 0.182768),
    float4(0.173861, 0.036043, 0.358412, 0.235688),
    float4(0.181497, 0.060802, 0.370356, 0.281425),
    float4(0.188821, 0.091171, 0.381059, 0.320414),
    float4(0.195813, 0.126377, 0.390989, 0.354799),
    float4(0.202480, 0.165776, 0.399754, 0.382877),
    float4(0.208841, 0.208841, 0.407897, 0.407897),
    float4(0.084832, 0.000000, 0.214029, 0.051054),
    float4(0.089332, 0.002806, 0.222133, 0.067466),
    float4(0.094351, 0.007911, 0.233385, 0.096776),
    float4(0.099710, 0.016920, 0.244697, 0.133753),
    float4(0.105215, 0.030488, 0.254891, 0.170949),
    float4(0.110737, 0.048577, 0.265454, 0.205802),
    float4(0.116201, 0.070896, 0.274953, 0.237978),
    float4(0.121564, 0.097087, 0.284344, 0.266478),
    float4(0.126802, 0.126802, 0.292591, 0.292591),
    float4(0.045694, 0.000000, 0.136863, 0.026732),
    float4(0.048241, 0.001245, 0.141364, 0.034585),
    float4(0.051171, 0.003432, 0.149232, 0.048612),
    float4(0.054460, 0.007513, 0.157698, 0.070389),
    float4(0.058023, 0.014336, 0.166338, 0.096135),
    float4(0.061769, 0.024314, 0.174836, 0.123043),
    float4(0.065625, 0.037548, 0.183688, 0.150034),
    float4(0.069535, 0.053977, 0.191510, 0.175538),
    float4(0.073461, 0.073461, 0.199349, 0.199349),
    float4(0.023510, 0.000000, 0.077129, 0.013442),
    float4(0.024869, 0.000538, 0.085448, 0.017183),
    float4(0.026462, 0.001452, 0.090315, 0.023543),
    float4(0.028315, 0.003182, 0.096155, 0.034655),
    float4(0.030418, 0.006341, 0.102236, 0.050200),
    float4(0.032734, 0.011429, 0.108813, 0.068517),
    float4(0.035219, 0.018747, 0.115228, 0.088260),
    float4(0.037833, 0.028438, 0.122056, 0.108488),
    float4(0.040542, 0.040542, 0.128762, 0.128762),
    float4(0.011538, 0.000000, 0.044156, 0.006482),
    float4(0.012223, 0.000225, 0.047926, 0.008212),
    float4(0.013036, 0.000598, 0.051036, 0.011021),
    float4(0.014006, 0.001295, 0.054716, 0.016106),
    float4(0.015150, 0.002650, 0.058910, 0.024443),
    float4(0.016466, 0.005048, 0.063299, 0.035358),
    float4(0.017940, 0.008814, 0.068245, 0.048820),
    float4(0.019553, 0.014178, 0.073202, 0.063263),
    float4(0.021283, 0.021283, 0.078274, 0.078274),
    float4(0.005393, 0.000000, 0.023970, 0.002987),
    float4(0.005721, 0.000091, 0.025678, 0.003757),
    float4(0.006113, 0.000238, 0.027489, 0.004974),
    float4(0.006588, 0.000508, 0.029659, 0.007179),
    float4(0.007165, 0.001050, 0.031967, 0.011055),
    float4(0.007856, 0.002097, 0.034811, 0.017039),
    float4(0.008665, 0.003899, 0.037976, 0.024920),
    float4(0.009587, 0.006680, 0.041446, 0.034527),
    float4(0.010615, 0.010615, 0.045104, 0.045104),
    float4(0.002399, 0.000000, 0.012328, 0.001312),
    float4(0.002547, 0.000035, 0.013069, 0.001643),
    float4(0.002725, 0.000092, 0.014027, 0.002155),
    float4(0.002944, 0.000192, 0.015131, 0.003053),
    float4(0.003216, 0.000397, 0.016461, 0.004752),
    float4(0.003553, 0.000821, 0.018095, 0.007644),
    float4(0.003965, 0.001623, 0.019957, 0.011976),
    float4(0.004455, 0.002971, 0.022108, 0.017660),
    float4(0.005024, 0.005024, 0.024478, 0.024478),
    float4(0.001014, 0.000000, 0.006003, 0.000549),
    float4(0.001077, 0.000013, 0.006309, 0.000685),
    float4(0.001154, 0.000034, 0.006766, 0.000892),
    float4(0.001249, 0.000070, 0.007300, 0.001243),
    float4(0.001369, 0.000143, 0.007962, 0.001922),
    float4(0.001523, 0.000304, 0.008857, 0.003222),
    float4(0.001717, 0.000635, 0.009860, 0.005362),
    float4(0.001960, 0.001246, 0.011085, 0.008492),
    float4(0.002254, 0.002254, 0.012501, 0.012501),
    float4(0.000407, 0.000000, 0.002766, 0.000218),
    float4(0.000433, 0.000005, 0.002878, 0.000272),
    float4(0.000464, 0.000012, 0.003077, 0.000352),
    float4(0.000503, 0.000025, 0.003335, 0.000485),
    float4(0.000553, 0.000050, 0.003649, 0.000742),
    float4(0.000618, 0.000106, 0.004071, 0.001266),
    float4(0.000704, 0.000234, 0.004606, 0.002252),
    float4(0.000816, 0.000493, 0.005252, 0.003791),
    float4(0.000958, 0.000958, 0.006020, 0.006020),
    float4(0.000155, 0.000000, 0.001205, 0.000082),
    float4(0.000165, 0.000002, 0.001245, 0.000102),
    float4(0.000177, 0.000004, 0.001329, 0.000132),
    float4(0.000192, 0.000008, 0.001434, 0.000181),
    float4(0.000211, 0.000016, 0.001580, 0.000273),
    float4(0.000237, 0.000035, 0.001764, 0.000470),
    float4(0.000273, 0.000081, 0.002007, 0.000874),
    float4(0.000321, 0.000184, 0.002339, 0.001605),
    float4(0.000385, 0.000385, 0.002737, 0.002737),
    float4(0.000056, 0.000000, 0.000435, 0.000030),
    float4(0.000059, 0.000001, 0.000508, 0.000037),
    float4(0.000064, 0.000001, 0.000540, 0.000047),
    float4(0.000069, 0.000003, 0.000584, 0.000064),
    float4(0.000077, 0.000005, 0.000642, 0.000096),
    float4(0.000086, 0.000011, 0.000719, 0.000164),
    float4(0.000100, 0.000026, 0.000827, 0.000319),
    float4(0.000119, 0.000064, 0.000974, 0.000630),
    float4(0.000147, 0.000147, 0.001168, 0.001168),
    float4(0.000019, 0.000000, 0.000168, 0.000010),
    float4(0.000020, 0.000000, 0.000186, 0.000012),
    float4(0.000022, 0.000000, 0.000201, 0.000016),
    float4(0.000024, 0.000001, 0.000219, 0.000022),
    float4(0.000026, 0.000002, 0.000242, 0.000032),
    float4(0.000030, 0.000003, 0.000274, 0.000054),
    float4(0.000035, 0.000008, 0.000316, 0.000109),
    float4(0.000042, 0.000021, 0.000381, 0.000233),
    float4(0.000053, 0.000053, 0.000468, 0.000468),
    float4(0.000006, 0.000000, 0.000061, 0.000003),
    float4(0.000007, 0.000000, 0.000066, 0.000004),
    float4(0.000007, 0.000000, 0.000072, 0.000005),
    float4(0.000008, 0.000000, 0.000078, 0.000007),
    float4(0.000009, 0.000000, 0.000087, 0.000010),
    float4(0.000010, 0.000001, 0.000098, 0.000017),
    float4(0.000011, 0.000002, 0.000115, 0.000035),
    float4(0.000014, 0.000007, 0.000139, 0.000080),
    float4(0.000018, 0.000018, 0.000175, 0.000175),
    float4(0.000002, 0.000000, 0.000021, 0.000001),
    float4(0.000002, 0.000000, 0.000022, 0.000001),
    float4(0.000002, 0.000000, 0.000024, 0.000002),
    float4(0.000002, 0.000000, 0.000026, 0.000002),
    float4(0.000003, 0.000000, 0.000029, 0.000003),
    float4(0.000003, 0.000000, 0.000033, 0.000005),
    float4(0.000004, 0.000001, 0.000039, 0.000011),
    float4(0.000004, 0.000002, 0.000048, 0.000026),
    float4(0.000006, 0.000006, 0.000063, 0.000063),
    float4(0.000001, 0.000000, 0.000007, 0.000000),
    float4(0.000001, 0.000000, 0.000007, 0.000000),
    float4(0.000001, 0.000000, 0.000008, 0.000000),
    float4(0.000001, 0.000000, 0.000008, 0.000001),
    float4(0.000001, 0.000000, 0.000009, 0.000001),
    float4(0.000001, 0.000000, 0.000011, 0.000001),
    float4(0.000001, 0.000000, 0.000013, 0.000003),
    float4(0.000001, 0.000001, 0.000016, 0.000008),
    float4(0.000002, 0.000002, 0.000021, 0.000021),
    float4(0.000000, 0.000000, 0.000002, 0.000000),
    float4(0.000000, 0.000000, 0.000002, 0.000000),
    float4(0.000000, 0.000000, 0.000002, 0.000000),
    float4(0.000000, 0.000000, 0.000003, 0.000000),
    float4(0.000000, 0.000000, 0.000003, 0.000000),
    float4(0.000000, 0.000000, 0.000003, 0.000000),
    float4(0.000000, 0.000000, 0.000004, 0.000001),
    float4(0.000000, 0.000000, 0.000005, 0.000002),
    float4(0.000000, 0.000000, 0.000007, 0.000007),
    float4(0.000000, 0.000000, 0.000001, 0.000000),
    float4(0.000000, 0.000000, 0.000001, 0.000000),
    float4(0.000000, 0.000000, 0.000001, 0.000000),
    float4(0.000000, 0.000000, 0.000001, 0.000000),
    float4(0.000000, 0.000000, 0.000001, 0.000000),
    float4(0.000000, 0.000000, 0.000001, 0.000000),
    float4(0.000000, 0.000000, 0.000001, 0.000000),
    float4(0.000000, 0.000000, 0.000001, 0.000001),
    float4(0.000000, 0.000000, 0.000002, 0.000002),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000001, 0.000001),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000),
    float4(0.000000, 0.000000, 0.000000, 0.000000)};

// m1, m2, j1, j2 at tau (in the larger standard deviation) and kappa (the
// smaller variance over the larger), bilinearly; past tau 8, nothing kept.
float4 detail_shrink_moments(float tau, float kappa)
{
    const float t = clamp(tau / kShrinkTauStep, 0.0, 32.0), k = saturate(kappa) * 8;
    const uint ti = min(uint(t), 31u), ki = min(uint(k), 7u);
    const float ft = t - ti, fk = k - ki;
    const float4 a = lerp(kShrinkMoments[ti * 9 + ki], kShrinkMoments[ti * 9 + ki + 1], fk);
    const float4 b = lerp(kShrinkMoments[(ti + 1) * 9 + ki], kShrinkMoments[(ti + 1) * 9 + ki + 1], fk);
    return lerp(a, b, ft);
}

// The normal's upper tail.
float detail_tail(float t)
{
    // erfc by Abramowitz and Stegun 7.1.26 (to 1.5e-7).
    const float x = t / sqrt(2.0), u = 1 / (1 + 0.3275911 * x);
    const float e = u * (0.254829592 + u * (-0.284496736 + u * (1.421413741 + u * (-1.453152027 + u * 1.061405429)))) * exp(-x * x);
    return 0.5 * e;
}

#include "detail_frame.hlsli"

// Of one scale's coefficients' covariance `s` (L, Cr, Cb), what the
// shrinkage at thresholds `tl` (luminance) and `tc` (the colour pair's
// length) keeps.
float3x3 detail_retained(float3x3 s, float tl, float tc, out float3x3 gain)
{
    float3x3 r = 0;
    gain = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
    // Luminance.
    float slope = 1;
    if (s[0][0] > 0)
    {
        const float tau = tl / sqrt(s[0][0]), q = detail_tail(tau), phi = exp(-tau * tau / 2) / 2.5066283;
        r[0][0] = s[0][0] * 2 * ((1 + tau * tau) * q - tau * phi);
        slope = 2 * q;
    }
    // The colour pair, in its eigenframe.
    const float half_ = (s[1][1] + s[2][2]) / 2, d = (s[1][1] - s[2][2]) / 2, root = sqrt(d * d + s[1][2] * s[1][2]);
    const float l1 = half_ + root, l2 = max(half_ - root, 0.0);
    if (l1 > 0)
    {
        const float angle = detail_colour_angle(s[1][1], s[1][2], s[2][2]);
        const float2 v1 = float2(cos(angle), sin(angle)), v2 = float2(-v1.y, v1.x);
        const float4 m = detail_shrink_moments(tc / sqrt(l1), l2 / l1);
        const float2x2 kept = l1 * (m.x * float2x2(v1.x * v1, v1.y * v1) + m.y * float2x2(v2.x * v2, v2.y * v2));
        const float2x2 jac = m.z * float2x2(v1.x * v1, v1.y * v1) + m.w * float2x2(v2.x * v2, v2.y * v2);
        r[1][1] = kept[0][0], r[1][2] = r[2][1] = kept[0][1], r[2][2] = kept[1][1];
        const float2 cross = slope * mul(jac, float2(s[0][1], s[0][2]));
        r[0][1] = r[1][0] = cross.x, r[0][2] = r[2][0] = cross.y;
        gain[1][1] = jac[0][0], gain[1][2] = jac[0][1], gain[2][1] = jac[1][0], gain[2][2] = jac[1][1];
    }
    else
        r[0][1] = r[1][0] = slope * s[0][1], r[0][2] = r[2][0] = slope * s[0][2];
    gain[0][0] = slope;
    return r;
}

// A symmetric 3 x 3's eigenvalues (w) and eigenvectors (v's columns), by
// Jacobi's rotations (detail.cpp's symmetric_eigen()).
void detail_eigen(float3x3 m, out float3x3 v, out float3 w)
{
    float3x3 a = (m + transpose(m)) / 2;
    v = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
    [loop] for (int sweep = 0; sweep < 8; ++sweep)
        [unroll] for (int r = 0; r < 3; ++r)
        {
            const int p = r == 2 ? 1 : 0, q = r == 0 ? 1 : 2;
            if (a[p][q] == 0)
                continue;
            const float theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
            const float t = (theta >= 0 ? 1.0 : -1.0) / (abs(theta) + sqrt(theta * theta + 1)), c = 1 / sqrt(t * t + 1), s = t * c;
            [unroll] for (int k = 0; k < 3; ++k)
            {
                const float kp = a[k][p], kq = a[k][q];
                a[k][p] = c * kp - s * kq, a[k][q] = s * kp + c * kq;
            }
            [unroll] for (int k2 = 0; k2 < 3; ++k2)
            {
                const float pk = a[p][k2], qk = a[q][k2];
                a[p][k2] = c * pk - s * qk, a[q][k2] = s * pk + c * qk;
            }
            [unroll] for (int k3 = 0; k3 < 3; ++k3)
            {
                const float kp = v[k3][p], kq = v[k3][q];
                v[k3][p] = c * kp - s * kq, v[k3][q] = s * kp + c * kq;
            }
        }
    w = float3(a[0][0], a[1][1], a[2][2]);
}

// A symmetric 3 x 3 to a power, its eigenvalues below nothing taken as
// nothing (power 1: the nearest positive semi-definite); for a negative
// power, those under a millionth of the greatest as that (detail.cpp's
// symmetric_power()).
float3x3 detail_power(float3x3 m, float power)
{
    float3x3 v;
    float3 w;
    detail_eigen(m, v, w);
    const float top = max(max(w.x, w.y), max(w.z, 0.0));
    float3x3 result = 0;
    if (top <= 0)
        return result;
    [unroll] for (int k = 0; k < 3; ++k)
    {
        const float x = power < 0 ? max(w[k], 1e-6 * top) : max(w[k], 0.0);
        const float f = power == 1 ? x : pow(x, power);
        const float3 col = float3(v[0][k], v[1][k], v[2][k]);
        result += f * float3x3(col.x * col, col.y * col, col.z * col);
    }
    return result;
}

// Cholesky factor; the floor only covers a semidefinite roundoff endpoint.
float3x3 detail_cholesky(float3x3 a)
{
    float3x3 l = 0;
    const float top = max(max(a[0][0], a[1][1]), max(a[2][2], 1e-30));
    [unroll] for (int i = 0; i < 3; ++i)
        [unroll] for (int j = 0; j < 3; ++j)
            if (j <= i)
            {
                float x = (a[i][j] + a[j][i]) * .5;
                [unroll] for (int k = 0; k < 3; ++k)
                    if (k < j)
                        x -= l[i][k] * l[j][k];
                l[i][j] = i == j ? sqrt(max(x, top * 1e-12)) : x / l[j][j];
            }
    return l;
}

// Symmetric M B M=S from Cholesky factors and a one-sided Jacobi SVD:
// never form the squared-condition F F' (detail.cpp's transport()).
float3x3 detail_transport(float3x3 b, float3x3 s)
{
    const float3x3 l = detail_cholesky(b), r = detail_cholesky(s);
    float3x3 f = mul(transpose(l), r);
    [loop] for (int sweep = 0; sweep < 12; ++sweep)
        [unroll] for (int pair = 0; pair < 3; ++pair)
        {
            const int p = pair == 2 ? 1 : 0, q = pair == 0 ? 1 : 2;
            float aa = 0, bb = 0, ab = 0;
            [unroll] for (int k = 0; k < 3; ++k)
                aa += f[k][p]*f[k][p], bb += f[k][q]*f[k][q], ab += f[k][p]*f[k][q];
            if (abs(ab) <= 1e-7 * sqrt(aa * bb))
                continue;
            const float tau = (bb-aa)/(2*ab);
            const float t = (tau >= 0 ? 1.0 : -1.0)/(abs(tau)+sqrt(1+tau*tau));
            const float c = 1/sqrt(1+t*t), sn = c*t;
            [unroll] for (int k2 = 0; k2 < 3; ++k2)
            {
                const float a = f[k2][p], d = f[k2][q];
                f[k2][p] = c*a-sn*d, f[k2][q] = sn*a+c*d;
            }
        }
    float3x3 root = 0;
    [unroll] for (int k3 = 0; k3 < 3; ++k3)
    {
        float length2 = 0;
        [unroll] for (int i = 0; i < 3; ++i)
            length2 += f[i][k3]*f[i][k3];
        const float inv_length = length2 > 0 ? 1/sqrt(length2) : 0;
        [unroll] for (int i2 = 0; i2 < 3; ++i2)
            [unroll] for (int j = 0; j < 3; ++j)
                root[i2][j] += f[i2][k3]*f[j][k3]*inv_length;
    }
    float3x3 inv = 0;
    [unroll] for (int j2 = 0; j2 < 3; ++j2)
        [unroll] for (int i3 = 0; i3 < 3; ++i3)
            if (i3 >= j2)
            {
                float x = i3 == j2 ? 1.0 : 0;
                [unroll] for (int k4 = 0; k4 < 3; ++k4)
                    if (k4 >= j2 && k4 < i3)
                        x -= l[i3][k4]*inv[k4][j2];
                inv[i3][j2] = x/l[i3][i3];
            }
    const float3x3 m = mul(mul(transpose(inv), root), inv);
    return (m + transpose(m)) * .5;
}

// What the scales and the sharpening make of the noise of each pair of
// colours (RR GG BB RG RB GB; detail.hlsl's DETAIL_SHAPING, detail.cpp's
// noise_shaping()), measured per unit of the pair's deviations' product,
// else of its covariance at a pixel: each scale's (and the coarsest's) share
// (k), through the sharpening's high pass (d) and against it (x); the whole high pass's of the pixel's own (D, X); the colours'
// correlation at a pixel, measured (R-G R-B G-B; measured 0: the decode's);
// the sharpening's amount (0: none) and halo.
struct DetailShaping
{
    float k[6][6], d[6][6], x[6][6];
    float D[6], X[6];
    float3 correlation;
    float measured, amount, halo;
};

// Pair p's colours.
uint2 detail_pair(uint p)
{
    return p < 3 ? uint2(p, p) : p == 3 ? uint2(0, 1) : p == 4 ? uint2(0, 2) : uint2(1, 2);
}

// A symmetric matrix of the colours from a pair's six values.
float3x3 detail_pairs(float v[6])
{
    return float3x3(v[0], v[3], v[4], v[3], v[1], v[5], v[4], v[5], v[2]);
}

// Each colour's share kept, at level y: `law` its a (xyz) and b, the
// correlations (light's, readout's: R-G, R-B, G-B; unless measured), `tl`
// and `tc` each scale's thresholds (absolute, in the transformed domain),
// `w` the shaping. detail.cpp's noise_kept_of() says how.
float3 detail_kept(float3 y, float3 offset, float3 la, float3 lb, float3 ra, float3 rb, float tl[5], float tc[5], DetailShaping w)
{
    // The transform's slope squared, either side of 0 (detail_forward()).
    const float3 slope = 1 / (4 * (abs(y) + offset));
    const float3 va = la * max(y, 0.0) * slope, vb = lb * slope, v = va + vb;
    float3x3 e;
    e[0][0] = v.x, e[1][1] = v.y, e[2][2] = v.z;
    if (w.measured > 0)
    {
        e[0][1] = e[1][0] = w.correlation.x * sqrt(v.x * v.y);
        e[0][2] = e[2][0] = w.correlation.y * sqrt(v.x * v.z);
        e[1][2] = e[2][1] = w.correlation.z * sqrt(v.y * v.z);
    }
    else
    {
        e[0][1] = e[1][0] = ra.x * sqrt(va.x * va.y) + rb.x * sqrt(vb.x * vb.y);
        e[0][2] = e[2][0] = ra.y * sqrt(va.x * va.z) + rb.y * sqrt(vb.x * vb.z);
        e[1][2] = e[2][1] = ra.z * sqrt(va.y * va.z) + rb.z * sqrt(vb.y * vb.z);
    }
    const float3x3 to = float3x3(0.25, 0.5, 0.25, 1, -1, 0, 0, -1, 1);
    const float3x3 s = mul(to, mul(e, transpose(to)));
    // Each pair's unit: measured, its deviations' product; else its
    // covariance at a pixel. Each scale's coefficients' covariance (b_j), the
    // shrinkers' mean Jacobian G_j and what they keep past it, N_j = r_j -
    // G_j b_j G_j' (luminance's and the colour pair's apart); at the pixel,
    // each scale's share of its covariance, s_j = M b_j M (M the symmetric
    // positive definite M B M = S), through G_j, and N_j through M: positive
    // semi-definite scale by scale (detail.cpp's noise_kept_of()).
    const float3 sd3 = sqrt(max(v, 0.0));
    const float3x3 unit = w.measured > 0 ? float3x3(sd3.x * sd3, sd3.y * sd3, sd3.z * sd3) : e;
    // The sharpening's high pass: the whole high pass's of the pixel's own
    // noise (D, X), as much as the scales' shares through it keep; the colour
    // differences' shares spread as luminance's makes up the whole.
    float3x3 sum_b = 0, scale_b[6], scale_g[6], scale_r[6];
    float d_all = 0, d_kept = 0, x_all = 0, x_kept = 0;
    float xl[6];
    float2 xc[6], kc[6];
    [unroll] for (int j = 0; j < 6; ++j)
    {
        float kk[6], dd[6], xx[6];
        [unroll] for (uint p = 0; p < 6; ++p)
            kk[p] = w.k[j][p], dd[p] = w.d[j][p], xx[p] = w.x[j][p];
        const float3x3 ej = unit * detail_pairs(kk), ed = unit * detail_pairs(dd), ex = unit * detail_pairs(xx);
        const float3x3 bj = mul(to, mul(ej, transpose(to)));
        float3x3 gj = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
        const float3x3 rj = j < 5 ? detail_retained(bj, tl[min(j, 4)], tc[min(j, 4)], gj) : bj;
        scale_b[j] = bj;
        scale_g[j] = gj;
        scale_r[j] = rj;
        sum_b += bj;
        const float kl = bj[0][0] > 0 ? rj[0][0] / bj[0][0] : 1;
        const float dl = dot(to[0], mul(ed, to[0]));
        xl[j] = dot(to[0], mul(ex, to[0]));
        d_all += dl, d_kept += kl * dl, x_all += xl[j], x_kept += kl * xl[j];
        [unroll] for (uint c = 0; c < 2; ++c)
        {
            kc[j][c] = abs(bj[0][1 + c]) > 1e-30 ? rj[0][1 + c] / bj[0][1 + c] : 1;
            xc[j][c] = dot(to[0], mul(ex, to[1 + c]));
        }
    }
    // M: the one symmetric positive definite M B M = S (detail.cpp's).
    const float3x3 m = detail_transport(sum_b, s);
    float3x3 kept = 0;
    [loop] for (int j4 = 0; j4 < 6; ++j4)
    {
        const float3x3 share = mul(mul(m, scale_b[j4]), transpose(m));
        const float3x3 through = mul(mul(scale_g[j4], scale_b[j4]), transpose(scale_g[j4]));
        float3x3 past = 0;  // N_j: luminance's, and the colour pair's
        past[0][0] = scale_r[j4][0][0] - through[0][0];
        past[1][1] = scale_r[j4][1][1] - through[1][1], past[1][2] = scale_r[j4][1][2] - through[1][2];
        past[2][1] = scale_r[j4][2][1] - through[2][1], past[2][2] = scale_r[j4][2][2] - through[2][2];
        past = detail_power(past, 1);  // the moments' tables' rounding: not below nothing
        kept += mul(mul(scale_g[j4], share), transpose(scale_g[j4])) + mul(mul(m, past), transpose(m));
    }
    const float3x3 whole_d = unit * detail_pairs(w.D), whole_x = unit * detail_pairs(w.X);
    const float var_d = dot(to[0], mul(whole_d, to[0])) * (d_all > 0 ? d_kept / d_all : 1);
    const float cov_ld = dot(to[0], mul(whole_x, to[0])) * (x_all > 0 ? x_kept / x_all : 1);
    const float scale_x = x_all > 0 ? dot(to[0], mul(whole_x, to[0])) / x_all : 1;
    float2 cov_dc = 0;
    [unroll] for (uint c3 = 0; c3 < 2; ++c3)
    {
        float sum = 0;
        [unroll] for (int j1 = 0; j1 < 6; ++j1)
            sum += scale_x * xc[j1][c3];
        const float whole = dot(to[0], mul(whole_x, to[1 + c3]));
        [unroll] for (int j2 = 0; j2 < 6; ++j2)
            cov_dc[c3] += kc[j2][c3] * (scale_x * xc[j2][c3] + (x_all > 0 ? xl[j2] / x_all : 1.0 / 6) * (whole - sum));
    }
    if (w.amount > 0 && var_d > 0)
    {
        // E over d of h^2 / (h + |d|)^2 and d^2 h^2 / (h + |d|)^2.
        const float sd = sqrt(var_d), h = w.halo;
        float e1 = 0, e2 = 0, total = 0;
        for (int i = -20; i <= 20; ++i)
        {
            const float z = i * 0.25, p = exp(-0.5 * z * z), d = abs(z * sd), f = h / (h + d);
            e1 += p * f * f, e2 += p * d * d * f * f, total += p;
        }
        e1 /= total, e2 /= total;
        kept[0][0] += 2 * w.amount * e1 * cov_ld + w.amount * w.amount * e2;
        kept[0][1] += w.amount * e1 * cov_dc.x, kept[1][0] = kept[0][1];
        kept[0][2] += w.amount * e1 * cov_dc.y, kept[2][0] = kept[0][2];
        // What the sharpening's model adds kept a covariance: no direction of
        // it below nothing.
        kept = detail_power(kept, 1);
    }
    const float3x3 back = float3x3(1, 0.75, -0.25, 1, -0.25, -0.25, 1, -0.25, 0.75);
    float3 share_kept;
    [unroll] for (uint c2 = 0; c2 < 3; ++c2)
    {
        const float3 k = back[c2];
        const float before = dot(k, mul(s, k));
        share_kept[c2] = before > 0 ? dot(k, mul(kept, k)) / before : 1;
    }
    return share_kept;
}

// A stabilised colour `d` back to its level: f's own inverse, moved by the
// share of its noise taken out (1 - kept, detail_kept()) of the way to the
// expectation's inverse (`full`, the level whose E[f(X) | y] d is). Nothing
// taken out, f's own (encoding then decoding gives back what was there);
// all of it, the level whose average it is. Between, the bias of f's own
// inverse is, to first order (the delta method), in proportion to the
// variance taken out of the transformed value. What that and detail_kept()
// leave out (check_detail() measures it, flat grey under unequal
// Poisson-Gaussian noise, Luminance 0 to 100, Color 25 to 100): within 0.003
// of a pixel's noise in each colour and 0.0045 between them below a photon
// a pixel, the readout's noise most of it; at a few photons a pixel, where
// the transformed noise is still skewed and the shrinkers move its mean
// (beyond the second moments this models), to 0.019 and 0.031.
float detail_unbiased(float d, float offset, float full, float kept)
{
    const float own = detail_inverse(d, offset);
    return own + (1 - kept) * (full - own);
}

#endif
