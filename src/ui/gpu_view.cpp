#include "gpu_view.h"

#include <glib.h>
#include "../compare.h"
#include "../decoders/decoder.h"
#include "../decoders/icc.h"
#include "../decoders/ljpeg.h"
#include "../decoders/rawblocks.h"
#include "../decoders/rawpacked.h"
#include "../gpu.h"
#include "../half.h"
#include "../image.h"
#include "../raw_linearity.h"
#include "../shaders/drift.hlsli"
#include "../shaders/clip_layout.hlsli"
#include "../shaders/stats_layout.hlsli"
#include "../white_balance.h"
#include "shaders/clip.h"
#include "shaders/clip_find.h"
#include "shaders/clip_combine.h"
#include "shaders/clip_drift.h"
#include "shaders/clip_limit.h"
#include "shaders/clip_linear.h"
#include "shaders/clip_ycc.h"
#include "shaders/compose.h"
#include "shaders/decode.h"
#include "shaders/detail.h"
#include "shaders/develop.h"
#include "shaders/develop_linear.h"
#include "shaders/develop_correct.h"
#include "shaders/develop_impulse.h"
#include "shaders/develop_drift.h"
#include "shaders/develop_drift_linear.h"
#include "shaders/develop_drift_ycc.h"
#include "shaders/develop_pile.h"
#include "shaders/develop_pile_linear.h"
#include "shaders/develop_pile_ycc.h"
#include "shaders/develop_gather.h"
#include "shaders/develop_gather_linear.h"
#include "shaders/develop_gather_ycc.h"
#include "shaders/develop_green.h"
#include "shaders/develop_ycc.h"
#include "shaders/difference.h"
#include "shaders/highlight_down.h"
#include "shaders/highlight_relax.h"
#include "shaders/highlight_relax_ratios.h"
#include "shaders/highlight_up.h"
#include "shaders/histogram.h"
#include "shaders/histogram_max.h"
#include "shaders/histogram_view.h"
#include "shaders/ljpeg_black.h"
#include "shaders/ljpeg_columns.h"
#include "shaders/ljpeg_decode.h"
#include "shaders/ljpeg_guess.h"
#include "shaders/ljpeg_resolve.h"
#include "shaders/ljpeg_rows.h"
#include "shaders/ljpeg_sync.h"
#include "shaders/mosaic.h"
#include "shaders/mosaic_linear.h"
#include "shaders/noise.h"
#include "shaders/peak.h"
#include "shaders/raw_blocks.h"
#include "shaders/resample.h"
#include "shaders/stats.h"
#include "shaders/stats_top.h"
#include "shaders/stats_curve.h"
#include "shaders/volume_curve.h"
#include "shaders/unpack.h"
#include "shaders/volume.h"
#include "shaders/volume_clear.h"
#include "shaders/volume_prepare.h"
#include "shaders/warp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// ljpeg.hlsli's block.
struct LjpegParams
{
    uint32_t parts, chunk_bits, chunks, components;
    uint32_t samples, row, rows, precision;
    uint32_t readout_w, readout_h, slice_n, slice_w;
    uint32_t slice_last, left, top, filters;
    int32_t rect[4];
    uint32_t mosaic_w, mosaic_h, scheme, curved;
    uint32_t initial[4];
    uint32_t tile_w, tile_h, tiles_across, planes;
    uint32_t quads, unused[3];
};
static_assert(sizeof(LjpegParams) == 144, "ljpeg.hlsli's layout");

namespace
{

// The shaders' parameter blocks, as their cbuffers have them.
struct DecodeParams
{
    int32_t mode, channels, encoded, is_signed;
};

struct ResampleParams
{
    int32_t axis, filter;
    float inv, offset;
    int32_t size, other;
    float unused[2];
};

// detail.hlsl's and noise.hlsl's.
struct DetailParams
{
    int32_t mode, axis, step, sharpening;
    int32_t origin[2], size[2], out_origin[2], image_size[2];
    float as_shot[4], luminance[4], colour[4], coarsest[4], sharpen[4], mask[4], support[4], model[4];
};

struct NoiseParams
{
    int32_t mode, size[2], fitting;
    float as_shot[4];
    int32_t read[4];
    float light[4], readout[4];  // develop_noise()'s: a pixel's noise against the fit's measure, the light's and the readout's
    float green_light[4], green_readout[4];  // and each colour's measure against green's (a colour with no fit of its own)
    float measure[3][4];         // and V, the masked border's photosite noise to the measure (a Bayer mosaic's)
    float correlation_light[4], correlation_readout[4];  // develop_noise()'s: the colours' noise correlations, R-G R-B G-B
};

// The noise fit measures each colour after the develop by what's left
// after the four neighbours' mean (noise.hlsl); a pixel's own variance is
// more, as the develop's interpolation made neighbours alike. Both are sums
// over the photosites that went into the pixel, each colour's in proportion
// to its own variance s_k: pixel P_c = sum_k W_ck s_k, measured M_c = sum_k
// V_ck s_k (tools/noise_paths.py derives them). The model of s: one sensor
// under every colour (one photodiode and readout, a and b the same for
// each), so the photosites' variances differ by colour only as the white
// balance, applied before the interpolation, scales them: the light's (a) by
// mul_k, the readout's (b) by mul_k squared. Each colour's fitted a and b
// (in M's measure) go to a pixel's by P_c / M_c at those s_k, the light's
// and the readout's apart; a colour too sparse to fit takes green's, by
// M_c / M_g at those s_k (develop_noise()). Not s = V^-1 M from the three
// fits: R's and B's rows of V are near mirror images, so W V^-1 leans on
// the fits' differences (its rows' absolute sums 8.2, 5.4, 8.2), and a few
// percent of fitting error in one colour becomes several times that in
// another's law.
//  - A Bayer mosaic's Malvar-He-Cutler is linear: W and V exactly, from its
//    weights (rows R G B out, columns R G B in); and X, the output colours'
//    covariances from each input colour's photosites, so their
//    correlations at those s_k.
//  - X-Trans's weights follow the data, so no fixed W and V are a model of
//    it (each colour's photosites alone through it, then summed, put a
//    pixel's red 55% over the whole's): P_c / M_c and M_c / M_g simulated
//    through its own interpolation with every colour's noise there, over a
//    grid of red's and blue's photosite noise against green's (0.5 to 3, R
//    by B), between them bilinearly; its correlations at equal noise (at
//    twice red's, 0.56 0.45 0.71: within 0.14).
//  - Canon's sRAW and mRAW: luma a pixel, colour filled from 25 blocks
//    around. Our fill alone would leave luma's noise as it is (these
//    factors, near 1, and the colours almost wholly correlated); but the
//    camera's own scaling to an mRAW made neighbours' luma alike (0.55 a
//    pixel apart in a Canon's at ISO 12800), which hides 3.5 times over
//    the pixel's variance from the measure. Not ours to know: measured
//    instead, from the noise's spatial correlation (measured_law, noise.hlsl's
//    mode 6), the colours' correlation with it.
//  - A linear raw, a monochrome sensor's: none (no interpolation); measured
//    too, as LibRaw's develop is.
constexpr float kBayerW[3][3] = {{0.5625f, 0.2461f, 0.1758f}, {0.0781f, 0.6250f, 0.0781f}, {0.1758f, 0.2461f, 0.5625f}};
constexpr float kBayerV[3][3] = {{0.0812f, 0.2172f, 0.1617f}, {0.0719f, 0.2563f, 0.0719f}, {0.1617f, 0.2172f, 0.0813f}};
constexpr float kXtransGrid[6] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f};
constexpr float kXtransRatio[36][3] = {{1.229f, 1.356f, 1.231f}, {1.221f, 1.340f, 1.414f}, {1.216f, 1.326f, 1.620f}, {1.209f, 1.305f, 2.006f}, {1.204f, 1.291f, 2.296f}, {1.202f, 1.273f, 2.637f}, {1.405f, 1.341f, 1.224f}, {1.392f, 1.324f, 1.400f}, {1.383f, 1.310f, 1.601f}, {1.369f, 1.289f, 1.975f}, {1.358f, 1.274f, 2.256f}, {1.347f, 1.257f, 2.583f}, {1.602f, 1.328f, 1.219f}, {1.583f, 1.310f, 1.391f}, {1.569f, 1.296f, 1.586f}, {1.547f, 1.275f, 1.949f}, {1.531f, 1.260f, 2.219f}, {1.510f, 1.243f, 2.537f}, {1.967f, 1.308f, 1.213f}, {1.934f, 1.291f, 1.377f}, {1.908f, 1.276f, 1.563f}, {1.871f, 1.255f, 1.907f}, {1.845f, 1.239f, 2.165f}, {1.810f, 1.222f, 2.468f}, {2.243f, 1.295f, 1.210f}, {2.201f, 1.277f, 1.368f}, {2.167f, 1.263f, 1.547f}, {2.117f, 1.241f, 1.877f}, {2.081f, 1.225f, 2.124f}, {2.032f, 1.208f, 2.420f}, {2.566f, 1.279f, 1.210f}, {2.513f, 1.262f, 1.359f}, {2.469f, 1.247f, 1.527f}, {2.404f, 1.225f, 1.838f}, {2.358f, 1.210f, 2.070f}, {2.294f, 1.193f, 2.350f}};
constexpr float kXtransGreen[36][2] = {{0.895f, 0.896f}, {0.896f, 0.986f}, {0.896f, 1.111f}, {0.894f, 1.461f}, {0.890f, 1.951f}, {0.884f, 3.320f}, {0.984f, 0.898f}, {0.986f, 0.989f}, {0.987f, 1.113f}, {0.984f, 1.467f}, {0.981f, 1.959f}, {0.974f, 3.336f}, {1.108f, 0.898f}, {1.112f, 0.989f}, {1.113f, 1.114f}, {1.111f, 1.470f}, {1.107f, 1.966f}, {1.099f, 3.348f}, {1.465f, 0.897f}, {1.471f, 0.988f}, {1.474f, 1.115f}, {1.473f, 1.474f}, {1.466f, 1.973f}, {1.454f, 3.360f}, {1.955f, 0.895f}, {1.963f, 0.986f}, {1.968f, 1.114f}, {1.968f, 1.476f}, {1.961f, 1.976f}, {1.945f, 3.358f}, {3.337f, 0.890f}, {3.353f, 0.982f}, {3.361f, 1.110f}, {3.362f, 1.472f}, {3.351f, 1.969f}, {3.323f, 3.345f}};
constexpr float kYccRatio[3] = {1.035f, 1.008f, 1.037f};
constexpr float kBayerX[3][3][3] = {{{0.5625f, 0.2461f, 0.1758f}, {0.1250f, 0.3125f, 0.1172f}, {0.1875f, 0.2109f, 0.1875f}},
                                    {{0.1250f, 0.3125f, 0.1172f}, {0.0781f, 0.6250f, 0.0781f}, {0.1172f, 0.3125f, 0.1250f}},
                                    {{0.1875f, 0.2109f, 0.1875f}, {0.1172f, 0.3125f, 0.1250f}, {0.1758f, 0.2461f, 0.5625f}}};
constexpr float kXtransCorrelation[3] = {0.697f, 0.547f, 0.695f};
constexpr float kYccCorrelation[3] = {0.953f, 0.960f, 0.967f};

// A raw's develop's pixel noise against the fit's measure, each colour's,
// for its a (`light`) and its b (`readout`); each colour's measure against
// green's (`green`, the light's and the readout's: what a colour too sparse
// to fit takes green's by); the colours' correlations after it, the
// light's and the readout's (R-G, R-B, G-B); and (a Bayer mosaic's) V,
// which takes the masked border's photosite noise to the measure.
void develop_noise(const RawMosaic& m, float light[3], float readout[3], float green[2][3], float correlation[2][3], float measure[3][3])
{
    std::memset(measure, 0, sizeof(float) * 9);
    std::memset(correlation, 0, sizeof(float) * 6);
    for (int c = 0; c < 3; ++c)
        light[c] = readout[c] = green[0][c] = green[1][c] = 1;
    if (m.ycc.block_h)
    {
        for (int c = 0; c < 3; ++c)
            light[c] = readout[c] = kYccRatio[c], correlation[0][c] = correlation[1][c] = kYccCorrelation[c];
        return;
    }
    if (m.channels == 3 || m.grey)
        return;
    const float g = m.as_shot[1] > 0 ? m.as_shot[1] : 1;
    const float mul[3] = {m.as_shot[0] / g, 1, m.as_shot[2] / g};
    if (m.period == 6)
    {
        for (int c = 0; c < 3; ++c)
            correlation[0][c] = correlation[1][c] = kXtransCorrelation[c];
        // The grid's bilinear lookup at red's and blue's noise against green's.
        const auto at = [](float v, int& i, float& t)
        {
            v = std::clamp(v, kXtransGrid[0], kXtransGrid[5]);
            i = 0;
            while (i < 4 && v > kXtransGrid[i + 1])
                ++i;
            t = (v - kXtransGrid[i]) / (kXtransGrid[i + 1] - kXtransGrid[i]);
        };
        for (int part = 0; part < 2; ++part)
        {
            const float r = part ? mul[0] : std::sqrt(mul[0]), b = part ? mul[2] : std::sqrt(mul[2]);  // standard deviations
            int i, j;
            float t, u;
            at(r, i, t), at(b, j, u);
            float* out = part ? readout : light;
            for (int c = 0; c < 3; ++c)
            {
                const auto q = [&](int ri, int bi) { return kXtransRatio[ri * 6 + bi][c]; };
                out[c] = (1 - t) * ((1 - u) * q(i, j) + u * q(i, j + 1)) + t * ((1 - u) * q(i + 1, j) + u * q(i + 1, j + 1));
            }
            for (int k = 0; k < 2; ++k)
            {
                const auto q = [&](int ri, int bi) { return kXtransGreen[ri * 6 + bi][k]; };
                green[part][k * 2] = (1 - t) * ((1 - u) * q(i, j) + u * q(i, j + 1)) + t * ((1 - u) * q(i + 1, j) + u * q(i + 1, j + 1));
            }
        }
        return;
    }
    for (int part = 0; part < 2; ++part)
    {
        float* out = part ? readout : light;
        float v[3] = {};
        for (int c = 0; c < 3; ++c)
        {
            float p = 0;
            for (int k = 0; k < 3; ++k)
            {
                const float s = part ? mul[k] * mul[k] : mul[k];
                p += kBayerW[c][k] * s, v[c] += kBayerV[c][k] * s;
            }
            out[c] = p / v[c];
        }
        for (int c = 0; c < 3; ++c)
            green[part][c] = v[c] / v[1];
        static constexpr int pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
        for (int q = 0; q < 3; ++q)
        {
            const int c = pairs[q][0], d = pairs[q][1];
            float cd = 0, cc = 0, dd = 0;
            for (int k = 0; k < 3; ++k)
            {
                const float s = part ? mul[k] * mul[k] : mul[k];
                cd += kBayerX[c][d][k] * s, cc += kBayerX[c][c][k] * s, dd += kBayerX[d][d][k] * s;
            }
            correlation[part][q] = cd / std::sqrt(cc * dd);
        }
    }
    std::memcpy(measure, kBayerV, sizeof kBayerV);
}

struct ComposeParams
{
    float background[4], checker_a[4], checker_b[4], image_rect[4];
    int32_t nearest, filter;
    float inv, offset, offset_x;
    int32_t column, rows, checker_size;
    float inv_x;
    int32_t columns;
    float exposure;
    int32_t tone, output;
    float white;
    int32_t measured;
    float display_peak;
    float display_r[2], display_g[2], display_b[2];
    int32_t show_histogram;
    uint32_t histogram_channels;
    float panel[4];
    float input_x[4], input_y[4], input_z[4];
    int32_t auto_exposure, gain, adjusting;
    float anchor;
    float to_working[3][4], from_working[3][4];
    float lc_weight;
    float min_slope;
    int32_t hk_probe;
    float white_luminance;  // (the row's fourth: view.hlsli packs it there)
    int32_t treated;
    int32_t passthrough;
    float pad_[2];
};
static_assert(sizeof(ComposeParams) == 368, "as view.hlsli's cbuffer packs");

struct DevelopParams
{
    uint32_t cfa[4];   // each site's LibRaw colour, 2 bits, 16 a word; w: the period
    float as_shot[4];  // w: the white the file states
    int32_t map_x[4], map_y[4];
    int32_t size[4];
    int32_t ycc[4];     // YCC: block height, formula, hue, luma offset
    int32_t ycc_at[4];  // YCC: the first block's x, y
    float ycc_mul[4];   // YCC: sraw_mul; w: the guided fill's regularisation
    int32_t flags[4];   // x: the data is white balanced already; y: grey, no filter
    float black[36];    // per site of the pattern
};
static_assert(sizeof(DevelopParams) == 288, "develop.hlsl's layout");

// warp.hlsl's block.
struct WarpParams
{
    int32_t map_x[4], map_y[4];
    int32_t size[4];
};

// raw_blocks.hlsl's block.
struct BlocksParams
{
    uint32_t format, bits, per, units;
    uint32_t blocks, readout_w, row_blocks, span;
    uint32_t rotated, rotation, size, unused;
};

// unpack.hlsl's block.
struct UnpackParams
{
    uint32_t readout_w, readout_h, row_bits, bits;
    uint32_t group, shift, swap, curved;
    uint32_t lead, words, unused[2];
};
static_assert(sizeof(UnpackParams) == 48, "unpack.hlsl's layout");

// BT.709 (sRGB) linear to XYZ, D65.
constexpr float kXyzFrom709[3][3] = {
    {0.4124564f, 0.3575761f, 0.1804375f}, {0.2126729f, 0.7151522f, 0.0721750f}, {0.0193339f, 0.1191920f, 0.9503041f}};

// sRGB's primaries and white in u'v' (CIE 1976 UCS): the display's until
// rapport describes the real one.
constexpr float kSrgbR[2] = {0.4507f, 0.5229f}, kSrgbG[2] = {0.1250f, 0.5625f}, kSrgbB[2] = {0.1754f, 0.1579f};

float linear(uint8_t v)
{
    const double c = v / 255.0;
    return float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}

void set_linear(float out[4], rap::color c)
{
    out[0] = linear(c.r), out[1] = linear(c.g), out[2] = linear(c.b), out[3] = 1;
}

template <class T>
std::span<const std::byte> bytes(const T& v)
{
    return std::as_bytes(std::span(&v, 1));
}

// The filter's reach in texels, at `inv` texels per output pixel.
double reach(ScaleFilter f, double inv)
{
    return (f == ScaleFilter::Lanczos3 ? 3.0 : f == ScaleFilter::Area ? 0.5 : 2.0) * std::max(inv, 1.0);
}

}  // namespace

static float display_peak();
static std::vector<float> boundary_table(float peak);

GpuView::GpuView(std::shared_ptr<rap::gpu::device> device) : device_(std::move(device))
{
    // A GPU in software decodes a raw's stream slower than the CPU's cores:
    // the loader unpacks them from now on (the ones it already prepared are
    // decoded on the CPU here, decode_stream).
    // (IFRIT_GPU_DECODE_ON_SOFTWARE keeps the GPU's decode, for testing it there.)
    static const bool keep = std::getenv("IFRIT_GPU_DECODE_ON_SOFTWARE") != nullptr;
    if (device_->software() && !keep)
    {
        DecodeOptions o = decode_options();
        if (o.gpu_decode)
        {
            o.gpu_decode = false;
            set_decode_options(o);
        }
    }
}

void GpuView::read_details(std::function<void(const Details&)> done)
{
    if (!histogram_generation_ || !histogram_stats_)
        return;
    struct Read
    {
        std::vector<std::byte> stats = std::vector<std::byte>(kStatsWords * 4), histogram = std::vector<std::byte>(1032 * 4);
    };
    auto read = std::make_shared<Read>();
    const bool scene = histogram_scene_;
    const float exposure = histogram_scale_;
    const bool automatic = histogram_auto_;
    const float fixed = histogram_fixed_;
    device_->readback(histogram_stats_, read->stats);
    device_->readback(histogram_, read->histogram,
                      [read, device = device_, scene, exposure, automatic, fixed, done = std::move(done)]
                      {
                          if (device->lost())  // a failed readback leaves them as they were: nothing measured
                          {
                              Details unknown;
                              unknown.known = false;
                              done(unknown);
                              return;
                          }
                          const auto word = [](const std::vector<std::byte>& v, size_t i)
                          {
                              uint32_t w;
                              std::memcpy(&w, v.data() + i * 4, 4);
                              return w;
                          };
                          const auto as_float = [](uint32_t w)
                          {
                              float f;
                              std::memcpy(&f, &w, 4);
                              return f;
                          };
                          Details d;
                          d.scene = scene;
                          const uint32_t count = word(read->stats, kStatsCount);
                          if (count)
                          {
                              float scale = exposure;
                              if (fixed > 0)
                                  scale *= fixed;  // a treated raw: the camera's, as auto_gain() has it
                              else if (automatic && as_float(word(read->stats, kStatsAutoGain)) > 0)
                                  scale *= as_float(word(read->stats, kStatsAutoGain));  // as auto_gain() has it
                              d.minimum = as_float(~word(read->stats, kStatsMinimum)) * scale;
                              d.maximum = as_float(word(read->stats, kStatsMaximum)) * scale;
                              d.peak = as_float(word(read->stats, kStatsPeak)) * scale;
                              // The mean from the log2 histogram's bin centres (to a couple of percent).
                              double sum = 0;
                              for (int bin = 0; bin < 1024; ++bin)
                                  sum += double(word(read->stats, kStatsLuminance + bin)) * std::exp2((bin + 0.5) * 32 / 1024 - 20);
                              d.average = float(sum / count) * scale;
                          }
                          // Clipped as shown: R, G or B at an end (each channel's share, the largest).
                          uint64_t total = 0;
                          for (int b = 0; b < 256; ++b)
                              total += word(read->histogram, size_t(768 + b));
                          if (total)
                              for (int ch = 0; ch < 3; ++ch)
                              {
                                  d.black = std::max(d.black, float(word(read->histogram, size_t(ch * 256))) / float(total));
                                  d.white = std::max(d.white, float(word(read->histogram, size_t(ch * 256 + 255))) / float(total));
                              }
                          done(d);
                      });
}

void GpuView::edit_of(const Image& image, Frame& f)
{
    f.exposure = image.tonable() ? image.exposure_edit : 0.0;
    std::fill(std::begin(f.white_balance), std::end(f.white_balance), 0.0f);
    if (image.mosaic && image.wb_kelvin > 0)
        wb_multipliers(*image.mosaic, image.wb_kelvin, image.wb_tint, f.white_balance);
    f.adjust = image.adjust_tables();
    f.treated = image.adjust.treated;
    f.detail = image.adjust.detail;
}

bool GpuView::export_pixels(const std::shared_ptr<Image>& image, uint64_t generation, PixelBuffer& out, std::string& error)
{
    // The view's own frame at 1:1, the whole image, as edited, then mapped
    // to an 8-bit sRGB file: the SDR output mapping, its peak at SDR white
    // whatever the display (the highlights rolled off, not clipped).
    Frame f;
    f.width = image->width;
    f.height = image->height;
    f.zoom = 1;
    edit_of(*image, f);
    f.background = rap::color{0, 0, 0};
    f.enlarge = f.shrink = ScaleFilter::Nearest;
    f.front = FrontBuffer::Srgb8;
    rap::gpu::texture_ptr frame;
    rap::gpu::ticket ready;
    rap::frame_color colour;
    if (!render(image, generation, f, frame, ready, colour, error))
        return false;
    std::vector<std::byte> rgba(size_t(f.width) * f.height * 4);
    device_->readback(frame, rgba, size_t(f.width) * 4).wait();
    if (device_->lost())
    {
        error = "the GPU was lost reading the image back";
        return false;
    }
    out.resize(rgba.size());
    for (size_t i = 0; i < rgba.size(); i += 4)  // RGBA to BGRA, opaque
    {
        out[i] = uint8_t(rgba[i + 2]);
        out[i + 1] = uint8_t(rgba[i + 1]);
        out[i + 2] = uint8_t(rgba[i]);
        out[i + 3] = 255;
    }
    return true;
}

bool GpuView::draw_histogram(int width, int height, int channels, rap::gpu::texture_ptr& out, rap::gpu::ticket& ready)
{
    if (!histogram_generation_ || width <= 0 || height <= 0 || !histogram_view_)
        return false;
    rap::gpu::texture_ptr& t = panels_[next_panel_];
    if (!t || t->width() != width || t->height() != height)
        t = device_->create({width, height, rap::gpu::format::rgba8, 1, true});
    if (!t)
        return false;
    next_panel_ ^= 1;
    struct
    {
        float panel[4];
        uint32_t channels, unused[3];
    } params{{0, 0, float(width), float(height)}, uint32_t(channels), {}};
    rap::gpu::pass p;
    p.program = histogram_view_;
    p.buffers = {histogram_, histogram_};  // t8 unused, t9 the counts
    p.uniforms = std::as_bytes(std::span(&params, 1));
    p.target = t;
    ready = device_->run(std::span(&p, 1));
    out = t;
    return true;
}

bool GpuView::programs(std::string& error)
{
    if (compose_)
        return true;
    // A program and the shader it's made from (a function pointer, spelt out:
    // GCC 14 deduces std::pair's from a function's type, not its pointer).
    using entry = std::pair<rap::gpu::program_ptr*, decltype(&shaders::decode)>;
    for (auto [program, make] : {entry{&decode_, shaders::decode}, entry{&resample_, shaders::resample},
                                 entry{&compose_, shaders::compose}, entry{&difference_program_, shaders::difference},
                                 entry{&histogram_view_, shaders::histogram_view},
                                 entry{&develop_, shaders::develop}, entry{&develop_correct_, shaders::develop_correct},
                                 entry{&develop_impulse_, shaders::develop_impulse},
                                 entry{&develop_green_, shaders::develop_green},
                                 entry{&develop_gather_, shaders::develop_gather},
                                 entry{&develop_gather_linear_, shaders::develop_gather_linear},
                                 entry{&develop_gather_ycc_, shaders::develop_gather_ycc},
                                 entry{&highlight_down_, shaders::highlight_down}, entry{&highlight_relax_, shaders::highlight_relax}, entry{&highlight_relax_ratios_, shaders::highlight_relax_ratios},
                                 entry{&highlight_up_, shaders::highlight_up},
                                 entry{&develop_linear_, shaders::develop_linear},
                                 entry{&develop_ycc_, shaders::develop_ycc}, entry{&mosaic_, shaders::mosaic},
                                 entry{&mosaic_linear_, shaders::mosaic_linear},
                                 entry{&warp_, shaders::warp}, entry{&detail_, shaders::detail}})
    {
        auto r = device_->program(make());
        if (!r.program)
        {
            error = r.error;
            return false;
        }
        *program = r.program;
    }
    for (auto [program, make] :
         {entry{&stats_program_, shaders::stats}, entry{&stats_curve_, shaders::stats_curve}, entry{&stats_top_, shaders::stats_top}, entry{&peak_, shaders::peak}, entry{&volume_, shaders::volume},
          entry{&volume_clear_, shaders::volume_clear}, entry{&volume_curve_, shaders::volume_curve}, entry{&volume_prepare_, shaders::volume_prepare}, entry{&histogram_program_, shaders::histogram},
          entry{&histogram_max_, shaders::histogram_max}, entry{&ljpeg_guess_, shaders::ljpeg_guess}, entry{&ljpeg_sync_, shaders::ljpeg_sync},
          entry{&ljpeg_resolve_, shaders::ljpeg_resolve}, entry{&ljpeg_decode_, shaders::ljpeg_decode},
          entry{&ljpeg_columns_, shaders::ljpeg_columns}, entry{&ljpeg_rows_, shaders::ljpeg_rows}, entry{&ljpeg_black_, shaders::ljpeg_black},
          entry{&raw_blocks_, shaders::raw_blocks}, entry{&unpack_, shaders::unpack}, entry{&noise_, shaders::noise},
          entry{&clip_, shaders::clip}, entry{&clip_linear_, shaders::clip_linear}, entry{&clip_ycc_, shaders::clip_ycc},
          entry{&clip_find_, shaders::clip_find}, entry{&clip_limit_, shaders::clip_limit},
          entry{&develop_pile_, shaders::develop_pile}, entry{&develop_pile_linear_, shaders::develop_pile_linear},
          entry{&develop_pile_ycc_, shaders::develop_pile_ycc}, entry{&develop_drift_, shaders::develop_drift},
          entry{&develop_drift_linear_, shaders::develop_drift_linear}, entry{&develop_drift_ycc_, shaders::develop_drift_ycc},
          entry{&clip_drift_, shaders::clip_drift}, entry{&clip_combine_, shaders::clip_combine}})
    {
        auto r = device_->compute_program(make());
        if (!r.program)
        {
            error = r.error;
            return false;
        }
        *program = r.program;
    }
    histogram_ = device_->create_buffer(1032 * 4);
    blank_ = device_->create({1, 1, rap::gpu::format::r32f});
    if (!histogram_ || !blank_)
    {
        error = "no room for the measures";
        return false;
    }
    const float zero = 0;
    device_->upload(blank_, std::as_bytes(std::span(&zero, 1)), {1, rap::gpu::sample_type::f32});
    return true;
}

namespace
{

// IFRIT_GPU_RAW_CHECK: each readout the GPU decodes compared with the CPU's decode.
bool raw_check()
{
    static const bool on = std::getenv("IFRIT_GPU_RAW_CHECK") != nullptr;
    return on;
}

// The readout's (x, y) in a decode's samples, laid out as the Stream says:
// sample `c` of the photosite's `n` (a linear DNG's three colours).
uint16_t sample_at(const RawMosaic::Stream& st, const std::vector<uint16_t>& samples, unsigned x, unsigned y, unsigned n = 1, unsigned c = 0)
{
    const size_t i = st.index(x, y) * n + c;
    return i < samples.size() ? samples[i] : 0;
}

// A raw's readout decoded on the CPU, up as a photosite a texel (its
// colours, a linear DNG's): the active area out of it, and the masked areas
// summed for the black level just as ljpeg_black does on the GPU.
bool upload_decoded(rap::gpu::device& device, const RawMosaic& m, const std::vector<uint16_t>& samples, rap::gpu::texture_ptr& sites,
                    rap::gpu::buffer_ptr& levels, std::string& error)
{
    const RawMosaic::Stream& st = *m.stream;
    const auto sample = [&](unsigned x, unsigned y) { return sample_at(st, samples, x, y); };
    const unsigned n = unsigned(m.channels);
    std::vector<uint16_t> area(size_t(m.width) * m.height * n);
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            for (unsigned c = 0; c < n; ++c)
                area[(size_t(y) * m.width + x) * n + c] = sample_at(st, samples, unsigned(x + st.left), unsigned(y + st.top), n, c);
    // Sum R G B G2, count R G B G2, zeros; then the squared differences two
    // rows apart, R G B G2 each in 64 bits (low word first), and how many
    // (develop.hlsli's levels).
    uint32_t sums[kLevelsWords] = {};
    uint64_t squares[4] = {};
    for (const auto& r : st.mask)
    {
        const int bottom = std::min(r[2], st.readout_h);
        for (int y = std::max(r[0], 0); y < bottom; ++y)
            for (int x = std::max(r[1], 0); x < std::min(r[3], st.readout_w); ++x)
            {
                const unsigned c = (st.filters >> ((((y << 1) & 14) | (x & 1)) << 1)) & 3;
                const uint16_t v = sample(unsigned(x), unsigned(y));
                sums[c] += v;
                sums[4 + c] += 1;
                sums[8] += v == 0;
                if (y + 2 < bottom)
                {
                    const int d = std::clamp(int(v) - int(sample(unsigned(x), unsigned(y + 2))), -2047, 2047);
                    squares[c] += uint64_t(d * d);
                    sums[kLevelsDifferences + c] += 1;
                }
            }
    }
    for (int c = 0; c < 4; ++c)
    {
        sums[kLevelsSquares + 2 * c] = uint32_t(squares[c]);
        sums[kLevelsSquares + 2 * c + 1] = uint32_t(squares[c] >> 32);
    }
    sites = device.create({m.width, m.height, n == 1 ? rap::gpu::format::r16 : rap::gpu::format::rgba16});
    levels = device.create_buffer(sizeof sums);
    if (!sites || !levels)
        return error = "too big for the GPU", false;
    device.upload(sites, std::as_bytes(std::span(area)), {int(n), rap::gpu::sample_type::u16});
    device.upload(levels, std::as_bytes(std::span(sums)));
    return true;
}

// ljpeg.hlsli's block as ljpeg_black and mosaic read it, for any readout:
// its size, Canon's slices or a DNG's tiles, where the active area is, its
// filter pattern.
LjpegParams readout_params(const RawMosaic& m)
{
    const RawMosaic::Stream& st = *m.stream;
    LjpegParams p{};
    p.readout_w = uint32_t(st.readout_w), p.readout_h = uint32_t(st.readout_h);
    p.slice_n = st.slices[0], p.slice_w = st.slices[1], p.slice_last = st.slices[2];
    p.tile_w = st.tile[0], p.tile_h = st.tile[1], p.tiles_across = st.tile[2];
    p.planes = st.planes ? 1 : 0;
    p.quads = st.quads ? 1 : 0;
    p.left = uint32_t(st.left), p.top = uint32_t(st.top);
    p.filters = st.filters;
    p.mosaic_w = uint32_t(m.width), p.mosaic_h = uint32_t(m.height);
    return p;
}

}  // namespace

// Decoded on the CPU's cores instead, on a GPU in software (unless
// IFRIT_GPU_DECODE_ON_SOFTWARE, to test the passes there).
bool GpuView::decode_on_cpu() const
{
    static const bool keep = std::getenv("IFRIT_GPU_DECODE_ON_SOFTWARE") != nullptr;
    return device_->software() && !keep;
}

// `n` bytes of the file in a buffer of whole words, zeros after them and
// `slack` bytes more of zeros, for a shader reading a little past the end.
rap::gpu::buffer_ptr GpuView::upload_bytes(const uint8_t* data, size_t n, size_t slack)
{
    const size_t whole = n & ~size_t(3);
    std::vector<uint8_t> tail(((n + 3) & ~size_t(3)) - whole + slack);
    std::copy(data + whole, data + n, tail.begin());
    auto b = device_->create_buffer(whole + tail.size());
    if (b)
    {
        device_->upload(b, std::as_bytes(std::span(data, whole)));
        device_->upload(b, std::as_bytes(std::span(tail)), whole);
    }
    return b;
}

// Where a readout's decode goes: its samples (`samples`, 16 bits each, two
// to a word; what a decode leaves unwritten reads 0), the masked areas'
// sums (ljpeg_black's), and the active area as a photosite a texel (R, G, B
// for a linear DNG's three colours).
bool GpuView::readout_buffers(const RawMosaic& m, uint64_t samples, rap::gpu::buffer_ptr& values, rap::gpu::buffer_ptr& levels, rap::gpu::texture_ptr& sites,
                              std::string& error)
{
    values = device_->create_buffer((samples * 2 + 3) & ~uint64_t(3));
    levels = device_->create_buffer(kLevelsWords * 4);
    sites = device_->create({m.width, m.height, m.channels == 1 ? rap::gpu::format::r16 : rap::gpu::format::rgba16, 1, true});
    if (!values || !levels || !sites)
        return error = "too big for the GPU", false;
    device_->clear(values, 0);
    device_->clear(levels, 0);
    return true;
}

// After `work` (the decode into `values`, as `p`'s readout lays out): the
// masked areas, inside the readout as crop_masked_pixels clips them, summed
// for the black level, and the active area out into `sites`.
void GpuView::finish_readout(const RawMosaic& m, const LjpegParams& p, std::vector<rap::gpu::dispatch> work, const rap::gpu::buffer_ptr& values,
                             const rap::gpu::buffer_ptr& levels, const rap::gpu::texture_ptr& sites)
{
    const RawMosaic::Stream& st = *m.stream;
    std::vector<LjpegParams> masks;
    masks.reserve(std::size(st.mask));  // the dispatches point into it
    for (const auto& r : st.mask)
    {
        const int top = std::max(r[0], 0), left = std::max(r[1], 0);
        const int bottom = std::min(r[2], st.readout_h), right = std::min(r[3], st.readout_w);
        if (bottom <= top || right <= left)
            continue;
        LjpegParams& k = masks.emplace_back(p);
        k.rect[0] = top, k.rect[1] = left, k.rect[2] = bottom, k.rect[3] = right;
        rap::gpu::dispatch d;
        d.program = ljpeg_black_;
        d.buffers = {values, levels};
        d.uniforms = bytes(k);
        d.groups[0] = (right - left + 15) / 16;
        d.groups[1] = (bottom - top + 15) / 16;
        work.push_back(std::move(d));
    }
    device_->run(std::span(work));
    rap::gpu::pass pass;
    pass.program = m.channels == 1 ? mosaic_ : mosaic_linear_;
    pass.buffers = {values};
    pass.uniforms = bytes(p);
    pass.target = sites;
    device_->run(std::span(&pass, 1));
}

// IFRIT_GPU_RAW_CHECK: the GPU's samples read back and compared with the
// CPU's decode of the same readout (`cpu` makes it), the black level said,
// and the active area as the mosaic pass laid it out against the CPU's
// samples put in their places (Canon's slices, a DNG's tiles).
void GpuView::check_readout(const RawMosaic& m, const rap::gpu::buffer_ptr& values, const rap::gpu::buffer_ptr& levels, const rap::gpu::texture_ptr& sites,
                            const std::function<bool(std::vector<uint16_t>&, std::string&)>& cpu)
{
    const gint64 t0 = g_get_monotonic_time();
    std::vector<std::byte> gpu(values->size()), sums(levels->size());
    device_->readback(values, gpu).wait();
    device_->readback(levels, sums).wait();
    if (device_->lost())
    {
        std::fprintf(stderr, "gpu raw check: the GPU was lost reading the samples back\n");
        return;
    }
    const gint64 t1 = g_get_monotonic_time();
    std::vector<uint16_t> samples;
    std::string why;
    if (!cpu(samples, why))
    {
        std::fprintf(stderr, "gpu raw check: the CPU couldn't decode it: %s\n", why.c_str());
        return;
    }
    size_t wrong = 0, first = 0;
    for (size_t i = 0; i < samples.size(); ++i)
    {
        uint16_t v;
        std::memcpy(&v, gpu.data() + i * 2, 2);
        if (v != samples[i] && !wrong++)
            first = i;
    }
    uint32_t w[9];
    std::memcpy(w, sums.data(), sizeof w);
    std::fprintf(stderr, "gpu raw check: %zu of %zu samples differ%s (read back in %.0f ms); black R %u G %u B %u (%u zero)\n", wrong, samples.size(),
                 wrong ? (", first at " + std::to_string(first)).c_str() : "", (t1 - t0) / 1000.0, w[4] ? w[0] / w[4] : 0, w[5] ? w[1] / w[5] : 0,
                 w[6] ? w[2] / w[6] : 0, w[8]);
    const RawMosaic::Stream& st = *m.stream;
    const unsigned n = unsigned(m.channels), texel = n == 1 ? 1 : 4;  // as readout_buffers made it
    std::vector<uint16_t> area(size_t(m.width) * m.height * texel);
    device_->readback(sites, std::as_writable_bytes(std::span(area)), size_t(m.width) * 2 * texel).wait();
    if (device_->lost())
    {
        std::fprintf(stderr, "gpu raw check: the GPU was lost reading the active area back\n");
        return;
    }
    size_t off = 0;
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            for (unsigned c = 0; c < n; ++c)
                off += area[(size_t(y) * m.width + x) * texel + c] != sample_at(st, samples, unsigned(x + st.left), unsigned(y + st.top), n, c);
    std::fprintf(stderr, "gpu raw check: %zu of %zu samples of the active area differ\n", off, size_t(m.width) * m.height * n);
}

// LibRaw rounds down after each of its corrections, the develop not at all:
// how far apart they are, over the whole and away from the edges (where
// LibRaw reads the margins the develop doesn't have), and the farthest; up
// to saturation, which the develop clips at.
void GpuView::check_corrections(const RawMosaic& m, const rap::gpu::texture_ptr& corrected)
{
    const gint64 t0 = g_get_monotonic_time();
    std::vector<uint16_t> read(size_t(m.width) * m.height);
    device_->readback(corrected, std::as_writable_bytes(std::span(read)), size_t(m.width) * 2).wait();
    if (device_->lost())
    {
        std::fprintf(stderr, "gpu raw check: the GPU was lost reading the corrections back\n");
        return;
    }
    const gint64 t1 = g_get_monotonic_time();
    std::vector<float> ours(read.size());  // black off, as LibRaw's
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            ours[size_t(y) * m.width + x] = float(read[size_t(y) * m.width + x]) - m.black[m.site(x, y)];
    const std::vector<uint16_t>& theirs = *m.corrected;
    if (theirs.size() != ours.size())
        return;
    constexpr int kEdge = 4;
    double sum = 0;
    float worst[2] = {}, limits[3] = {1, 2, 8};
    size_t at[2] = {}, over[2][3] = {};
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
        {
            const size_t i = size_t(y) * m.width + x;
            const float white = m.white - m.black[m.site(x, y)];
            const float d = std::min(ours[i], white) - std::min(float(theirs[i]), white), a = std::fabs(d);
            const int inner = x >= kEdge && y >= kEdge && x < m.width - kEdge && y < m.height - kEdge;
            sum += d;
            for (int k = 0; k <= inner; ++k)
            {
                if (a > worst[k])
                    worst[k] = a, at[k] = i;
                for (int l = 0; l < 3; ++l)
                    over[k][l] += a > limits[l];
            }
        }
    for (int k = 0; k < 2; ++k)
        std::fprintf(stderr, "gpu corrections check: %s: greatest difference %.2f at (%zu, %zu), ours %.2f, LibRaw's %u; over 1: %zu, over 2: %zu, over 8: %zu\n",
                     k ? "away from the edges" : "every photosite", worst[k], at[k] % m.width, at[k] / m.width, ours[at[k]], theirs[at[k]], over[k][0],
                     over[k][1], over[k][2]);
    std::fprintf(stderr, "gpu corrections check: %zu photosites, ours less LibRaw's %+.3f on average (made and read back in %.0f ms)\n", ours.size(),
                 sum / double(ours.size()), (t1 - t0) / 1000.0);
}

bool GpuView::decode_stream(const RawMosaic& m, rap::gpu::texture_ptr& sites, rap::gpu::buffer_ptr& levels, std::string& error)
{
    const RawMosaic::Stream& st = *m.stream;
    if (st.packed)
        return unpack_stream(m, sites, levels, error);
    if (st.blocks)
        return decode_blocks(m, sites, levels, error);
    if (st.decoded)
        return assemble_decoded(m, sites, levels, error);
    const ljpeg::Prepared& j = *st.jpeg;
    if (decode_on_cpu())
    {
        ljpeg::Image cpu;
        return ljpeg::decode(j, cpu, error) && upload_decoded(*device_, m, cpu.samples, sites, levels, error);
    }
    const gint64 t_start = g_get_monotonic_time();
    const uint64_t frame = uint64_t(j.width) * j.components * j.height, samples = frame * j.parts.size();
    const uint32_t row = uint32_t(j.width * j.components);
    // ~1400 samples: the true decode mostly meets a guess within a few dozen
    // (where it doesn't, the sync goes on into the stream's next chunks), and
    // the resolve walks a stream's chunks one by one.
    constexpr uint32_t kChunkBits = 16384;
    // The streams (ljpeg.hlsli's PART_WORDS each), then their tables.
    constexpr size_t kTableWords = sizeof(ljpeg::Table) / 4, kPartWords = 12;
    const size_t part_words = j.parts.size() * kPartWords;
    std::vector<uint32_t> meta(part_words + j.tables.size() * kTableWords);
    uint64_t chunks = 0, held = 0;  // held: the samples up to the last stream's last
    for (size_t k = 0; k < j.parts.size(); ++k)
    {
        const ljpeg::Prepared::Part& part = j.parts[k];
        const uint64_t n = std::max<uint64_t>(1, (part.bits + kChunkBits - 1) / kChunkBits);
        uint32_t* w = meta.data() + k * kPartWords;
        w[0] = uint32_t(part.start), w[1] = uint32_t(part.start + part.bits), w[2] = uint32_t(chunks), w[3] = uint32_t(n);
        for (int c = 0; c < 4; ++c)
            w[4 + c] = uint32_t(part_words + part.table[c < j.components ? c : 0] * kTableWords);
        w[8] = uint32_t(row) * uint32_t(part.rows);
        held = k * frame + w[8];
        chunks += n;
    }
    std::memcpy(meta.data() + part_words, j.tables.data(), j.tables.size() * sizeof(ljpeg::Table));
    if (j.predictor != 1 || row % 2 || j.components > 4 || samples * 2 > gpu::kMaxBuffer || j.bits.size() * 8 >= (uint64_t(1) << 32) ||
        meta.size() >= (uint64_t(1) << 30) || j.height > 65535 || j.parts.size() > 65535 || chunks * j.components > 64 * 65535)
    {
        error = "a raw too big, or laid out in a way, the GPU doesn't decode";
        return false;
    }
    LjpegParams p = readout_params(m);
    p.parts = uint32_t(j.parts.size());
    p.chunk_bits = kChunkBits;
    p.chunks = uint32_t(chunks);
    p.components = uint32_t(j.components);
    p.samples = uint32_t(frame);
    p.row = row;
    p.rows = uint32_t(j.height);
    p.precision = uint32_t(j.precision);
    p.scheme = (j.nikon ? 1u : 0u) | uint32_t(j.step) << 8;
    p.curved = j.curve.empty() ? 0 : 1;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 4; c += 2)
            p.initial[(c >> 1) * 2 + r] = j.initial[r][c] | uint32_t(j.initial[r][c + 1]) << 16;

    // The streams, their tables, the curve (16 bits an entry), and the
    // passes' working buffers.
    auto stream = upload_bytes(j.bits.data(), j.bits.size(), 4);
    auto tables = upload_bytes(reinterpret_cast<const uint8_t*>(meta.data()), meta.size() * 4, 0);
    const uint16_t none = 0;
    auto curve = j.curve.empty() ? upload_bytes(reinterpret_cast<const uint8_t*>(&none), 2, 0)
                                 : upload_bytes(reinterpret_cast<const uint8_t*>(j.curve.data()), j.curve.size() * 2, 0);
    auto guesses = device_->create_buffer(chunks * j.components * 12);
    auto ways = device_->create_buffer(chunks * j.components * 16);
    auto plan = device_->create_buffer(16 + chunks * 16);
    rap::gpu::buffer_ptr values;
    if (!stream || !tables || !guesses || !ways || !plan || !curve)
        return error = "too big for the GPU", false;
    if (!readout_buffers(m, samples, values, levels, sites, error))
        return false;
    device_->clear(plan, 0);

    const auto dispatch = [&](rap::gpu::program_ptr program, std::vector<rap::gpu::buffer_ptr> buffers, uint64_t gx, uint64_t gy)
    {
        rap::gpu::dispatch d;
        d.program = std::move(program);
        d.buffers = std::move(buffers);
        d.uniforms = bytes(p);
        d.groups[0] = int(gx);
        d.groups[1] = int(gy);
        return d;
    };
    const uint64_t ways_in = chunks * j.components;
    finish_readout(m, p,
                   {
                       dispatch(ljpeg_guess_, {stream, tables, guesses}, (ways_in + 63) / 64, 1),
                       dispatch(ljpeg_sync_, {stream, tables, guesses, ways}, (ways_in + 63) / 64, 1),
                       dispatch(ljpeg_resolve_, {guesses, ways, plan, tables}, (j.parts.size() + 63) / 64, 1),
                       dispatch(ljpeg_decode_, {stream, tables, plan, values}, (chunks + 63) / 64, 1),
                       dispatch(ljpeg_columns_, {values}, j.parts.size(), 1),
                       dispatch(ljpeg_rows_, {values, curve}, uint64_t(j.height), j.parts.size()),
                   },
                   values, levels, sites);

    // A chunk whose true decode met none of its guesses (not expected, at
    // this chunk size) leaves the result wrong: said, and checked against
    // the CPU's decode on request.
    if (raw_check() || std::getenv("IFRIT_TRACE"))
    {
        // (Read back once the passes are done: how long they took, from the uploads on.)
        auto head = std::make_shared<std::vector<std::byte>>(plan->size());
        const size_t streams = j.parts.size();
        device_->readback(plan, *head, [head, device = device_, t_start, streams, chunks]
                          {
                              if (device->lost())
                              {
                                  std::fprintf(stderr, "gpu: the GPU was lost decoding a raw's stream\n");
                                  return;
                              }
                              uint32_t failed;
                              std::memcpy(&failed, head->data(), 4);
                              if (failed)
                                  std::fprintf(stderr, "gpu: a raw's stream didn't resolve on the GPU: its image is wrong\n");
                              std::fprintf(stderr, "gpu: a raw's %zu stream%s (%llu chunks) uploaded and decoded in %.1f ms\n", streams,
                                           streams == 1 ? "" : "s", (unsigned long long)chunks, (g_get_monotonic_time() - t_start) / 1000.0);
                          });
    }
    if (raw_check())
        check_readout(m, values, levels, sites,
                      [&](std::vector<uint16_t>& out, std::string& why)
                      {
                          ljpeg::Image cpu;
                          if (!ljpeg::decode(j, cpu, why))
                              return false;
                          out = std::move(cpu.samples);
                          out.resize(held);  // past it (a short last restart interval) neither decodes the stream
                          return true;
                      });
    return true;
}

bool GpuView::unpack_stream(const RawMosaic& m, rap::gpu::texture_ptr& sites, rap::gpu::buffer_ptr& levels, std::string& error)
{
    const RawMosaic::Stream& st = *m.stream;
    const rawpacked::Prepared& pk = *st.packed;
    if (decode_on_cpu())
    {
        std::vector<uint16_t> readout;
        rawpacked::unpack(pk, readout);
        return upload_decoded(*device_, m, readout, sites, levels, error);
    }
    // The rows' bytes from the word they start in, and a little more: a
    // sample's bits are read three bytes at a time, and whole groups.
    const size_t start = pk.offset & ~size_t(3), lead = pk.offset - start;
    const uint64_t samples = uint64_t(pk.readout_w) * pk.readout_h, words = (samples + 1) / 2;
    if (lead + pk.bytes + 16 > gpu::kMaxBuffer || words * 4 > gpu::kMaxBuffer || words > uint64_t(65536) * 65535 ||
        uint64_t(lead) * 8 + uint64_t(pk.row_bits) * pk.readout_h >= (uint64_t(1) << 32))
        return error = "a raw too big for the GPU to unpack", false;
    auto stored = upload_bytes(pk.file + start, lead + pk.bytes, 16);
    auto curve = device_->create_buffer(pk.curve.empty() ? 4 : pk.curve.size() * 2);
    rap::gpu::buffer_ptr values;
    if (!stored || !curve)
        return error = "too big for the GPU", false;
    if (!readout_buffers(m, samples, values, levels, sites, error))
        return false;
    if (!pk.curve.empty())
        device_->upload(curve, std::as_bytes(std::span(pk.curve)));

    UnpackParams u{};
    u.readout_w = uint32_t(pk.readout_w), u.readout_h = uint32_t(pk.readout_h);
    u.row_bits = pk.row_bits, u.bits = uint32_t(pk.bits);
    u.group = uint32_t(pk.group), u.shift = uint32_t(pk.shift), u.swap = pk.swap ? 1 : 0, u.curved = pk.curve.empty() ? 0 : 1;
    u.lead = uint32_t(lead), u.words = uint32_t(words);
    std::vector<rap::gpu::dispatch> work(1);
    work[0].program = unpack_;
    work[0].buffers = {stored, curve, values};
    work[0].uniforms = bytes(u);
    work[0].groups[0] = int(std::min<uint64_t>(256, (words + 255) / 256));
    work[0].groups[1] = int((words + 65535) / 65536);
    finish_readout(m, readout_params(m), std::move(work), values, levels, sites);
    if (raw_check())
        check_readout(m, values, levels, sites,
                      [&](std::vector<uint16_t>& out, std::string&)
                      {
                          rawpacked::unpack(pk, out);
                          return true;
                      });
    return true;
}

bool GpuView::decode_blocks(const RawMosaic& m, rap::gpu::texture_ptr& sites, rap::gpu::buffer_ptr& levels, std::string& error)
{
    const RawMosaic::Stream& st = *m.stream;
    const rawblocks::Prepared& b = *st.blocks;
    // On the CPU's cores where the GPU is software (one too big for it the loader unpacked).
    if (decode_on_cpu() || !rawblocks::fits_gpu(b))
    {
        std::vector<uint16_t> readout;
        rawblocks::decode(b, readout);
        return upload_decoded(*device_, m, readout, sites, levels, error);
    }
    const uint64_t extent = b.extent();
    BlocksParams p{};
    p.format = uint32_t(b.format);
    p.bits = uint32_t(b.bits);
    p.per = uint32_t(b.per);
    p.units = b.units();
    p.blocks = b.blocks;
    p.readout_w = uint32_t(st.readout_w);
    p.row_blocks = uint32_t(b.row_blocks);
    p.rotated = b.rotated ? 1 : 0;
    p.rotation = b.rotation;
    p.size = uint32_t(extent);  // past it, none of the file is read
    // A thread a pair of blocks, 64 a group: in rows of 1024 groups when
    // there are more than a dispatch's 65535 across.
    const uint64_t groups = (uint64_t(p.units) + 63) / 64;
    const uint64_t gx = groups <= 65535 ? std::max<uint64_t>(groups, 1) : 1024, gy = (groups + gx - 1) / gx;
    p.span = uint32_t(gx * 64);

    // The file where the blocks lie, as LibRaw reads it, and Sony's curve.
    auto data = upload_bytes(b.data, size_t(extent), 32);
    std::vector<uint32_t> curve_words(b.curve.begin(), b.curve.end());
    if (curve_words.empty())
        curve_words.push_back(0);
    auto curve = device_->create_buffer(curve_words.size() * 4);
    rap::gpu::buffer_ptr values;
    if (!data || !curve)
        return error = "too big for the GPU", false;
    if (!readout_buffers(m, uint64_t(st.readout_w) * st.readout_h, values, levels, sites, error))
        return false;
    device_->upload(curve, std::as_bytes(std::span(curve_words)));

    std::vector<rap::gpu::dispatch> work(1);
    work[0].program = raw_blocks_;
    work[0].buffers = {data, curve, values};
    work[0].uniforms = bytes(p);
    work[0].groups[0] = int(gx);
    work[0].groups[1] = int(gy);
    finish_readout(m, readout_params(m), std::move(work), values, levels, sites);
    if (raw_check())
        check_readout(m, values, levels, sites,
                      [&](std::vector<uint16_t>& out, std::string&)
                      {
                          rawblocks::decode(b, out);
                          return true;
                      });
    return true;
}

bool GpuView::assemble_decoded(const RawMosaic& m, rap::gpu::texture_ptr& sites, rap::gpu::buffer_ptr& levels, std::string& error)
{
    const std::vector<uint16_t>& decoded = *m.stream->decoded;
    if (decode_on_cpu())
        return upload_decoded(*device_, m, decoded, sites, levels, error);
    if (decoded.size() * 2 > gpu::kMaxBuffer)
        return error = "a raw too big for the GPU", false;
    rap::gpu::buffer_ptr values;
    if (!readout_buffers(m, decoded.size(), values, levels, sites, error))
        return false;
    device_->upload(values, std::as_bytes(std::span(decoded)));
    finish_readout(m, readout_params(m), {}, values, levels, sites);
    if (raw_check())
        check_readout(m, values, levels, sites,
                      [&](std::vector<uint16_t>& out, std::string&)
                      {
                          out = decoded;
                          return true;
                      });
    return true;
}

bool GpuView::upload(const Image& image, Source& s, std::string& error)
{
    s.levels.clear();
    s.tone.reset();
    const int w = image.width, h = image.height;
    rap::gpu::texture_ptr data;
    DecodeParams dp{1, 3, 0, 0};
    s.scene_gain = 1;
    s.tonable = image.tonable();
    // Normalized BT.709 integer sources cannot leave the display without
    // an edit. Floating/signed data and wide/unknown profiles can, even
    // when their measured luminance is below white.
    s.measure_colour = image.source && image.source->sample != SourceData::Sample::U16;
    if (image.declared_space && !image.declared_space->with_transfer(ColorSpace::Transfer::SRGB).is_srgb())
        s.measure_colour = true;
    if (image.stored && !image.stored->icc.empty())
        s.measure_colour = s.measure_colour || !icc_bt709_primaries(image.stored->icc.data(), image.stored->icc.size());
    s.auto_exposure = false;
    s.clips.reset();
    s.raw = false;
    for (int c = 0; c < 3; ++c)
    {
        s.light[c] = s.readout[c] = s.green[0][c] = s.green[1][c] = 1;
        s.correlation[0][c] = s.correlation[1][c] = 0;
        for (int k = 0; k < 3; ++k)
            s.measure[c][k] = 0;
    }
    s.anchor = 0;
    s.white_luminance = 0;
    s.dng.reset();
    std::memcpy(s.input_to_xyz, kXyzFrom709, sizeof s.input_to_xyz);
    if (const RawMosaic* m = image.mosaic.get())
    {
        // A raw, developed here: the mosaic up as it is, then demosaiced into
        // level 0 (camera RGB, unbalanced). The display's boundary, which its
        // view needs (boundary()), is made meanwhile on another thread, from
        // the first raw on.
        const gint64 t_start = g_get_monotonic_time();
        if (const float peak = display_peak(); !(boundary_ && boundary_peak_ == peak) && !(boundary_ahead_.valid() && boundary_ahead_peak_ == peak))
        {
            boundary_ahead_peak_ = peak;
            boundary_ahead_ = std::async(std::launch::async, boundary_table, peak);
        }
        rap::gpu::texture_ptr sites;
        rap::gpu::buffer_ptr levels;
        if (m->stream)
        {
            if (!decode_stream(*m, sites, levels, error))
                return false;
        }
        else if (m->data && m->channels == 3)
        {
            if ((sites = device_->create({m->width, m->height, rap::gpu::format::rgba16})))
                device_->upload(sites, std::as_bytes(std::span(*m->data)), {3, rap::gpu::sample_type::u16});
        }
        else if (m->data && (sites = device_->create({m->width, m->height, rap::gpu::format::r16})))
            device_->upload(sites, std::as_bytes(std::span(*m->data)), {1, rap::gpu::sample_type::u16});
        auto top = device_->create({w, h, rap::gpu::format::rgba16f, 1, true});
        if (!sites || !top)
        {
            error = "too big for the GPU";
            return false;
        }
        DevelopParams p{};
        // The pattern over 6 x 6, a whole number of it (as develop.hlsl reads it).
        for (int i = 0; i < 36; ++i)
        {
            const int s = m->channels == 3 ? i : m->site(i % 6, i / 6);
            p.cfa[i / 16] |= uint32_t(m->filter[s]) << (i % 16 * 2);
            p.black[i] = m->black[s];
        }
        p.cfa[3] = uint32_t(m->period);
        for (int c = 0; c < 3; ++c)
            p.as_shot[c] = m->as_shot[c];
        p.as_shot[3] = m->white;
        p.flags[0] = m->balanced, p.flags[1] = m->grey;
        p.size[0] = m->width, p.size[1] = m->height, p.size[2] = levels ? 1 : 0;
        rap::gpu::buffer_ptr corrections;
        if (m->corrections && m->channels == 1 && !m->ycc.block_h &&
            (corrections = device_->create_buffer(m->corrections->size() * 4)))
        {
            device_->upload(corrections, std::as_bytes(std::span(*m->corrections)));
            p.size[3] = 1;
        }
        // Its own corrections of the mosaic (OpcodeList2's gain maps, a Phase
        // One's readout corrections), made once a photosite into a mosaic
        // the develop reads as it would the raw's: whole data units, as
        // LibRaw makes a Phase One's; the gain maps' fractions kept (their
        // gains lift the shadows of a phone's 10 bits) as halves.
        bool halves = false;  // the mosaic now in half floats (gains' fractions kept)
        if (corrections && ((*m->corrections)[0] || (*m->corrections)[3]))
        {
            const bool whole = (*m->corrections)[3];
            halves = !whole;
            rap::gpu::pass c;
            c.program = develop_correct_;
            c.inputs = {sites};
            c.buffers = {levels ? levels : histogram_, corrections};
            c.uniforms = bytes(p);
            c.target = device_->create({m->width, m->height, whole ? rap::gpu::format::r16 : rap::gpu::format::r16f, 1, true});
            if (!c.target)
            {
                error = "too big for the GPU";
                return false;
            }
            device_->run(std::span(&c, 1));
            sites = c.target;
            if (m->corrected)
                check_corrections(*m, sites);
        }
        // Hot and cold photosites out, before the demosaic spreads them into
        // coloured specks (Bayer's and a monochrome sensor's; X-Trans's
        // neighbours of a colour aren't on a lattice). In the format it came
        // in: unorm 16 keeps 16-bit data exact, which halves wouldn't.
        static const bool impulses = !std::getenv("IFRIT_NO_IMPULSE");
        if (impulses && m->channels == 1 && !m->ycc.block_h && (m->period == 2 || m->grey))
        {
            rap::gpu::pass c;
            c.program = develop_impulse_;
            c.inputs = {sites};
            c.buffers = {levels ? levels : histogram_, corrections ? corrections : histogram_};
            c.uniforms = bytes(p);
            c.target = device_->create({m->width, m->height, halves ? rap::gpu::format::r16f : rap::gpu::format::r16, 1, true});
            if (!c.target)
            {
                error = "too big for the GPU";
                return false;
            }
            device_->run(std::span(&c, 1));
            sites = c.target;
        }
        // Output (x, y), as shown, to the photosite: through the turn, then
        // into the framing (EXIF orientation: what the stored image needs).
        const int cw = m->crop[2] - 1, ch = m->crop[3] - 1, cx = m->crop[0], cy = m->crop[1];
        int a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;  // crop x = a*x + b*y + c, crop y = d*x + e*y + f
        switch (m->orientation)
        {
            case 2: a = -1, c = cw; break;
            case 3: a = -1, c = cw, e = -1, f = ch; break;
            case 4: e = -1, f = ch; break;
            case 5: a = 0, b = 1, d = 1, e = 0; break;
            case 6: a = 0, b = 1, d = -1, e = 0, f = ch; break;
            case 7: a = 0, b = -1, c = cw, d = -1, e = 0, f = ch; break;
            case 8: a = 0, b = -1, c = cw, d = 1, e = 0; break;
            default: break;
        }
        p.map_x[0] = a, p.map_x[1] = b, p.map_x[2] = c + cx;
        p.map_y[0] = d, p.map_y[1] = e, p.map_y[2] = f + cy;
        rap::gpu::pass pass;
        if (m->ycc.block_h)
        {
            p.ycc[0] = m->ycc.block_h, p.ycc[1] = m->ycc.formula, p.ycc[2] = m->ycc.hue, p.ycc[3] = m->ycc.luma_offset;
            p.ycc_at[0] = m->ycc.x0, p.ycc_at[1] = m->ycc.y0;
            static const bool sraw_mul = !std::getenv("IFRIT_SRAW_NO_MUL");
            for (int c = 0; c < 3; ++c)
                p.ycc_mul[c] = sraw_mul ? m->ycc.mul[c] : 1.f;
            const float e = 0.01f * m->white;  // colour follows luma changes larger than ~1% of the range
            p.ycc_mul[3] = e * e;
        }
        // X-Trans's green first, everywhere, a photosite a texel (develop.hlsl's GREEN).
        rap::gpu::texture_ptr green;
        if (m->period == 6 && !m->grey)
        {
            rap::gpu::pass g;
            g.program = develop_green_;
            g.inputs = {sites};
            g.buffers = {levels ? levels : histogram_, corrections ? corrections : histogram_};
            g.uniforms = bytes(p);
            g.target = green = device_->create({m->width, m->height, rap::gpu::format::r16f, 1, true});
            if (!green)
            {
                error = "too big for the GPU";
                return false;
            }
            device_->run(std::span(&g, 1));
        }
        // Where each colour clips, from the data as the develop reads it
        // (clip.hlsl), for the develop to clip at.
        auto clips = device_->create_buffer(kClipWords * 4);
        if (!clips)
        {
            error = "too big for the GPU";
            return false;
        }
        device_->clear(clips, 0);
        // What's remembered of the camera's linearity (raw_linearity.h), for
        // where the image doesn't show its own (clip.hlsl's COMBINE).
        const std::string format = linearity::format_of(*m);
        {
            float stored[3];
            linearity::recall(m->camera, format, stored);
            device_->upload(clips, std::as_bytes(std::span(stored)), kClipStored * 4);
        }
        {
            rap::gpu::dispatch d[2];
            d[0].program = m->ycc.block_h ? clip_ycc_ : m->channels == 3 ? clip_linear_ : clip_;
            d[0].inputs = {sites};
            d[0].buffers = {clips, levels ? levels : histogram_};
            d[0].uniforms = bytes(p);
            d[0].groups[0] = (m->width + 63) / 64;
            d[0].groups[1] = (m->height + 63) / 64;
            d[1].program = clip_find_;
            d[1].buffers = {clips, levels ? levels : histogram_};
            d[1].uniforms = bytes(p);
            device_->run(std::span(d));
        }
        pass.program = m->ycc.block_h ? develop_ycc_ : m->channels == 3 ? develop_linear_ : develop_;
        pass.inputs = {sites, green ? green : sites};  // the second read only for X-Trans
        pass.buffers = {levels ? levels : histogram_, corrections ? corrections : histogram_, clips};  // the first two read only when there
        // A DNG whose lens is corrected (WarpRectilinear): the whole active
        // area developed as it lies, then warped into the framing as shown.
        const bool warp = corrections && (*m->corrections)[16] != 0;
        rap::gpu::texture_ptr whole;
        if (warp && !(whole = device_->create({m->width, m->height, rap::gpu::format::rgba16f, 1, true})))
        {
            error = "too big for the GPU";
            return false;
        }
        WarpParams wp{};
        if (warp)
        {
            std::memcpy(wp.map_x, p.map_x, sizeof wp.map_x);
            std::memcpy(wp.map_y, p.map_y, sizeof wp.map_y);
            wp.size[0] = m->width, wp.size[1] = m->height;
            p.map_x[0] = 1, p.map_x[1] = 0, p.map_x[2] = 0;
            p.map_y[0] = 0, p.map_y[1] = 1, p.map_y[2] = 0;
        }
        // Where each colour's values pile up over the developed image, its
        // linearity limit (develop.hlsl's PILE, clip.hlsl's LIMIT); then the
        // colour field the develop estimates what's past those from
        // (develop.hlsl's GATHER, a texel a 4 x 4 cell), measured twice (the
        // second with the first's estimates, and the magnitude) and spread
        // smoothly through a pyramid (highlight.hlsl), relaxed at each level:
        // the ratios as a membrane, the magnitude as a thin plate (RELAX).
        const rap::gpu::texture_ptr& out = warp ? whole : top;
        p.map_x[3] = out->width(), p.map_y[3] = out->height();
        {
            rap::gpu::dispatch d[2];
            d[0].program = m->ycc.block_h ? develop_pile_ycc_ : m->channels == 3 ? develop_pile_linear_ : develop_pile_;
            d[0].inputs = {sites, green ? green : sites};
            d[0].buffers = {clips, levels ? levels : histogram_, corrections ? corrections : histogram_};
            d[0].uniforms = bytes(p);
            d[0].groups[0] = (out->width() + 63) / 64;
            d[0].groups[1] = (out->height() + 63) / 64;
            d[1].program = clip_limit_;
            d[1].buffers = {clips};
            d[1].uniforms = bytes(p);
            device_->run(std::span(d));
        }
        // Where each colour's ratio to the others starts to drift as the
        // light on a surface rises (drift.hlsli: develop.hlsl's DRIFT a
        // tile, clip.hlsl's DRIFT over runs of three), and the limits from
        // that and the piles (COMBINE).
        {
            const int across = (out->width() + kDriftTile - 1) / kDriftTile, down = (out->height() + kDriftTile - 1) / kDriftTile;
            auto tiles = device_->create_buffer(size_t(across) * size_t(down) * 32);  // develop.hlsl's DRIFT: two float4s a tile
            if (!tiles)
            {
                error = "too big for the GPU";
                return false;
            }
            rap::gpu::dispatch d[3];
            d[0].program = m->ycc.block_h ? develop_drift_ycc_ : m->channels == 3 ? develop_drift_linear_ : develop_drift_;
            d[0].inputs = {sites, green ? green : sites};
            d[0].buffers = {clips, levels ? levels : histogram_, corrections ? corrections : histogram_, tiles};
            d[0].uniforms = bytes(p);
            d[0].groups[0] = (across + 7) / 8;
            d[0].groups[1] = (down + 7) / 8;
            d[1].program = clip_drift_;
            d[1].buffers = {clips, histogram_, tiles};
            d[1].uniforms = bytes(p);
            d[1].groups[0] = (across + 15) / 16;
            d[1].groups[1] = (down + 15) / 16;
            d[2].program = clip_combine_;
            d[2].buffers = {clips};
            d[2].uniforms = bytes(p);
            device_->run(std::span(d));
        }
        constexpr int kCell = 4;  // develop.hlsl's kRatioCell
        const int fw = (out->width() + kCell - 1) / kCell, fh = (out->height() + kCell - 1) / kCell;
        // The field's textures, the view's (made once for a size, and kept:
        // the next raw of that size, and the four spreads here, reuse them;
        // made again only for another size), by slot: each spread's
        // result (0..3, stage 1's two then stage 2's, read after it), then
        // what every spread uses in turn (run one after another, as passes
        // are). Level 0, the largest, half floats: its log ratios and
        // magnitude want no more, and its weights and confidences (a
        // level-0 cell's measures, or a fill's confidence brought down from
        // above, a quarter a level) stay small; the levels above sum their
        // cells' weights, four a level, so they're full floats.
        const auto pooled = [&](size_t slot, int w, int h) -> rap::gpu::texture_ptr
        {
            const rap::gpu::format f = w == fw && h == fh ? rap::gpu::format::rgba16f : rap::gpu::format::rgba32f;
            if (field_pool_.size() <= slot)
                field_pool_.resize(slot + 1);
            auto& t = field_pool_[slot];
            if (!t || t->width() != w || t->height() != h || t->format() != f)
                t = device_->create({w, h, f, 1, true});
            return t;
        };
        // GATHER's measures (stage 1 or 2, field 0 or 1), spread.
        constexpr int kRelax = 8;  // RELAX's sweeps a level (even: they end where they began)
        const auto spread = [&](int stage, int which, const rap::gpu::texture_ptr* before) -> rap::gpu::texture_ptr
        {
            // Slots: the result 0..3; level 0's gathered 4 and relaxed 5;
            // above it, level k's (k >= 1) brought down, filled and relaxed.
            const size_t result = size_t(stage - 1) * 2 + size_t(which);
            const auto level_slot = [](size_t k, size_t part) { return 6 + (k - 1) * 3 + part; };
            std::vector<rap::gpu::texture_ptr> pushed;
            auto gathered = pooled(4, fw, fh);
            if (!gathered)
                return nullptr;
            DevelopParams q = p;
            q.flags[2] = stage, q.flags[3] = which;
            std::vector<rap::gpu::pass> passes(1);
            passes[0].program = m->ycc.block_h ? develop_gather_ycc_ : m->channels == 3 ? develop_gather_linear_ : develop_gather_;
            passes[0].inputs = {sites, green ? green : sites, before ? before[0] : blank_, before ? before[1] : blank_};
            passes[0].buffers = {levels ? levels : histogram_, corrections ? corrections : histogram_, clips};
            passes[0].uniforms = bytes(q);
            passes[0].target = gathered;
            pushed.push_back(gathered);
            for (int w = fw, h = fh; w > 1 || h > 1;)
            {
                w = (w + 1) / 2, h = (h + 1) / 2;
                auto next = pooled(level_slot(pushed.size(), 0), w, h);
                if (!next)
                    return nullptr;
                rap::gpu::pass d;
                d.program = highlight_down_;
                d.buffers = {clips};
                d.inputs = {pushed.back()};
                d.target = next;
                passes.push_back(d);
                pushed.push_back(next);
            }
            rap::gpu::texture_ptr filled = pushed.back();
            for (size_t k = pushed.size() - 1; k-- > 0;)
            {
                auto finer = k == 0 ? pooled(result, fw, fh) : pooled(level_slot(k, 1), pushed[k]->width(), pushed[k]->height());
                if (!finer)
                    return nullptr;
                rap::gpu::pass u;
                u.program = highlight_up_;
                u.buffers = {clips};
                u.inputs = {pushed[k], filled};
                u.target = finer;
                passes.push_back(u);
                filled = finer;
                if (stage == 2)
                {
                    auto other = k == 0 ? pooled(5, fw, fh) : pooled(level_slot(k, 2), finer->width(), finer->height());
                    if (!other)
                        return nullptr;
                    for (int i = 0; i < kRelax; ++i)
                    {
                        rap::gpu::pass r;
                        r.program = which == 1 ? highlight_relax_ : highlight_relax_ratios_;
                        r.buffers = {clips};
                        r.inputs = {filled};
                        r.target = i % 2 ? finer : other;
                        passes.push_back(r);
                        filled = r.target;
                    }
                }
            }
            device_->run(std::span<const rap::gpu::pass>(passes));
            return filled;
        };
        rap::gpu::texture_ptr first[2] = {spread(1, 0, nullptr), spread(1, 1, nullptr)};
        rap::gpu::texture_ptr second[2];
        if (first[0] && first[1])
            second[0] = spread(2, 0, first), second[1] = spread(2, 1, first);
        if (!second[0] || !second[1])
        {
            error = "too big for the GPU";
            return false;
        }
        pass.inputs.push_back(second[0]);
        pass.inputs.push_back(second[1]);
        pass.uniforms = bytes(p);
        pass.target = out;
        device_->run(std::span(&pass, 1));
        if (warp)
        {
            rap::gpu::pass w;
            w.program = warp_;
            w.inputs = {whole};
            w.buffers = {corrections};
            w.uniforms = bytes(wp);
            w.target = top;
            device_->run(std::span(&w, 1));
        }
        if (std::getenv("IFRIT_TRACE"))
        {
            // (Read back once the develop is done: how long it took, from the upload on.)
            auto probe = std::make_shared<std::vector<std::byte>>(histogram_->size());
            device_->readback(histogram_, *probe, [probe, t_start]
                              { std::fprintf(stderr, "gpu: a raw uploaded and developed in %.1f ms\n", (g_get_monotonic_time() - t_start) / 1000.0); });
            auto found = std::make_shared<std::vector<std::byte>>(clips->size());
            device_->readback(clips, *found,
                              [found]
                              {
                                  float f[3];
                                  std::memcpy(f, found->data() + kClipFound * 4, sizeof f);
                                  std::fprintf(stderr, "gpu: colours clip at R %.4f G %.4f B %.4f of the stated white (0: none found)\n", f[0], f[1], f[2]);
                                  std::memcpy(f, found->data() + kClipLimit * 4, sizeof f);
                                  std::fprintf(stderr, "gpu: colours linear to R %.4f G %.4f B %.4f of their clips\n", f[0], f[1], f[2]);
                                  float piled[3], drifted[3], sure[3], stored[3], censored;
                                  std::memcpy(piled, found->data() + kClipPiled * 4, sizeof piled);
                                  std::memcpy(drifted, found->data() + kClipDrifted * 4, sizeof drifted);
                                  std::memcpy(sure, found->data() + kClipDriftSure * 4, sizeof sure);
                                  std::memcpy(stored, found->data() + kClipStored * 4, sizeof stored);
                                  std::memcpy(&censored, found->data() + kClipCensored * 4, sizeof censored);
                                  uint32_t any = 0;
                                  std::memcpy(&any, found->data() + kClipAny * 4, sizeof any);
                                  std::fprintf(stderr,
                                               "gpu: piles at R %.4f G %.4f B %.4f; drift R %.4f%s G %.4f%s B %.4f%s; remembered R %.4f G %.4f B %.4f "
                                               "(0: none); %.0f sampled values censored under the clip; %s\n",
                                               piled[0], piled[1], piled[2], drifted[0], sure[0] > 0 ? " sure" : "", drifted[1],
                                               sure[1] > 0 ? " sure" : "", drifted[2], sure[2] > 0 ? " sure" : "", stored[0], stored[1], stored[2],
                                               censored, any ? "photosites censored: estimated" : "none censored: nothing estimated");
                                  for (int c = 0; c < 3; ++c)
                                  {
                                      std::fprintf(stderr, "gpu: drift %c:", "RGB"[c]);
                                      for (unsigned b = 0; b < kDriftBins; ++b)
                                      {
                                          float s[kDriftSums] = {};  // the quadrants' together
                                          for (unsigned g = 0; g < kDriftGroups; ++g)
                                          {
                                              float q[kDriftSums];
                                              std::memcpy(q, found->data() + (kClipDrift + ((g * 3 + c) * kDriftBins + b) * kDriftSums) * 4, sizeof q);
                                              for (unsigned k = 0; k < kDriftSums; ++k)
                                                  s[k] += q[k];
                                          }
                                          if (s[3] > 0)
                                              std::fprintf(stderr, " %.3f:%.3f/%.0f", kPileFrom + (b + 1) * (1 - kPileFrom) / kDriftBins,
                                                           s[1] > s[4] ? (s[0] + s[4]) / (s[1] - s[4]) : 0.f, s[3]);  // drift_limit()'s slope
                                      }
                                      std::fprintf(stderr, "\n");
                                  }
                              });
        }
        // The image's drift, remembered for its camera where it's sure.
        if (!m->camera.empty())
        {
            auto drift = std::make_shared<std::vector<std::byte>>(clips->size());
            device_->readback(clips, *drift,
                              [drift, camera = m->camera, format]
                              {
                                  float limit[3], sure[3];
                                  std::memcpy(limit, drift->data() + kClipDrifted * 4, sizeof limit);
                                  std::memcpy(sure, drift->data() + kClipDriftSure * 4, sizeof sure);
                                  const bool kept[3] = {sure[0] > 0, sure[1] > 0, sure[2] > 0};
                                  linearity::remember(camera, format, limit, kept);
                              });
            if (levels)
            {
                // The masked border's black and readout noise, data units.
                auto sums = std::make_shared<std::vector<std::byte>>(levels->size());
                device_->readback(levels, *sums,
                                  [sums]
                                  {
                                      uint32_t w[kLevelsWords];
                                      std::memcpy(w, sums->data(), sizeof w);
                                      double black[4] = {}, sigma[4] = {};
                                      for (int c = 0; c < 4; ++c)
                                      {
                                          const uint64_t sq = w[kLevelsSquares + 2 * c] | uint64_t(w[kLevelsSquares + 2 * c + 1]) << 32;
                                          black[c] = w[4 + c] ? double(w[c]) / w[4 + c] : 0;
                                          sigma[c] = w[kLevelsDifferences + c] ? std::sqrt(double(sq) / w[kLevelsDifferences + c] / 2) : 0;
                                      }
                                      std::fprintf(stderr, "gpu: masked border: black R %.1f G %.1f B %.1f G2 %.1f, readout noise R %.2f G %.2f B %.2f G2 %.2f\n",
                                                   black[0], black[1], black[2], black[3], sigma[0], sigma[1], sigma[2], sigma[3]);
                                  });
            }
        }
        s.levels.push_back(top);
        // Shown through its colour matrix, XYZ <- 709 . rgb_cam; each frame
        // puts its white balance in front (render()).
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                float v = 0;
                for (int k = 0; k < 3; ++k)
                    v += kXyzFrom709[i][k] * m->rgb_cam[k][j];
                s.input_to_xyz[i][j] = v;
            }
        std::memcpy(s.as_shot, m->as_shot, sizeof s.as_shot);
        s.dng = m->dng;
        s.auto_exposure = true;
        s.clips = clips;
        s.raw = true;
        develop_noise(*m, s.light, s.readout, s.green, s.correlation, s.measure);
        s.measured_law = m->ycc.block_h || m->channels == 3 || m->grey;  // the others' interpolation is ours, its factors exact
        // The camera's own exposure: its middle grey where its JPEG would
        // have it, a stop over the raw's (about where cameras put it), and a
        // DNG's BaselineExposure on top.
        s.anchor = float(2 * std::exp2(m->dng ? m->dng->exposure : 0.0));
        // And the scene's light, absolute: the luminance at its white, from
        // how it was taken (exposure.hlsli); 0 without its ISO, shutter or aperture.
        s.white_luminance = m->white_luminance;
        s.measured = false;
        return true;
    }
    if (const LinearData* raw = image.linear.get(); raw && raw->rgb.size() == size_t(w) * h * 3)
    {
        s.raw = true;  // a raw LibRaw developed
        // Linear RGB, 16 bits; its scale (and curve) applied as it's shown.
        data = device_->create({w, h, rap::gpu::format::rgba16});
        if (data)
            device_->upload(data, std::as_bytes(std::span(raw->rgb.data(), raw->rgb.size())), {3, rap::gpu::sample_type::u16});
        if (!raw->tone.empty())
        {
            std::vector<float> curve(4096, raw->tone.back());
            std::copy_n(raw->tone.begin(), std::min<size_t>(4096, raw->tone.size()), curve.begin());
            s.tone = device_->create({4096, 1, rap::gpu::format::r32f});
            if (s.tone)
                device_->upload(s.tone, std::as_bytes(std::span(curve)), {1, rap::gpu::sample_type::f32});
        }
        else
            s.scene_gain = float(raw->auto_gain);
    }
    else if (const SourceData* s = image.source.get(); s && s->width == w && s->height == h)
    {
        rap::gpu::format format = rap::gpu::format::rgba16;
        rap::gpu::sample_type type = rap::gpu::sample_type::u16;
        switch (s->sample)
        {
            case SourceData::Sample::U16:
                break;
            case SourceData::Sample::I16:
                format = rap::gpu::format::rgba16_snorm, type = rap::gpu::sample_type::i16;
                break;
            case SourceData::Sample::F16:
                format = rap::gpu::format::rgba16f, type = rap::gpu::sample_type::f16;
                break;
            case SourceData::Sample::F32:
                format = rap::gpu::format::rgba32f, type = rap::gpu::sample_type::f32;
                break;
        }
        data = device_->create({w, h, format});
        if (data)
            device_->upload(data, std::as_bytes(std::span(s->data.data(), s->data.size())), {s->channels, type});
        dp = {1, s->channels, s->linear ? 0 : 1, s->sample == SourceData::Sample::I16 ? 1 : 0};
    }
    else
    {
        // An 8-bit image: its pixels are its data.
        data = device_->create({w, h, rap::gpu::format::rgba8});
        if (data)
            device_->upload(data, std::as_bytes(std::span(image.pixels.data(), image.pixels.size())),
                            {4, rap::gpu::sample_type::u8, size_t(image.stride)});
        dp = {0, 4, 1, 0};
    }
    auto top = device_->create({w, h, rap::gpu::format::rgba16f, 1, true});
    if (!data || !top || (image.linear && !image.linear->tone.empty() && !s.tone))
    {
        error = "too big for the GPU";
        return false;
    }
    rap::gpu::pass p;
    p.program = decode_;
    p.inputs = {data};
    p.uniforms = bytes(dp);
    p.target = top;
    device_->run(std::span(&p, 1));
    s.levels.push_back(top);
    s.measured = false;
    return true;
}

size_t GpuView::Source::bytes() const
{
    size_t n = 0;
    for (const rap::gpu::texture_ptr& t : levels)
        n += size_t(t->width()) * t->height() * 8;  // half float RGBA
    return n;
}

void GpuView::retain(Source& s, const std::shared_ptr<Image>& next, uint64_t next_generation)
{
    std::shared_ptr<Image> image = s.image.lock();
    s.region.reset();  // made again if it's shown at 1:1 again
    if (image && !s.levels.empty() && !(image == next && next_generation != s.generation))
        retained_.insert(retained_.begin(), std::move(s));
    s = Source{};
    // What's gone (images closed), and the oldest past the budget.
    std::erase_if(retained_, [](const Source& r) { return r.image.expired(); });
    const rap::gpu::memory_budget m = device_->memory();
    const size_t budget = m.budget ? size_t(m.budget / 2) : size_t(2) << 30;
    size_t used = 0;
    for (size_t i = 0; i < retained_.size(); ++i)
    {
        used += retained_[i].bytes();
        if (used > budget)
        {
            retained_.resize(i);
            break;
        }
    }
}

bool GpuView::restore(Source& s, const std::shared_ptr<Image>& image, uint64_t generation)
{
    for (auto it = retained_.begin(); it != retained_.end(); ++it)
        if (it->image.lock() == image)
        {
            if (it->generation != generation)
            {
                retained_.erase(it);  // its data changed since: made again
                return false;
            }
            s = std::move(*it);
            retained_.erase(it);
            return true;
        }
    return false;
}

bool GpuView::hold(int n, const std::shared_ptr<Image>& image, uint64_t generation, std::string& error)
{
    Source& s = shown_[n];
    const auto holds = [&](const Source& in) { return in.image.lock() == image && in.generation == generation && !in.levels.empty(); };
    if (holds(s))
        return true;
    if (holds(shown_[n ^ 1]))
    {
        std::swap(s, shown_[n ^ 1]);  // A and B changed places
        return true;
    }
    retain(s, image, generation);
    const bool kept = restore(s, image, generation);
    if (std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "gpu: %s %dx%d (%zu kept, %zu MB)\n", kept ? "kept on the GPU:" : "uploading", image->width, image->height,
                     retained_.size(), [&] { size_t b = 0; for (auto& r : retained_) b += r.bytes(); return b >> 20; }());
    if (!kept)
    {
        const int64_t t0 = g_get_monotonic_time();
        s.stats = device_->create_buffer(kStatsWords * 4);  // this image's measures
        if (!s.stats || !upload(*image, s, error))
        {
            if (error.empty())
                error = "no room for the measures";
            s = Source{};
            return false;
        }
        if (std::getenv("IFRIT_TRACE"))
            std::fprintf(stderr, "gpu: %s in %.1f ms on the UI thread\n", image->mosaic ? "mosaic uploaded and developed" : "uploaded",
                         (g_get_monotonic_time() - t0) / 1000.0);
    }
    s.image = image;
    s.generation = generation;
    return true;
}

void GpuView::follow_detail(Source& s, const Detail& d)
{
    if (s.detailed == d)
        return;
    s.levels.resize(1);
    s.detailed = d;
    ++s.details;
    s.region.reset();
    if (!s.auto_exposure)
        s.measured = false;  // a nonraw's full-resolution luminance includes the detail filter
}

void GpuView::measure_noise(Source& s)
{
    if (s.noise)
        return;
    s.noise = device_->create_buffer(DetailFilter::kNoiseWords * 4);  // noise.hlsl's: two histograms, the two deviations, the model, its fit's counts
    if (!s.noise)
        return;
    device_->clear(s.noise, 0);
    const int w = s.levels[0]->width(), h = s.levels[0]->height();
    // The model fitted (a raw's), then the noise measured in its encoding.
    // The readout's noise from the masked border, where clip.hlsl had it.
    static const bool masked = !std::getenv("IFRIT_NO_MASKED_NOISE");  // to compare
    // Then (modes 4 to 6) the noise's spatial correlation, and from it the
    // law where the develop's factors aren't known (s.measured_law).
    constexpr int kModes = 7;
    NoiseParams params[kModes];
    for (int i = 0; i < kModes; ++i)
    {
        params[i] = {i, {w, h}, s.raw ? 1 : 0, {s.as_shot[0], s.as_shot[1], s.as_shot[2], 1}, {s.clips && masked ? 1 : 0, s.measured_law ? 1 : 0, 0, 0},
                     {s.light[0], s.light[1], s.light[2], 0}, {s.readout[0], s.readout[1], s.readout[2], 0},
                     {s.green[0][0], s.green[0][1], s.green[0][2], 0}, {s.green[1][0], s.green[1][1], s.green[1][2], 0}, {},
                     {s.correlation[0][0], s.correlation[0][1], s.correlation[0][2], 0}, {s.correlation[1][0], s.correlation[1][1], s.correlation[1][2], 0}};
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                params[i].measure[c][k] = s.measure[c][k];
    }
    rap::gpu::dispatch d[kModes];
    for (int i = 0; i < kModes; ++i)
    {
        d[i].program = noise_;
        d[i].inputs = {s.levels[0]};
        d[i].buffers = {s.noise, s.clips ? s.clips : histogram_};
        d[i].uniforms = bytes(params[i]);
    }
    const int blocks = 16, lattice = 4;  // noise.hlsl's kFitLattice, kNoiseStride
    d[0].groups[0] = ((w + blocks - 1) / blocks + 15) / 16;
    d[0].groups[1] = ((h + blocks - 1) / blocks + 15) / 16;
    d[2].groups[0] = ((w + lattice - 1) / lattice + 15) / 16;
    d[2].groups[1] = ((h + lattice - 1) / lattice + 15) / 16;
    const int step = 32;  // noise.hlsl's kSpatialStep: a thread a block (4), a group a block (5)
    d[4].groups[0] = ((w + step - 1) / step + 15) / 16;
    d[4].groups[1] = ((h + step - 1) / step + 15) / 16;
    d[5].groups[0] = (w + step - 1) / step;
    d[5].groups[1] = (h + step - 1) / step;
    device_->run(std::span(d));
    if (std::getenv("IFRIT_TRACE"))
    {
        // (Read back once it's done, not waited for: the filter's timing is its own.)
        auto words = std::make_shared<std::vector<std::byte>>(s.noise->size());
        device_->readback(s.noise, *words,
                          [words, raw = s.raw]
                          {
                              float f[11], pixel[3];
                              uint32_t flags[2];
                              std::memcpy(f, words->data() + (2 * 1024) * 4, sizeof f);
                              std::memcpy(flags, words->data() + (2 * 1024 + 2 + 9) * 4, sizeof flags);
                              std::memcpy(pixel, words->data() + words->size() - 4 * 4, sizeof pixel);  // noise.hlsl's kNoisePixel: last but its flag
                              std::fprintf(stderr,
                                           "gpu: noise %s%s: a R %.3g G %.3g B %.3g, b R %.3g G %.3g B %.3g (colours fitted %u); offset %.3g; "
                                           "luminance %.4g colour %.4g; pixel over measure %.3g %.3g %.3g\n",
                                           !raw ? "of a non-raw (no model)" : flags[0] & 2 ? "fitted" : "not fitted (too few flat areas)",
                                           flags[1] ? ", b from the masked border" : "", f[5], f[6], f[7], f[8], f[9], f[10], flags[0], f[2], f[0], f[1],
                                           pixel[0], pixel[1], pixel[2]);
                          });
    }
}

bool GpuView::filter_detail(Source& s, int x0, int y0, int x1, int y1, const rap::gpu::texture_ptr& target, int tx, int ty)
{
    const int64_t t0 = g_get_monotonic_time();  // the noise's measure, the first time, counted in
    const DetailFilter filter(s.detailed);
    if (filter.denoise)
        measure_noise(s);
    if (filter.denoise && !s.noise)
        return false;
    const rap::gpu::texture_ptr& image = s.levels[0];
    const int width = image->width(), height = image->height();
    constexpr int kTile = 1024, kReach = DetailFilter::kReach;
    // The intermediates: a tile and its reach, 32-bit (the details are
    // small next to the values).
    const int tw = std::min(kTile + 2 * kReach, width), th = std::min(kTile + 2 * kReach, height);
    for (rap::gpu::texture_ptr& t : detail_temps_)
        if (!t || t->width() < tw || t->height() < th)
            if (!(t = device_->create({tw, th, rap::gpu::format::rgba32f, 1, true})))
                return false;
    if (filter.denoise && !s.detail_table && !(s.detail_table = device_->create({NoiseModel::kInverse, 1, rap::gpu::format::rgba32f, 1, true})))
        return false;
    if (filter.denoise && !s.detail_shaping && !(s.detail_shaping = device_->create({7, 6, rap::gpu::format::rgba32f, 1, true})))
        return false;
    // The table again only for another image's noise or another setting.
    bool tabulate = filter.denoise && !(s.table_noise == s.noise && s.table_for == s.detailed);
    DetailParams base{};
    base.image_size[0] = width, base.image_size[1] = height;
    for (int c = 0; c < 3; ++c)
        base.as_shot[c] = s.as_shot[c];
    base.as_shot[3] = 1;
    for (int j = 0; j < 4; ++j)
        base.luminance[j] = filter.luminance[j], base.colour[j] = filter.colour[j], base.support[j] = filter.support[j];
    base.coarsest[0] = filter.luminance[4], base.coarsest[1] = filter.colour[4], base.coarsest[2] = filter.guide;
    base.sharpen[0] = filter.amount, base.sharpen[1] = filter.sigma, base.sharpen[2] = filter.halo;
    base.mask[0] = filter.mask_low, base.mask[1] = filter.mask_high;
    base.sharpening = filter.sharpen ? 1 : 0;
    base.model[0] = filter.denoise ? 1.f : 0.f;  // the noise buffer's encoding (the model's, or as without one)
    base.out_origin[0] = x0 - tx, base.out_origin[1] = y0 - ty;
    rap::gpu::ticket done;
    for (int oy = y0; oy < y1; oy += kTile)
        for (int ox = x0; ox < x1; ox += kTile)
        {
            const int ow = std::min(kTile, x1 - ox), oh = std::min(kTile, y1 - oy);
            const int ix = std::max(0, ox - kReach), iy = std::max(0, oy - kReach);
            const int iw = std::min(width, ox + ow + kReach) - ix, ih = std::min(height, oy + oh + kReach) - iy;
            std::vector<DetailParams> params;
            params.reserve(4 + 3 * DetailFilter::kLevels + 3);  // their bytes are queued with the passes
            std::vector<rap::gpu::pass> passes;
            const auto add = [&](int mode, std::vector<rap::gpu::texture_ptr> inputs, const rap::gpu::texture_ptr& to, int axis = 0,
                                 int step = 0)
            {
                DetailParams& p = params.emplace_back(base);
                p.mode = mode, p.axis = axis, p.step = step;
                p.origin[0] = ix, p.origin[1] = iy, p.size[0] = iw, p.size[1] = ih;
                rap::gpu::pass& pass = passes.emplace_back();
                pass.program = detail_;
                pass.inputs = std::move(inputs);
                if (filter.denoise)
                    pass.buffers = {s.noise};
                pass.uniforms = bytes(p);
                pass.target = to;
                pass.region = to == target ? rap::gpu::rect{ox - x0 + tx, oy - y0 + ty, ow, oh} : rap::gpu::rect{0, 0, iw, ih};
            };
            // detail.hlsl's modes.
            enum { kEncode, kSmooth, kShrink, kCombine, kBlur, kDecode, kTable, kShaping };
            const auto& t = detail_temps_;
            rap::gpu::texture_ptr cur = t[0], next = t[1], sum = t[3], other = t[4];
            if (tabulate)
            {
                // The decode's table, once: per image, noise and setting, not
                // per pixel or region.
                add(kShaping, {blank_}, s.detail_shaping);
                passes.back().region = rap::gpu::rect{0, 0, 7, 6};
                add(kTable, {s.detail_shaping}, s.detail_table);
                passes.back().region = rap::gpu::rect{0, 0, NoiseModel::kInverse, 1};
                s.table_noise = s.noise, s.table_for = s.detailed, tabulate = false;
            }
            add(kEncode, {image}, cur);
            if (filter.denoise)
            {
                // Each scale: smoothed across then down; its detail, shrunk,
                // into the sum.
                for (int j = 0, step = 1; j < DetailFilter::kLevels; ++j, step *= 2)
                {
                    add(kSmooth, {cur}, t[2], 0, step);
                    add(kSmooth, {t[2]}, next, 1, step);
                    add(kShrink, {cur, next, j ? sum : blank_}, other, 0, j);
                    std::swap(cur, next);
                    std::swap(sum, other);
                }
                add(kCombine, {cur, sum}, t[2]);
                cur = t[2];
            }
            if (filter.sharpen)
            {
                add(kBlur, {cur}, sum, 0);
                add(kBlur, {sum}, other, 1);
            }
            add(kDecode, {cur, filter.sharpen ? other : blank_, image, filter.denoise ? s.detail_table : blank_}, target);
            done = device_->run(passes);
        }
    if (std::getenv("IFRIT_TRACE"))
    {
        done.wait();
        std::fprintf(stderr, "gpu: detail filtered %dx%d in %.1f ms\n", x1 - x0, y1 - y0, (g_get_monotonic_time() - t0) / 1000.0);
    }
    return true;
}

int GpuView::check_shaping()
{
    // noise.hlsl's layout (detail.hlsli's kNoise*).
    constexpr int kSigma = 2 * 1024, kModel = kSigma + 2, kLaw = kModel + 17, kInverse = kModel + 32 + 3 * 64 * 128;
    constexpr int kRho = kInverse + 3 * NoiseModel::kInverse + 256 + 1 + NoiseModel::kSpatialSeries * NoiseModel::kSpatialLags;
    constexpr int kPixel = kRho + NoiseModel::kSpatialSeries * NoiseModel::kSpatialLags;
    static_assert(kPixel + 4 == DetailFilter::kNoiseWords);
    constexpr float kNoiseL = 0.01f, kNoiseC = 0.02f;
    std::string error;
    if (!programs(error))
    {
        std::fprintf(stderr, "gpu shaping: %s\n", error.c_str());
        return 1;
    }
    int failed = 0;
    for (int fixture = 0; fixture < 3; ++fixture)
    {
        const bool shifted = fixture == 1, quiet = fixture == 2;
        const NoiseModel m = quiet ? quiet_axis_case() : shaping_case(shifted);
        Detail d;
        d.luminance = quiet ? 1 : 20, d.colour = quiet ? 1 : 25, d.sharpness = quiet ? 0 : 40;
        const DetailFilter filter(d);
        std::vector<float> words(DetailFilter::kNoiseWords, 0.0f);
        words[kSigma] = kNoiseL, words[kSigma + 1] = kNoiseC;
        for (int c = 0; c < 3; ++c)
        {
            words[size_t(kModel + c)] = m.offset[c];
            words[size_t(kModel + 11 + c)] = m.from[c], words[size_t(kModel + 14 + c)] = m.to[c];
            words[size_t(kLaw + c)] = m.law_a[c], words[size_t(kLaw + 3 + c)] = m.law_b[c];
            words[size_t(kPixel + c)] = m.pixel[c];
            for (int i = 0; i < NoiseModel::kInverse; ++i)
                words[size_t(kInverse + c * NoiseModel::kInverse + i)] = m.inverse[c][i];
        }
        const uint32_t fitted = 7, measured = 1;
        std::memcpy(&words[size_t(kModel + 9)], &fitted, 4);
        std::memcpy(&words[size_t(kPixel + 3)], &measured, 4);
        for (int k = 0; k < NoiseModel::kSpatialSeries; ++k)
            for (int i = 0; i < NoiseModel::kSpatialLags; ++i)
                words[size_t(kRho + k * NoiseModel::kSpatialLags + i)] = m.rho[k][i];
        Source s;
        s.noise = device_->create_buffer(DetailFilter::kNoiseWords * 4);
        s.detail_table = device_->create({NoiseModel::kInverse, 1, rap::gpu::format::rgba32f, 1, true});
        s.detail_shaping = device_->create({7, 6, rap::gpu::format::rgba32f, 1, true});
        if (!s.noise || !s.detail_table || !s.detail_shaping)
            return 1;
        device_->upload(s.noise, std::as_bytes(std::span(words)));
        DetailParams p{};
        for (int j = 0; j < 4; ++j)
            p.luminance[j] = filter.luminance[j], p.colour[j] = filter.colour[j];
        p.coarsest[0] = filter.luminance[4], p.coarsest[1] = filter.colour[4];
        p.sharpen[0] = filter.amount, p.sharpen[1] = filter.sigma, p.sharpen[2] = filter.halo;
        p.sharpening = filter.sharpen ? 1 : 0, p.model[0] = 1;
        DetailParams params[2] = {p, p};
        params[0].mode = 7, params[1].mode = 6;  // detail.hlsl's DETAIL_SHAPING, DETAIL_TABLE
        rap::gpu::pass passes[2];
        for (int i = 0; i < 2; ++i)
        {
            passes[i].program = detail_;
            passes[i].buffers = {s.noise};
            passes[i].uniforms = bytes(params[i]);
        }
        passes[0].inputs = {blank_}, passes[0].target = s.detail_shaping, passes[0].region = {0, 0, 7, 6};
        passes[1].inputs = {s.detail_shaping}, passes[1].target = s.detail_table, passes[1].region = {0, 0, NoiseModel::kInverse, 1};
        device_->run(std::span(passes)).wait();
        std::vector<float> shaping(7 * 6 * 4), table(NoiseModel::kInverse * 4);
        device_->readback(s.detail_shaping, std::as_writable_bytes(std::span(shaping)), 7 * 16).wait();
        device_->readback(s.detail_table, std::as_writable_bytes(std::span(table)), NoiseModel::kInverse * 16).wait();
        if (device_->lost())
            return 1;
        // Against the CPU's.
        const NoiseShaping cpu = noise_shaping(&m, &filter);
        static constexpr int pairs[6][2] = {{0, 0}, {1, 1}, {2, 2}, {0, 1}, {0, 2}, {1, 2}};
        double worst = 0;
        for (int q = 0; q < 6; ++q)
        {
            const int a = pairs[q][0], b = pairs[q][1];
            for (int j = 0; j < 7; ++j)
            {
                const float* v = &shaping[size_t((q * 7 + j) * 4)];
                const float want[3] = {j < 6 ? cpu.k[j][a][b] : cpu.D[a][b], j < 6 ? cpu.d[j][a][b] : cpu.X[a][b], j < 6 ? cpu.x[j][a][b] : 0};
                for (int i = 0; i < 3; ++i)
                    worst = std::max(worst, double(std::fabs(v[i] - want[i])));
            }
        }
        float tl[5], tc[5];
        for (int j = 0; j < 5; ++j)
            tl[j] = filter.luminance[j] * kNoiseL, tc[j] = filter.colour[j] * kNoiseC;
        static float shift[3][NoiseModel::kInverse];
        noise_decode_table(m, tl, tc, cpu, shift);
        double table_worst = 0, largest = 0;
        int worst_i = 0, worst_c = 0;
        for (int i = 0; i < NoiseModel::kInverse; ++i)
            for (int c = 0; c < 3; ++c)
            {
                if (double(std::fabs(table[size_t(i * 4 + c)] - shift[c][i])) > table_worst)
                    table_worst = double(std::fabs(table[size_t(i * 4 + c)] - shift[c][i])), worst_i = i, worst_c = c;
                largest = std::max(largest, double(std::fabs(shift[c][i])));
            }
        double endpoint = 0;
        if (quiet)
            std::printf("quiet table worst entry %d channel %d: gpu %.9g cpu %.9g range %.9g..%.9g\n", worst_i, worst_c, table[size_t(worst_i * 4 + worst_c)], shift[worst_c][worst_i], m.from[worst_c], m.to[worst_c]);
        if (quiet)
        {
            float gpu_shift[3][NoiseModel::kInverse];
            for (int c = 0; c < 3; ++c)
                for (int i = 0; i < NoiseModel::kInverse; ++i)
                    gpu_shift[c][i] = table[size_t(i * 4 + c)];
            const float d0 = std::sqrt(.0003f + m.offset[2]);
            const double sigma = std::sqrt(double(m.law_a[2]) * .0003 + m.law_b[2]);
            endpoint = std::fabs(noise_decode_by(m, gpu_shift, d0, 2) - .000299325009109) / sigma;
            std::printf("%s gpu quiet blue active endpoint against double transport: %.7g sigma\n", endpoint < .005 ? "ok    " : "FAILED", endpoint);
        }
        const bool ok = worst < 1e-4 && table_worst <= 2e-3 * largest + 1e-9 && endpoint < .005;
        failed += !ok;
        std::printf("%s gpu shaping, %s: against the CPU's, %.2g; the decode's table, %.2g of its largest shift %.3g\n", ok ? "ok    " : "FAILED",
                    quiet ? "quiet blue" : shifted ? "green red's a pixel on" : "independent", worst, largest > 0 ? table_worst / largest : 0.0, largest);
    }
    return failed;
}

int GpuView::check_negative_measures()
{
    // A raw developed here, every pixel (-0.1, 0.8, 0.2) in BT.709 (some
    // brightness, red below 0), exposure +2 on a treated raw's anchor of 2
    // (scale 8): its luminance as edited, measured (stats.hlsl's CURVE and
    // TOP, volume.hlsl's top), against what tone_map() is given, the colour as
    // compose.hlsl takes it (view.hlsli's tone_input()). Held at 0, red would
    // make it 3.8% more.
    constexpr int kSize = 16;
    constexpr float kRgb[3] = {-0.1f, 0.8f, 0.2f};
    static constexpr float kToXyz[3][3] = {{0.4124564f, 0.3575761f, 0.1804375f}, {0.2126729f, 0.7151522f, 0.0721750f}, {0.0193339f, 0.1191920f, 0.9503041f}};
    auto image = std::make_shared<Image>();
    image->allocate(kSize, kSize);
    image->is_raw = true;
    Source s;
    s.image = image;
    s.generation = 1;
    s.raw = s.tonable = s.auto_exposure = s.measured = true;
    s.anchor = 2;
    s.measures = 1;
    s.stats = device_->create_buffer(kStatsWords * 4);
    auto texture = device_->create({kSize, kSize, rap::gpu::format::rgba32f});
    if (!s.stats || !texture)
        return 1;
    device_->clear(s.stats, 0);
    std::memcpy(s.input_to_xyz, kToXyz, sizeof kToXyz);
    std::vector<float> pixels(size_t(kSize) * kSize * 4);
    for (size_t i = 0; i < size_t(kSize) * kSize; ++i)
        pixels[i * 4] = kRgb[0], pixels[i * 4 + 1] = kRgb[1], pixels[i * 4 + 2] = kRgb[2], pixels[i * 4 + 3] = 1;
    device_->upload(texture, std::as_bytes(std::span(pixels)), {4, rap::gpu::sample_type::f32});
    s.levels = {texture};
    shown_[0] = std::move(s);
    Frame f;
    f.width = f.height = kSize;
    f.zoom = 1;
    f.exposure = 2;
    f.treated = true;
    rap::gpu::texture_ptr out;
    rap::gpu::ticket ready;
    rap::frame_color colour;
    std::string error;
    if (!render(image, 1, f, out, ready, colour, error))
    {
        std::fprintf(stderr, "negative measures: %s\n", error.c_str());
        return 1;
    }
    ready.wait();
    std::vector<uint32_t> words(kStatsWords);
    device_->readback(shown_[0].stats, std::as_writable_bytes(std::span(words))).wait();
    shown_[0] = Source{};
    if (device_->lost())
        return 1;
    const auto at = [&](uint32_t i)
    {
        float v;
        std::memcpy(&v, &words[i], 4);
        return v;
    };
    // edit()'s toe on a raw's luminance, y^2 / (y + 0.005).
    const auto edited = [](float y) { return y * y / (y + 0.005f); };
    float y = 0, clamped = 0;
    for (int c = 0; c < 3; ++c)
        y += kToXyz[1][c] * kRgb[c], clamped += kToXyz[1][c] * std::max(kRgb[c], 0.0f);
    const float want = edited(y * 8), held = edited(clamped * 8);
    int failed = 0;
    for (const auto& [name, index] : {std::pair{"CURVE's greatest", kStatsCurveMost}, {"TOP's greatest", kStatsTopMost}, {"the top", kStatsTop}})
    {
        const float got = at(index), off = got / want - 1;
        const bool ok = std::fabs(off) < 1e-4f;
        failed += !ok;
        std::printf("%s negative measures: %s %.6f, tone_map()'s input luminance %.6f (%+.4f%%; held at 0: %.6f)\n", ok ? "ok    " : "FAILED", name, got,
                    want, 100 * off, held);
    }
    // A signed BT.2020 green in BT.709 coordinates. Its negative red/blue
    // are colour data, not raw readout noise. Both full-resolution stats
    // and tone_map() must receive the same unchanged colour via tone_input.
    {
        constexpr float rgb[3] = {-.587641f, 1.132900f, -.100579f};
        Source nonraw;
        nonraw.image = image;
        nonraw.generation = 2;
        nonraw.tonable = nonraw.measure_colour = true;
        nonraw.stats = device_->create_buffer(kStatsWords * 4);
        if (!nonraw.stats)
            return 1;
        std::memcpy(nonraw.input_to_xyz, kToXyz, sizeof kToXyz);
        for (size_t i = 0; i < size_t(kSize) * kSize; ++i)
            std::copy(rgb, rgb + 3, pixels.data() + i * 4);
        device_->upload(texture, std::as_bytes(std::span(pixels)), {4, rap::gpu::sample_type::f32});
        nonraw.levels = {texture};
        shown_[0] = std::move(nonraw);
        image->is_raw = false;
        Frame nf;
        nf.width = nf.height = kSize;
        nf.front = FrontBuffer::ScRgb16;
        nf.volume_probe = 8;  // tone_map's extent/lightness, before any fitting
        if (!render(image, 2, nf, out, ready, colour, error))
        {
            std::fprintf(stderr, "signed nonraw measures: %s\n", error.c_str());
            return 1;
        }
        ready.wait();
        device_->readback(shown_[0].stats, std::as_writable_bytes(std::span(words))).wait();
        std::vector<uint16_t> measured_pixels(size_t(kSize) * kSize * 4);
        device_->readback(out, std::as_writable_bytes(std::span(measured_pixels)), kSize * 8).wait();
        shown_[0] = Source{};
        if (device_->lost())
            return 1;
        float expected = 0;
        for (int c = 0; c < 3; ++c)
            expected += kToXyz[1][c] * rgb[c];
        const float top = at(kStatsMaximum);
        const bool top_ok = std::fabs(top - expected) < 1e-5f;
        failed += !top_ok;
        std::printf("%s signed nonraw measures: top %.7g, tone_input luminance %.7g\n", top_ok ? "ok    " : "FAILED", top, expected);
        float extent = 0;
        for (uint32_t i = 0; i < 72 * 32; ++i)
            extent = std::max(extent, at(kStatsVolumeRaw + i * 2));
        const float composed = half_to_float(measured_pixels[0]);
        const bool extent_ok = extent > 1 && std::fabs(composed - extent) < 2e-3f * extent;
        failed += !extent_ok;
        std::printf("%s signed nonraw measures: extent %.7g, tone_map extent %.7g\n", extent_ok ? "ok    " : "FAILED", extent, composed);
    }
    return failed;
}

rap::gpu::texture_ptr GpuView::detail_region(Source& s, int x0, int y0, int x1, int y1, int& ox, int& oy)
{
    const int* r = s.region_at;
    if (!s.region || x0 < r[0] || y0 < r[1] || x1 > r[2] || y1 > r[3])
    {
        // A quarter more each way, to pan into.
        const int width = s.levels[0]->width(), height = s.levels[0]->height();
        const int mx = (x1 - x0) / 4, my = (y1 - y0) / 4;
        const int a[4] = {std::max(0, x0 - mx), std::max(0, y0 - my), std::min(width, x1 + mx), std::min(height, y1 + my)};
        s.region = device_->create({a[2] - a[0], a[3] - a[1], rap::gpu::format::rgba16f, 1, true});
        if (!s.region || !filter_detail(s, a[0], a[1], a[2], a[3], s.region, 0, 0))
        {
            s.region.reset();
            ox = oy = 0;
            return s.levels[0];  // unfiltered, rather than nothing
        }
        std::copy(std::begin(a), std::end(a), s.region_at);
    }
    ox = r[0], oy = r[1];
    return s.region;
}

rap::gpu::texture_ptr GpuView::full_level(Source& s)
{
    if (!s.detailed.active())
        return s.levels[0];
    int ox, oy;
    return detail_region(s, 0, 0, s.levels[0]->width(), s.levels[0]->height(), ox, oy);
}

rap::gpu::texture_ptr GpuView::counted_level(Source& s)
{
    int n = 0;
    while (std::max(s.levels[0]->width(), s.levels[0]->height()) >> n > 2048)
        ++n;
    return level(s, n);
}

rap::gpu::texture_ptr GpuView::level(Source& s, int n)
{
    while (int(s.levels.size()) <= n)
    {
        // With noise reduction or sharpening, level 1 is made from level 0
        // filtered (the whole of it, let go once level 1 is made), and the
        // rest from level 1.
        rap::gpu::texture_ptr above = s.levels.back();
        if (s.levels.size() == 1 && s.detailed.active())
        {
            const int w = above->width(), h = above->height();
            auto filtered = device_->create({w, h, rap::gpu::format::rgba16f, 1, true});
            if (filtered && filter_detail(s, 0, 0, w, h, filtered, 0, 0))
                above = std::move(filtered);
        }
        const int w = above->width(), h = above->height();
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        if (nw == w && nh == h)
            return s.levels.back();
        // Halved with Lanczos-3, across then down, in linear light.
        auto across = device_->create({nw, h, rap::gpu::format::rgba16f, 1, true});
        auto next = device_->create({nw, nh, rap::gpu::format::rgba16f, 1, true});
        if (!across || !next)
            return s.levels.back();
        const ResampleParams px{0, int32_t(ScaleFilter::Lanczos3), float(w) / nw, 0, w, 0, {}};
        const ResampleParams py{1, int32_t(ScaleFilter::Lanczos3), float(h) / nh, 0, h, 0, {}};
        rap::gpu::pass passes[2];
        passes[0].program = resample_;
        passes[0].inputs = {above};
        passes[0].uniforms = bytes(px);
        passes[0].target = across;
        passes[1].program = resample_;
        passes[1].inputs = {across};
        passes[1].uniforms = bytes(py);
        passes[1].target = next;
        device_->run(passes);
        s.levels.push_back(next);
    }
    return s.levels[size_t(n)];
}

bool GpuView::begin_frame(const Frame& f, rap::gpu::texture_ptr& out, rap::frame_color& colour, std::string& error)
{
    static constexpr rap::gpu::format kFormats[] = {rap::gpu::format::rgba8, rap::gpu::format::rgb10a2, rap::gpu::format::rgb10a2,
                                                    rap::gpu::format::rgba16f};
    const rap::gpu::format format = kFormats[int(f.front)];
    rap::gpu::texture_ptr& frame = frames_[next_frame_];
    if (!frame || frame->width() != f.width || frame->height() != f.height || frame->format() != format)
        frame = device_->create({f.width, f.height, format, 1, true});
    if (!frame)
    {
        error = "no room for the frame";
        return false;
    }
    next_frame_ ^= 1;
    out = frame;
    colour = {};
    colour.premultiplied = false;  // opaque: composited over the background
    colour.sdr_white_nits = f.sdr_white_nits;
    if (f.front == FrontBuffer::Pq10)
        colour.curve = rap::transfer::pq;  // BT.709 primaries until the gamut is mapped to the display's
    else if (f.front == FrontBuffer::ScRgb16)
        colour.curve = rap::transfer::linear;
    return true;
}

// The display's peak, in SDR whites: SDR white until rapport describes the
// display. A test hook: IFRIT_DISPLAY_PEAK for a display brighter than SDR
// white, with --gpu-render's .pfm output to keep what's over it.
static float display_peak()
{
    static const float peak = [] {
        const char* v = std::getenv("IFRIT_DISPLAY_PEAK");
        return v ? std::max(1.0f, float(std::atof(v))) : 1.0f;
    }();
    return peak;
}

// The display's boundary in Oklab, for view.hlsli's display_boundary: its
// most chroma at each of 720 hues and 256 lightnesses from black to its
// white, BT.709 primaries, at `peak`. Exact at each point: along a chroma ray
// each channel is a cubic in chroma, and the first to leave 0..peak is
// found by halving (24 times: to 0.6 / 2^24 of the white's lightness, under
// 4e-8), the cubes multiplied out (std::pow took 0.7 s), the hues split over
// the cores. Made once per peak, only for a raw (view_inputs()), and for the
// display's own peak begun as the view is made (GpuView::GpuView), off the
// drawing thread.
static constexpr int kBoundaryHues = 720, kBoundaryLevels = 256;

static std::vector<float> boundary_table(float peak)
{
    std::vector<float> table(size_t(kBoundaryHues) * kBoundaryLevels);
    const double top = std::cbrt(double(peak));
    const auto hues = [&](int from, int to)
    {
        for (int j = from; j < to; ++j)
        {
            const double h = 2 * 3.14159265358979323846 * j / kBoundaryHues, a = std::cos(h), b = std::sin(h);
            const double k[3] = {0.3963377774 * a + 0.2158037573 * b, -0.1055613458 * a - 0.0638541728 * b,
                                 -0.0894841775 * a - 1.2914855480 * b};
            for (int i = 0; i < kBoundaryLevels; ++i)
            {
                const double L = top * i / (kBoundaryLevels - 1);
                double lo = 0, hi = i == 0 || i == kBoundaryLevels - 1 ? 0 : 0.6 * top;
                for (int n = 0; n < 24 && hi > 0; ++n)
                {
                    const double mid = 0.5 * (lo + hi);
                    double lms[3];
                    for (int c = 0; c < 3; ++c)
                    {
                        const double v = L + mid * k[c];
                        lms[c] = v * v * v;
                    }
                    const double rgb[3] = {4.0767416621 * lms[0] - 3.3077115913 * lms[1] + 0.2309699292 * lms[2],
                                           -1.2684380046 * lms[0] + 2.6097574011 * lms[1] - 0.3413193965 * lms[2],
                                           -0.0041960863 * lms[0] - 0.7034186147 * lms[1] + 1.7076147010 * lms[2]};
                    bool inside = true;
                    for (double v : rgb)
                        inside = inside && v >= 0 && v <= peak;
                    (inside ? lo : hi) = mid;
                }
                table[size_t(i) * kBoundaryHues + j] = float(lo);
            }
        }
    };
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t)
        pool.emplace_back(hues, kBoundaryHues * t / threads, kBoundaryHues * (t + 1) / threads);
    hues(0, kBoundaryHues / threads);
    for (std::thread& t : pool)
        t.join();
    return table;
}

const rap::gpu::texture_ptr& GpuView::boundary(float peak)
{
    if (boundary_ && boundary_peak_ == peak)
        return boundary_;
    std::vector<float> table;
    if (boundary_ahead_.valid() && boundary_ahead_peak_ == peak)
        table = boundary_ahead_.get();
    else
        table = boundary_table(peak);
    boundary_ahead_ = {};
    boundary_ = device_->create({kBoundaryHues, kBoundaryLevels, rap::gpu::format::r32f});
    if (boundary_)
        device_->upload(boundary_, std::as_bytes(std::span(table)), {1, rap::gpu::sample_type::f32});
    boundary_peak_ = peak;
    return boundary_ ? boundary_ : blank_;
}

// Whether the image's measures map it to the display (view.hlsli's
// measured), and with that whether it's a plain image that needs no
// mapping (passthrough): its data the display's own linear BT.709, not a
// raw, no camera curve, no edit. The shader also checks that the image
// needs no mapping when it has been measured. Set only here, from the
// measured state the pass will run with, so neither can go stale.
static void set_measured(ComposeParams& cp, bool measured)
{
    cp.measured = measured ? 1 : 0;
    bool plain = !cp.auto_exposure && !cp.tone && !cp.adjusting;
    const float* rows[3] = {cp.input_x, cp.input_y, cp.input_z};
    for (int i = 0; i < 3 && plain; ++i)
        for (int j = 0; j < 3 && plain; ++j)
            plain = std::abs(rows[i][j] - kXyzFrom709[i][j]) < 1e-6f;
    cp.passthrough = plain ? 1 : 0;
}

// The compose pass's parameters for showing `s` as `f` has it, but for
// the image's place and the resampling.
static void show_params(const GpuView::Frame& f, float scene_gain, bool tonable, bool tone, bool auto_exposure, float anchor,
                        float white_luminance, const float input_to_xyz[3][3], const float as_shot[3], const DngColour* dng, bool measured, ComposeParams& cp)
{
    set_linear(cp.background, f.background);
    set_linear(cp.checker_a, rap::color{102, 102, 102});
    set_linear(cp.checker_b, rap::color{140, 140, 140});
    cp.exposure = scene_gain * float(tonable ? std::pow(2.0, f.exposure) : 1.0);
    cp.tone = tone ? 1 : 0;
    cp.output = int32_t(f.front);
    cp.white = f.sdr_white_nits;
    // A raw's white balance (the frame's, else as shot), in front of its matrix.
    float wb[3] = {1, 1, 1};
    if (auto_exposure)
        for (int j = 0; j < 3; ++j)
            wb[j] = f.white_balance[0] > 0 ? f.white_balance[j] : as_shot[j];
    if (dng && auto_exposure)  // a DNG's matrices are the white's own
    {
        const Mat3 m = dng_camera_to_xyz(*dng, {1 / wb[0], 1 / wb[1], 1 / wb[2]});
        for (int j = 0; j < 3; ++j)
            cp.input_x[j] = float(m[0][j]), cp.input_y[j] = float(m[1][j]), cp.input_z[j] = float(m[2][j]);
    }
    else
        for (int j = 0; j < 3; ++j)
        {
            cp.input_x[j] = input_to_xyz[0][j] * wb[j];
            cp.input_y[j] = input_to_xyz[1][j] * wb[j];
            cp.input_z[j] = input_to_xyz[2][j] * wb[j];
        }
    cp.auto_exposure = auto_exposure ? 1 : 0;
    cp.anchor = auto_exposure ? anchor : 0;
    cp.white_luminance = auto_exposure ? white_luminance : 0;
    cp.treated = auto_exposure && f.treated ? 1 : 0;
    if (const AdjustTables* t = f.adjust.get())
    {
        cp.adjusting = 1 | (t->channels ? 2 : 0);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                cp.to_working[i][j] = t->to_working[i][j], cp.from_working[i][j] = t->from_working[i][j];
        cp.to_working[0][3] = t->tone_top;
    }
    set_measured(cp, measured);  // (after the input matrix, tone and edits above: passthrough reads them)
    // The display: sRGB at SDR white until rapport describes the real one.
    std::memcpy(cp.display_r, kSrgbR, sizeof cp.display_r);
    std::memcpy(cp.display_g, kSrgbG, sizeof cp.display_g);
    std::memcpy(cp.display_b, kSrgbB, sizeof cp.display_b);
    cp.display_peak = display_peak();
    // A test hook: IFRIT_LC_WEIGHT in place of view.hlsli's kLightnessChromaWeight.
    static const float lc_weight = [] {
        const char* v = std::getenv("IFRIT_LC_WEIGHT");
        return v ? std::max(0.0f, float(std::atof(v))) : 0.0f;
    }();
    cp.lc_weight = lc_weight;
    // A test hook: IFRIT_MIN_SLOPE in place of view.hlsli's
    // kMinBrightnessSlope (0: the projection not held to one).
    static const float min_slope = [] {
        const char* v = std::getenv("IFRIT_MIN_SLOPE");
        return v ? std::max(0.0f, float(std::atof(v))) : -1.0f;
    }();
    cp.min_slope = min_slope;
    // A test hook: IFRIT_HK_PROBE (view.hlsli's hk_probe).
    static const int hk_probe = [] {
        const char* v = std::getenv("IFRIT_HK_PROBE");
        return v ? std::atoi(v) : 0;
    }();
    cp.hk_probe = hk_probe | (f.volume_probe * 4);
    if (f.histogram)
    {
        cp.show_histogram = 1;
        cp.histogram_channels = uint32_t(f.histogram_channels);
        cp.panel[0] = float(f.panel[0]), cp.panel[1] = float(f.panel[1]);
        cp.panel[2] = float(f.panel[0] + f.panel[2]), cp.panel[3] = float(f.panel[1] + f.panel[3]);
    }
}

std::vector<rap::gpu::texture_ptr> GpuView::view_inputs(const rap::gpu::texture_ptr& image, const Source& s, const Frame* f,
                                                       const rap::gpu::texture_ptr& extra)
{
    // The same display boundary for raw and ordinary image volume mapping.
    std::vector<rap::gpu::texture_ptr> in{image, s.tone ? s.tone : blank_, extra ? extra : blank_,
                                          (s.auto_exposure || (s.measured && s.needs_mapping(f && f->adjust))) ? boundary(display_peak()) : blank_, blank_, blank_};
    const AdjustTables* t = f ? f->adjust.get() : nullptr;
    if (!t)
        return in;
    DeviceTables* d = nullptr;
    for (DeviceTables& c : tables_)
        if (c.serial == t->serial)
            d = &c;
    if (!d)
    {
        d = &tables_[next_tables_];
        next_tables_ ^= 1;
        d->serial = 0;
        d->tone = device_->create({AdjustTables::kTone, 1, rap::gpu::format::rgba32f});
        d->colour = device_->create({AdjustTables::kHues, AdjustTables::kSats, rap::gpu::format::rgba32f});
        if (!d->tone || !d->colour)
            return in;
        device_->upload(d->tone, std::as_bytes(std::span(t->tone)), {4, rap::gpu::sample_type::f32});
        device_->upload(d->colour, std::as_bytes(std::span(t->colour)), {4, rap::gpu::sample_type::f32});
        d->serial = t->serial;
    }
    in[4] = d->tone, in[5] = d->colour;
    return in;
}

void GpuView::measure(Source& s, std::span<const std::byte> params, bool always)
{
    static const bool unmeasured = std::getenv("IFRIT_NO_MEASURE");  // to tell the measures' part in a fault
    ComposeParams cp{};
    std::memcpy(&cp, params.data(), std::min(params.size(), sizeof cp));
    const bool mapping = s.needs_mapping(cp.adjusting != 0);
    if ((s.measured && (!mapping || s.mapping_measured)) || !(mapping || always) || unmeasured)
        return;
    const rap::gpu::texture_ptr counted = s.auto_exposure || !mapping ? counted_level(s) : full_level(s);
    if (std::getenv("IFRIT_TRACE"))
        std::fprintf(stderr, "gpu: measuring %dx%d (%s)\n", counted->width(), counted->height(), mapping ? "display mapping" : "histogram only");
    device_->clear(s.stats, 0);
    // The level's measures, then what peak.hlsl makes of them: taken once, at
    // the white balance it's first shown with, the reference a raw's
    // automatic exposure is set by (a change of white balance or edit
    // doesn't move it). Its brightness curve's are measure_volume()'s.
    rap::gpu::dispatch d[2];
    for (int i = 0; i < 2; ++i)
    {
        d[i].program = i == 0 ? stats_program_ : peak_;
        d[i].inputs = view_inputs(counted, s, nullptr);
        d[i].buffers = {histogram_, s.stats, s.clips ? s.clips : histogram_};
        d[i].uniforms = params;  // copied at run()
        d[i].groups[0] = i == 1 ? 1 : (counted->width() + 15) / 16;
        d[i].groups[1] = i == 1 ? 1 : (counted->height() + 15) / 16;
    }
    device_->run(std::span(d));
    s.measured = true;
    s.mapping_measured = mapping;
    if (s.auto_exposure && std::getenv("IFRIT_TRACE"))
    {
        // (Read back once they're made: the exposure and what it came from.)
        auto read = std::make_shared<std::vector<std::byte>>(kStatsWords * 4);
        const float anchor = s.anchor, white = s.white_luminance;
        device_->readback(s.stats, *read,
                          [read, anchor, white]
                          {
                              const auto at = [&](uint32_t i)
                              {
                                  float f;
                                  std::memcpy(&f, read->data() + i * 4, 4);
                                  return f;
                              };
                              const float average = at(kStatsAverage), adapting = at(kStatsAdapting), key = at(kStatsKey), gain = at(kStatsAutoGain);
                              std::fprintf(stderr,
                                           "gpu: exposure %+.2f EV from the camera's (gain %.4g, anchor %.4g): white %.4g cd/m2, L_A %.4g cd/m2 "
                                           "(log average %.4g of the develop's white, itself %.4g of the stated), key %.4g, p99 %.4g\n",
                                           std::log2(gain / anchor), gain, anchor, white, adapting, average, white > 0 ? adapting / (average * white) : 1.0f, key,
                                           at(kStatsP99));
                          });
    }
    ++s.measures;
}

// A raw's brightness curve and measure against the display's volume
// (volume.hlsl): after the image's measures, and again whenever what they're
// measured through changes (the exposure, white balance, develop edits,
// noise reduction and sharpening, the display's peak). The curve from the
// luminance as edited (stats.hlsl's CURVE on the measured level, TOP at full
// resolution), then the volume through it.
void GpuView::measure_volume(Source& s, const Frame& f, std::span<const std::byte> params)
{
    if (!s.measured)
        return;
    ComposeParams cp{};
    std::memcpy(&cp, params.data(), std::min(params.size(), sizeof cp));
    // A histogram may have measured a plain SDR image, but it still needs
    // no image-volume pass. Keep #42's exact passthrough, including after
    // opening the histogram. Edits/detail can leave its original gamut.
    if (!s.needs_mapping(cp.adjusting != 0))
        return;
    std::array<float, 16> key{cp.exposure, cp.display_peak};
    for (int j = 0; j < 4; ++j)
        key[2 + j] = cp.input_x[j], key[6 + j] = cp.input_y[j], key[10 + j] = cp.input_z[j];
    key[15] = cp.white;
    key[14] = float(cp.treated);  // a treated raw's gain is the camera's (auto_gain())
    const uint64_t adjusted = f.adjust ? f.adjust->serial : 0;
    if (s.volume_measures == s.measures && s.volume_adjust == adjusted && s.volume_key == key && s.volume_details == s.details)
        return;
    const rap::gpu::texture_ptr counted = s.auto_exposure ? counted_level(s) : full_level(s);
    set_measured(cp, true);
    const rap::gpu::program_ptr programs[6] = {volume_clear_, stats_curve_, stats_top_, volume_curve_, volume_, volume_prepare_};
    rap::gpu::dispatch d[6];
    int passes = 0;
    for (int i = 0; i < 6; ++i)
    {
        if (!s.auto_exposure && i >= 1 && i <= 3)
            continue;
        const int at = passes++;
        const rap::gpu::texture_ptr& from = i == 2 ? s.levels[0] : counted;
        const bool per_pixel = i == 1 || i == 2 || i == 4;
        d[at].program = programs[i];
        d[at].inputs = view_inputs(from, s, &f);
        d[at].buffers = {histogram_, s.stats};
        d[at].uniforms = bytes(cp);  // copied at run()
        d[at].groups[0] = per_pixel ? (from->width() + 15) / 16 : 1;
        d[at].groups[1] = per_pixel ? (from->height() + 15) / 16 : 1;
    }
    device_->run(std::span(d, passes));
    s.volume_key = key;
    s.volume_adjust = adjusted;
    s.volume_measures = s.measures;
    s.volume_details = s.details;
    // A test hook: IFRIT_CURVE_LOG prints each curve made (log2 knee and
    // top, the automatic exposure, the middle edge) to stderr.
    static const bool log_curve = std::getenv("IFRIT_CURVE_LOG");
    if (log_curve)
    {
        std::vector<uint32_t> words(kStatsWords);
        device_->readback(s.stats, std::as_writable_bytes(std::span(words))).wait();
        const auto at = [&](uint32_t i) { float v; std::memcpy(&v, &words[i], 4); return v; };
        std::fprintf(stderr, "curve: knee %.4f top %.4f gain %.5f mid %.4f details %llu\n", at(kStatsCurve), at(kStatsCurve + 1),
                     at(kStatsAutoGain), at(kStatsCurve + 2 + 64), (unsigned long long)s.details);
    }
}

void GpuView::count(Source& s, const Frame& f)
{
    follow_detail(s, f.detail);  // the levels it counts are made with it
    ComposeParams cp{};
    show_params(f, s.scene_gain, s.tonable, bool(s.tone), s.auto_exposure, s.anchor, s.white_luminance, s.input_to_xyz, s.as_shot, s.dng.get(), s.measured, cp);
    const bool fresh = !s.measured;
    measure(s, bytes(cp), true);
    measure_volume(s, f, bytes(cp));
    set_measured(cp, s.measured && s.needs_mapping(cp.adjusting != 0));
    // The histogram of what's shown: again when the image, its exposure,
    // white balance, develop settings or detail change.
    const std::shared_ptr<Image> image = s.image.lock();
    const uint64_t adjusted = f.adjust ? f.adjust->serial : 0;
    const bool same_wb = std::equal(std::begin(histogram_wb_), std::end(histogram_wb_), std::begin(f.white_balance));
    if (histogram_image_.lock() == image && histogram_generation_ == s.generation && histogram_exposure_ == f.exposure &&
        histogram_adjust_ == adjusted && same_wb && histogram_detail_ == f.detail && histogram_treated_ == f.treated && !fresh)
        return;
    const rap::gpu::texture_ptr counted = counted_level(s);
    device_->clear(histogram_, 0);
    rap::gpu::dispatch d[2];
    d[0].program = histogram_program_;
    d[0].inputs = view_inputs(counted, s, &f);
    d[0].buffers = {histogram_, s.stats};
    d[0].uniforms = bytes(cp);
    d[0].groups[0] = (counted->width() + 15) / 16;
    d[0].groups[1] = (counted->height() + 15) / 16;
    d[1].program = histogram_max_;
    d[1].buffers = {histogram_};
    d[1].uniforms = bytes(cp);
    device_->run(std::span(d));
    histogram_image_ = image;
    histogram_generation_ = s.generation;
    histogram_exposure_ = f.exposure;
    histogram_adjust_ = adjusted;
    histogram_detail_ = f.detail;
    histogram_treated_ = f.treated;
    std::copy(std::begin(f.white_balance), std::end(f.white_balance), std::begin(histogram_wb_));
    histogram_scale_ = cp.exposure;
    histogram_scene_ = s.tonable;
    histogram_auto_ = s.auto_exposure;
    histogram_fixed_ = s.auto_exposure && f.treated ? s.anchor : 0;
    histogram_stats_ = s.stats;
}

bool GpuView::draw(Source& s, int width, int height, bool alpha, const Frame& f, const rap::gpu::texture_ptr& frame, int x0, int x1,
                   rap::gpu::ticket& ready, std::string& error)
{
    x0 = std::clamp(x0, 0, f.width), x1 = std::clamp(x1, 0, f.width);
    if (x1 <= x0)
        return true;  // `ready` left as it was
    follow_detail(s, f.detail);
    ComposeParams cp{};
    show_params(f, s.scene_gain, s.tonable, bool(s.tone), s.auto_exposure, s.anchor, s.white_luminance, s.input_to_xyz, s.as_shot, s.dng.get(), s.measured, cp);
    measure(s, bytes(cp), false);
    measure_volume(s, f, bytes(cp));
    set_measured(cp, s.measured && s.needs_mapping(cp.adjusting != 0));
    const double shown_w = width * f.zoom, shown_h = height * f.zoom;
    cp.image_rect[0] = float(f.pan_x), cp.image_rect[1] = float(f.pan_y);
    cp.image_rect[2] = float(f.pan_x + shown_w), cp.image_rect[3] = float(f.pan_y + shown_h);
    cp.checker_size = f.checkerboard && alpha ? std::max(1, f.checker_size) : 0;

    // A whole-pixel 1:1 is a copy; enlarging with no filter is nearest.
    const bool one_to_one = std::fabs(f.zoom - 1) < 1e-9 && f.pan_x == std::floor(f.pan_x) && f.pan_y == std::floor(f.pan_y);
    const ScaleFilter filter = f.zoom > 1 ? f.enlarge : f.shrink;
    std::vector<rap::gpu::pass> passes;
    // A pass's uniforms are read at run(), below: they live as long.
    ResampleParams first_params{};
    rap::gpu::pass last;
    last.program = compose_;
    last.target = frame;
    last.region = {x0, 0, x1 - x0, f.height};
    if (one_to_one || filter == ScaleFilter::Nearest)
    {
        cp.nearest = 1;
        cp.inv = cp.inv_x = float(1 / f.zoom);
        cp.offset_x = float(-f.pan_x / f.zoom);
        cp.offset = float(-f.pan_y / f.zoom);
        cp.columns = width;
        cp.rows = height;
        rap::gpu::texture_ptr src = s.levels[0];
        if (s.detailed.active())
        {
            // Level 0 filtered over what's shown (its image position off the offsets).
            const int vx0 = std::clamp(int(std::floor((x0 - f.pan_x) / f.zoom)) - 1, 0, width);
            const int vx1 = std::clamp(int(std::ceil((x1 - f.pan_x) / f.zoom)) + 1, 0, width);
            const int vy0 = std::clamp(int(std::floor(-f.pan_y / f.zoom)) - 1, 0, height);
            const int vy1 = std::clamp(int(std::ceil((f.height - f.pan_y) / f.zoom)) + 1, 0, height);
            int ox = 0, oy = 0;
            src = detail_region(s, vx0, vy0, std::max(vx1, vx0 + 1), std::max(vy1, vy0 + 1), ox, oy);
            cp.offset_x -= float(ox);
            cp.offset -= float(oy);
            cp.columns = src->width();
            cp.rows = src->height();
        }
        last.inputs = view_inputs(src, s, &f);
    }
    else
    {
        // The level at most twice the size it's shown at.
        int n = 0;
        while (f.zoom * double(1 << (n + 1)) <= 1.0 + 1e-9 && n < 30)
            ++n;
        rap::gpu::texture_ptr src = level(s, n);
        const int lw = src->width(), lh = src->height();
        const double inv_x = lw / shown_w, inv_y = lh / shown_h;  // level texels per frame pixel
        if (n > 0)
            s.region.reset();  // zoomed out of level 0: let it go

        // The frame columns the image covers (of those drawn), and the level
        // rows the second pass reads for the frame rows it covers.
        const int c0 = std::clamp(int(std::floor(f.pan_x)), x0, x1), c1 = std::clamp(int(std::ceil(f.pan_x + shown_w)), x0, x1);
        const int y0 = std::clamp(int(std::floor(f.pan_y)), 0, f.height), y1 = std::clamp(int(std::ceil(f.pan_y + shown_h)), 0, f.height);
        const double r = reach(filter, inv_y) + 1;
        const int r0 = std::clamp(int(std::floor((y0 - f.pan_y) * inv_y - r)), 0, lh);
        const int r1 = std::clamp(int(std::ceil((y1 - f.pan_y) * inv_y + r)), 0, lh);
        const int cols = std::max(1, c1 - c0), rows = std::max(1, r1 - r0);
        // A swipe's two images each have their own (the GPU may still be
        // reading the first's when the second's is drawn into).
        rap::gpu::texture_ptr& across = rows_[&s == &shown_[1] ? 1 : 0];
        if (!across || across->width() < cols || across->height() < rows)
            across = device_->create({std::max(cols, across ? across->width() : 0), std::max(rows, across ? across->height() : 0),
                                      rap::gpu::format::rgba16f, 1, true});
        if (!across)
        {
            error = "no room for the first pass";
            return false;
        }
        first_params = {0, int32_t(filter), float(inv_x), float((c0 - f.pan_x) * inv_x), lw, r0, {}};
        if (n == 0 && s.detailed.active())
        {
            // Level 0 filtered over the columns and rows the passes read.
            const double rx = reach(filter, inv_x) + 1;
            const int qx0 = std::clamp(int(std::floor((c0 - f.pan_x) * inv_x - rx)), 0, lw);
            const int qx1 = std::clamp(int(std::ceil((c1 - f.pan_x) * inv_x + rx)), 0, lw);
            int ox = 0, oy = 0;
            src = detail_region(s, qx0, r0, std::max(qx1, qx0 + 1), std::max(r1, r0 + 1), ox, oy);
            first_params.offset -= float(ox);
            first_params.size = src->width();
            first_params.other = r0 - oy;
        }
        rap::gpu::pass first;
        first.program = resample_;
        first.inputs = {src};
        first.uniforms = bytes(first_params);
        first.target = across;
        first.region = {0, 0, cols, rows};
        passes.push_back(std::move(first));
        cp.filter = int32_t(filter);
        cp.inv = float(inv_y);
        cp.offset = float(-f.pan_y * inv_y - r0);  // into the rows, which start at level row r0
        cp.rows = rows;
        cp.column = c0;
        last.inputs = view_inputs(across, s, &f);
    }
    last.buffers = {s.stats, histogram_};
    last.uniforms = bytes(cp);
    passes.push_back(std::move(last));
    // IFRIT_GPU_SEPARATE_RUNS: each pass a run of its own, to tell a fault in
    // the passes from one between them (a barrier within a run).
    static const bool separate = std::getenv("IFRIT_GPU_SEPARATE_RUNS");
    if (separate && passes.size() > 1)
    {
        for (size_t i = 0; i + 1 < passes.size(); ++i)
            device_->run(std::span(&passes[i], 1));
        ready = device_->run(std::span(&passes.back(), 1));
        return true;
    }
    ready = device_->run(passes);
    return true;
}

bool GpuView::render(const std::shared_ptr<Image>& image, uint64_t generation, const Frame& f, rap::gpu::texture_ptr& out,
                     rap::gpu::ticket& ready, rap::frame_color& colour, std::string& error)
{
    if (!image || image->width <= 0 || image->height <= 0 || f.width <= 0 || f.height <= 0)
    {
        error = "nothing to draw";
        return false;
    }
    if (device_->lost())
    {
        error = "the GPU went away";
        return false;
    }
    if (!programs(error) || !hold(0, image, generation, error))
        return false;
    retain(shown_[1]);  // not comparing (or showing only one of them)
    if (!begin_frame(f, out, colour, error))
        return false;
    if (f.count_histogram)
        count(shown_[0], f);
    return draw(shown_[0], image->width, image->height, image->has_alpha, f, out, 0, f.width, ready, error);
}

bool GpuView::render_swipe(const std::shared_ptr<Image>& a, uint64_t a_generation, const Frame& fa, const std::shared_ptr<Image>& b,
                           uint64_t b_generation, const Frame& fb, int split, rap::gpu::texture_ptr& out, rap::gpu::ticket& ready,
                           rap::frame_color& colour, std::string& error)
{
    if (!a || !b || a->width <= 0 || a->height <= 0 || b->width <= 0 || b->height <= 0 || fa.width <= 0 || fa.height <= 0)
    {
        error = "nothing to draw";
        return false;
    }
    if (device_->lost())
    {
        error = "the GPU went away";
        return false;
    }
    if (!programs(error) || !hold(0, a, a_generation, error) || !hold(1, b, b_generation, error) || !begin_frame(fa, out, colour, error))
        return false;
    if (fa.count_histogram)
        count(shown_[0], fa);  // A's, as the view's own image
    // Each into its columns of the same frame (a side with none isn't drawn,
    // and the ticket is the last drawn's).
    return draw(shown_[0], a->width, a->height, a->has_alpha, fa, out, 0, split, ready, error) &&
           draw(shown_[1], b->width, b->height, b->has_alpha, fb, out, split, fa.width, ready, error);
}

bool GpuView::render_difference(const std::shared_ptr<Image>& a, uint64_t a_generation, const std::shared_ptr<const ImageDifference>& d,
                                int gain, const Frame& f, rap::gpu::texture_ptr& out, rap::gpu::ticket& ready, rap::frame_color& colour,
                                std::string& error)
{
    if (!a || !d || d->width != a->width || d->height != a->height || d->errors.empty() || f.width <= 0 || f.height <= 0)
    {
        error = "no difference to draw";
        return false;
    }
    if (device_->lost())
    {
        error = "the GPU went away";
        return false;
    }
    if (!programs(error) || !hold(0, a, a_generation, error) || !begin_frame(f, out, colour, error))
        return false;
    Source& s = shown_[0];
    if (f.count_histogram)
        count(s, f);  // A's: the difference has nothing to measure
    if (difference_of_ != d || !errors_)
    {
        errors_ = device_->create({d->width, d->height, rap::gpu::format::r8});
        if (!errors_)
        {
            error = "too big for the GPU";
            return false;
        }
        device_->upload(errors_, std::as_bytes(std::span(d->errors)), {1, rap::gpu::sample_type::u8});
        difference_of_ = d;
        difference_.levels.clear();
    }
    // Made at A's size when what it's made from changed, then shown as an
    // image (sRGB colours, nothing to tone).
    const bool same_wb = std::equal(std::begin(difference_wb_), std::end(difference_wb_), std::begin(f.white_balance));
    const uint64_t adjusted = f.adjust ? f.adjust->serial : 0;
    if (difference_.levels.empty() || difference_.image.lock() != a || difference_.generation != a_generation || difference_gain_ != gain ||
        difference_adjust_ != adjusted || difference_treated_ != f.treated || (s.tonable && (difference_exposure_ != f.exposure || !same_wb)))
    {
        ComposeParams cp{};
        show_params(f, s.scene_gain, s.tonable, bool(s.tone), s.auto_exposure, s.anchor, s.white_luminance, s.input_to_xyz, s.as_shot, s.dng.get(), s.measured, cp);
        measure(s, bytes(cp), false);
        measure_volume(s, f, bytes(cp));
        set_measured(cp, s.measured && s.needs_mapping(cp.adjusting != 0));
        cp.gain = std::clamp(gain, 1, 255);
        auto top = device_->create({a->width, a->height, rap::gpu::format::rgba16f, 1, true});
        if (!top)
        {
            error = "too big for the GPU";
            return false;
        }
        rap::gpu::pass p;
        p.program = difference_program_;
        p.inputs = view_inputs(s.levels[0], s, &f, errors_);
        p.buffers = {s.stats, histogram_};
        p.uniforms = bytes(cp);
        p.target = top;
        device_->run(std::span(&p, 1));
        difference_ = Source{};
        difference_.image = a;
        difference_.generation = a_generation;
        difference_.levels = {top};
        difference_.stats = s.stats;  // bound, not read: it isn't measured
        std::memcpy(difference_.input_to_xyz, kXyzFrom709, sizeof difference_.input_to_xyz);
        difference_gain_ = gain;
        difference_exposure_ = f.exposure;
        difference_adjust_ = adjusted;
        difference_treated_ = f.treated;  // a treated raw's exposure is the camera's (auto_gain())
        std::copy(std::begin(f.white_balance), std::end(f.white_balance), difference_wb_);
    }
    // Edited already, as A's colours went into it.
    Frame plain = f;
    plain.adjust.reset();
    return draw(difference_, a->width, a->height, false, plain, out, 0, f.width, ready, error);
}
