#pragma once

#include <array>

// Noise reduction and sharpening, Lightroom's Detail panel: what the develop
// does to a pixel's neighbourhood rather than the pixel alone, so it works at
// the image's own resolution, on its scene-linear data (white balanced as
// shot), before every per-pixel control (adjust.h). The view does it on the
// GPU (shaders/detail.hlsl, GpuView: the level-0 region shown, and the full
// image once on the way to the smaller levels); saving and the command line
// on the CPU (apply_detail), the same filter step for step.
//
// Noise reduction is a wavelet shrinkage: the image in a variance-
// stabilised encoding (noise then has about the same size at every
// brightness, so one threshold suits the shadows and the midtones alike),
// split into luminance and two colour differences, then into the details
// at five scales (an "a trous" B3 spline, 1, 2, 4, 8 and 16 pixels apart),
// each detail shrunk towards 0 by a multiple of the noise at that scale,
// and put back together. The noise is measured from the image itself: the
// finest scale's median absolute detail in that encoding. Luminance is
// shrunk gently (its Detail spares the finest scales, its Contrast the
// middle ones); colour harder and across coarser scales (its Detail spares
// the finest, its Smoothness reaches the coarsest), as a colour difference
// vector, so hues aren't bent. Colour is smoothed guided by luminance: a
// tap of the spline counts towards the colour less the more its luminance
// differs from the centre's (in multiples of the luminance noise), so
// colour averages over what's evenly lit and stops at edges (an
// "edge-avoiding" a trous, Dammertz et al. 2010), rather than bleeding
// across them.
//
// The encoding: a raw's noise is the light's (Poisson: its variance grows
// with the level) and the readout's (Gaussian, the same everywhere):
// variance a x + b at level x. Both are fitted from the image itself, as
// Foi et al. do ("Practical Poissonian-Gaussian noise modeling and fitting
// for single-image raw data", 2008): blocks of 8 x 8 pixels (one every 16
// across and down), each colour's
// mean and variance (what's left after the four neighbours' mean, so smooth
// shading barely counts), the lower quartile of the blocks' variances at
// each level (texture only adds to a block's), and a straight line through
// those, weighed by how sure each level is and leaving out those far from
// it. b, where the sensor's masked border was read, is the readout's noise
// as measured there instead. Then a generalised Anscombe transform: up to a
// scale, sqrt(x + 3a/8 + b/a), green's for all three colours; below 0
// (the readout's noise around black, kept negative from the develop) the
// same curve turned about 0, so it stays monotonic and exactly invertible.
// Where there's no fit (too few flat areas; not a raw) it's each channel's
// square root as before, sqrt(x + kFloor), turned the same way below 0.
// Decoding, with a model, by its noise (shaders/detail.hlsli): each
// colour's pixel a Poisson-Gaussian mixture of that colour's own a and b
// (as a pixel has them through the develop); with nothing taken out the
// transform's own inverse (encode then decode is the identity), with all of
// it the level whose expected transformed value the result is (Makitalo and
// Foi's exact unbiased inverse), and between, by the share of each colour's
// transformed noise the shrinkage takes out (noise_kept(): a model of the
// shrinkage, its moments on the colours' covariance, over the scales as the
// noise's measured spatial correlation spreads it) and the sharpening adds
// back (it amplifies luminance's noise: kept over 1, the decode moves the
// other way). Where the develop's factors aren't ours to know (LibRaw's, an
// sRAW's or mRAW's in-camera scaling, a linear raw's), each colour's pixel
// variance against the fit's measure, and the colours' correlation, are
// measured too (measure_spatial()).
// Without a model, the transform's own inverse. The readout's b is the
// masked border's where the GPU develops a Bayer mosaic that has one; else
// (the CPU's data is LibRaw's develop, which brings no masked border) it's
// fitted, on the GPU as on the CPU.
//
// Luminance's shrinking also asks the next coarser scale whether there's
// something there: where its luminance has an edge, a fine detail is part of
// it and is shrunk less (by 1 + (gradient / 3 sigma)^2, sigma that scale's
// noise); a lone speck barely shows a scale up, and is shrunk in full.
//
// Sharpening is an unsharp mask of the luminance (after the noise is
// taken out, so it isn't sharpened): the detail a Gaussian of `radius`
// leaves, its large swings tempered (Detail: how much of an edge's swing
// gets through, so how much halo), kept off smooth areas (Masking).

struct Detail
{
    double sharpness = 0;           // Sharpness, 0..150: how much
    double sharpen_radius = 1;      // SharpenRadius, 0.5..3: pixels
    double sharpen_detail = 25;     // SharpenDetail, 0..100: halos let through
    double sharpen_masking = 0;     // SharpenEdgeMasking, 0..100: smooth areas spared
    double luminance = 0;           // LuminanceSmoothing, 0..100
    double luminance_detail = 50;   // LuminanceNoiseReductionDetail, 0..100
    double luminance_contrast = 0;  // LuminanceNoiseReductionContrast, 0..100
    double colour = 0;              // ColorNoiseReduction, 0..100
    double colour_detail = 50;      // ColorNoiseReductionDetail, 0..100
    double colour_smoothness = 50;  // ColorNoiseReductionSmoothness, 0..100

    // A raw's with nothing set, as Lightroom has it: Sharpness 40, Color 25.
    static Detail raw_defaults();
    bool active() const { return sharpness > 0 || luminance > 0 || colour > 0; }
    bool operator==(const Detail&) const = default;
};

// What the filter makes of the settings: the shader's parameters, and the
// CPU's.
struct DetailFilter
{
    static constexpr int kLevels = 5;  // the wavelet's scales
    // How far a result reaches for its inputs, in pixels: 2 x (1 + 2 + 4 +
    // 8 + 16) for the wavelet, 9 for the widest sharpening Gaussian, 1 for
    // the mask's gradient, and some to spare. A region filtered with this
    // much around it is what the whole image filtered would have there.
    static constexpr int kReach = 80;
    static constexpr float kFloor = 0.002f;  // the encoding without a noise model: sqrt(v + kFloor)
    // The GPU's noise measure (shaders/detail.hlsli): its words.
    static constexpr int kNoiseWords = 2 * 1024 + 2 + 32 + 3 * 64 * 128 + 3 * 1024 + 256 + 1 + 9 * 85 + 9 * 85 + 4;

    explicit DetailFilter(const Detail& d);
    bool denoise = false, sharpen = false;
    // Each scale's shrinkage threshold, a multiple of the finest scale's
    // noise (measured per image): luminance, colour.
    float luminance[kLevels] = {}, colour[kLevels] = {};
    // Each scale's support from the next coarser: its luminance gradient is
    // weighed against this multiple of the finest scale's noise (0: none).
    float support[kLevels] = {};
    float amount = 0;     // of the luminance detail added back
    float sigma = 1;      // the sharpening Gaussian's, in pixels
    float halo = 0;       // a detail swing of this much gets through half
    float mask_low = 0, mask_high = 0;  // the luminance gradient the mask opens over; 0: no mask
    // Colour's smoothing fades over luminance differences of this many
    // times the luminance noise (a Gaussian's sigma); 0: not guided.
    float guide = 0;
    // Encoded by a noise model fitted to the image (a raw's), else as
    // without one.
    bool model = false;
};

// A raw's noise, fitted from linear RGB (white balanced, 1 = white): each
// colour's variance a x + b at level x, in the fit's blocks' measure (bit c
// of fitted_colours: colour c's fitted), and the encoding green's gives (its
// offset, for all three colours: one curve keeps noise they share, a
// demosaic's or an sRAW's luma, cancelling in their differences); unfitted,
// the encoding without a model. Then, for the decode, each colour's
// expectation inverse by its own law (tabulate_noise; `light` and
// `readout`, each colour's pixel noise against the blocks' measure through
// the develop that made it, its a's and its b's: for LibRaw's, whose
// interpolation isn't ours to know, `pixel`, measured).
struct NoiseModel
{
    bool fitted = false;
    int fitted_colours = 0;
    float a[3] = {}, b[3] = {};
    float offset[3] = {DetailFilter::kFloor, DetailFilter::kFloor, DetailFilter::kFloor};
    float light[3] = {1, 1, 1}, readout[3] = {1, 1, 1};
    // Each colour's measure against green's through the develop, the light's
    // and the readout's: what a colour with no fit of its own takes green's by.
    float green_light[3] = {1, 1, 1}, green_readout[3] = {1, 1, 1};
    // The colours' noise correlations after the develop, R-G R-B G-B, the
    // light's and the readout's (0: independent): unmeasured, the decode's.
    float correlation_light[3] = {}, correlation_readout[3] = {};
    // The noise's spatial correlation, measured over the image's flat parts
    // (measure_spatial(); shaders/noise.hlsl's): each colour against each
    // (spatial_series()) at the lags within kSpatialReach (a half plane:
    // spatial_lag()). A develop's interpolation, an sRAW's or mRAW's
    // in-camera scaling and its shared colour make neighbours alike: how much
    // of the noise the wavelet's scales and the sharpening see, and
    // (`pixel`) each colour's pixel variance against the fit's measure,
    // which a neighbour like the pixel hides. Each pair of colours' own
    // (an mRAW's luma, shared by all three, alike only a pixel or two apart;
    // its colour, filled over blocks, alike much further): how much of each
    // pair's covariance each scale has. Unmeasured, independent pixel to
    // pixel.
    static constexpr int kSpatialReach = 6, kSpatialLags = 85, kSpatialSeries = 9;
    bool spatial = false;
    float rho[kSpatialSeries][kSpatialLags] = {};
    float pixel[3] = {1, 1, 1};
    // Each colour's pixel law, a and b (tabulate_noise()).
    float law_a[3] = {}, law_b[3] = {};
    static constexpr int kInverse = 1024;
    float from[3] = {}, to[3] = {};
    float inverse[3][kInverse] = {};
};
// Lag i of NoiseModel::rho: (dx, dy). Its series for colour c at a pixel
// against colour d at the lag: R G B against themselves, then R-G G-R R-B
// B-R G-B B-G.
void spatial_lag(int i, int& dx, int& dy);
int spatial_series(int c, int d);
NoiseModel fit_noise(const float* rgb, int width, int height);
// The spatial correlation measured into `m` (fitted: its encoding), and
// `pixel` from it.
void measure_spatial(NoiseModel& m, const float* rgb, int width, int height);
void tabulate_noise(NoiseModel& m);
// What the scales and the sharpening make of the noise of each pair of
// colours (shaders/detail.hlsli's DetailShaping), measured per unit of the
// pair's deviations' product, else of its covariance at a pixel: each scale's
// (and the coarsest's) share (k), through the sharpening's high pass (d) and
// against it (x); the whole high pass's of the pixel's own
// (D, X); the colours' correlation at a pixel, measured (R-G R-B G-B; else
// the develop's); the sharpening (amount 0: none).
struct NoiseShaping
{
    float k[6][3][3] = {}, d[6][3][3] = {}, x[6][3][3] = {};
    float D[3][3] = {}, X[3][3] = {};
    bool measured = false;
    float correlation[3] = {};
    float amount = 0, halo = 0;
};
NoiseShaping noise_shaping(const NoiseModel* m, const DetailFilter* f);  // null: independent noise; no sharpening
// The pieces of the decode (shaders/detail.hlsli's), for --check-detail.
float noise_expected(float y, float offset, float a, float b);
float noise_expectation_inverse(const NoiseModel& m, float d, int c);
// The share of each colour's transformed noise noise reduction keeps:
// from the colours' covariance `e` there and each scale's thresholds (tl
// luminance's, tc the colour pair's), and at each colour's level y by the
// model's law.
std::array<std::array<float, 3>, 3> noise_retained(const std::array<std::array<float, 3>, 3>& s, float tl, float tc,
                                                   std::array<std::array<float, 3>, 3>* gain = nullptr);
void noise_kept_of(const std::array<std::array<float, 3>, 3>& e, const float tl[5], const float tc[5], const NoiseShaping& shaping, float out[3]);
void noise_kept(const NoiseModel& m, const float y[3], const float tl[5], const float tc[5], const NoiseShaping& shaping, float out[3]);
float noise_decode(const NoiseModel& m, float d, int c, float kept);
// The decode as each pixel has it: the shift from f's own inverse by level,
// tabulated once per image and thresholds on the inverse tables' entries
// (the share kept at each entry's level taken for all three colours: a
// neutral pixel's), then looked up.
void noise_decode_table(const NoiseModel& m, const float tl[5], const float tc[5], const NoiseShaping& shaping, float shift[3][NoiseModel::kInverse]);
float noise_decode_by(const NoiseModel& m, const float shift[3][NoiseModel::kInverse], float d, int c);
// --check-detail's (and GpuView::check_shaping()'s) off-centre case: a
// fitted model whose colours are independent, or green red's a pixel on (R(x)
// = Z(x), G(x) = Z(x + 1)), not correlated at a pixel but a lag on; and each
// colour's variance after sharpening noise of variance 1 by the kernel itself.
NoiseModel shaping_case(bool shifted);
// --check-detail quiet-axis conditioning regression, shared with the GPU check.
NoiseModel quiet_axis_case();
void sharpened_exactly(bool shifted, float amount, float sigma, double out[3]);

// The CPU's: linear RGB (3 floats a pixel, rows of `width`, white 1) through
// the filter into `out`, of the same layout.
void apply_detail(const DetailFilter& f, const float* rgb, int width, int height, float* out);

// --check-detail: the noise model's decode against its own model (both
// ends of the shrinkage, each colour's own law, the pure-Poisson limit, a
// fit of linear RGB) and continuous in the sliders. 0 when all pass.
int check_detail();
