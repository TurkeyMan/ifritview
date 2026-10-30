#include "gpu_view.h"

#include <glib.h>
#include "../compare.h"
#include "../decoders/decoder.h"
#include "../decoders/ljpeg.h"
#include "../decoders/rawblocks.h"
#include "../decoders/rawpacked.h"
#include "../gpu.h"
#include "../image.h"
#include "../white_balance.h"
#include "shaders/compose.h"
#include "shaders/decode.h"
#include "shaders/develop.h"
#include "shaders/develop_linear.h"
#include "shaders/develop_correct.h"
#include "shaders/develop_green.h"
#include "shaders/develop_ycc.h"
#include "shaders/difference.h"
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
#include "shaders/peak.h"
#include "shaders/raw_blocks.h"
#include "shaders/resample.h"
#include "shaders/stats.h"
#include "shaders/unpack.h"
#include "shaders/warp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    int32_t auto_exposure, gain, adjusting, unused3;
    float to_working[3][4], from_working[3][4];
};
static_assert(sizeof(ComposeParams) == 336, "as view.hlsli's cbuffer packs");

struct DevelopParams
{
    uint32_t cfa[4];   // each site's LibRaw colour, 2 bits, 16 a word; w: the period
    float as_shot[4];  // w: saturation
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
        std::vector<std::byte> stats = std::vector<std::byte>(1101 * 4), histogram = std::vector<std::byte>(1032 * 4);
    };
    auto read = std::make_shared<Read>();
    const bool scene = histogram_scene_;
    const float exposure = histogram_scale_;
    const bool automatic = histogram_auto_;
    device_->readback(histogram_stats_, read->stats);
    device_->readback(histogram_, read->histogram,
                      [read, scene, exposure, automatic, done = std::move(done)]
                      {
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
                          const uint32_t count = word(read->stats, 1096);
                          if (count)
                          {
                              float scale = exposure;
                              if (automatic && as_float(word(read->stats, 1100)) > 0)
                                  scale /= as_float(word(read->stats, 1100));  // as auto_gain() has it
                              d.minimum = as_float(~word(read->stats, 1098)) * scale;
                              d.maximum = as_float(word(read->stats, 1099)) * scale;
                              d.peak = as_float(word(read->stats, 1097)) * scale;
                              // The mean from the log2 histogram's bin centres (to a couple of percent).
                              double sum = 0;
                              for (int bin = 0; bin < 1024; ++bin)
                                  sum += double(word(read->stats, size_t(bin))) * std::exp2((bin + 0.5) * 32 / 1024 - 20);
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
    for (auto [program, make] : {std::pair{&decode_, shaders::decode}, std::pair{&resample_, shaders::resample},
                                 std::pair{&compose_, shaders::compose}, std::pair{&difference_program_, shaders::difference},
                                 std::pair{&histogram_view_, shaders::histogram_view},
                                 std::pair{&develop_, shaders::develop}, std::pair{&develop_correct_, shaders::develop_correct},
                                 std::pair{&develop_green_, shaders::develop_green},
                                 std::pair{&develop_linear_, shaders::develop_linear},
                                 std::pair{&develop_ycc_, shaders::develop_ycc}, std::pair{&mosaic_, shaders::mosaic},
                                 std::pair{&mosaic_linear_, shaders::mosaic_linear},
                                 std::pair{&warp_, shaders::warp}})
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
         {std::pair{&stats_program_, shaders::stats}, std::pair{&peak_, shaders::peak}, std::pair{&histogram_program_, shaders::histogram},
          std::pair{&histogram_max_, shaders::histogram_max}, std::pair{&ljpeg_guess_, shaders::ljpeg_guess}, std::pair{&ljpeg_sync_, shaders::ljpeg_sync},
          std::pair{&ljpeg_resolve_, shaders::ljpeg_resolve}, std::pair{&ljpeg_decode_, shaders::ljpeg_decode},
          std::pair{&ljpeg_columns_, shaders::ljpeg_columns}, std::pair{&ljpeg_rows_, shaders::ljpeg_rows}, std::pair{&ljpeg_black_, shaders::ljpeg_black},
          std::pair{&raw_blocks_, shaders::raw_blocks}, std::pair{&unpack_, shaders::unpack}})
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
    uint32_t sums[9] = {};  // sum R G B G2, count R G B G2, zeros
    for (const auto& r : st.mask)
        for (int y = std::max(r[0], 0); y < std::min(r[2], st.readout_h); ++y)
            for (int x = std::max(r[1], 0); x < std::min(r[3], st.readout_w); ++x)
            {
                const unsigned c = (st.filters >> ((((y << 1) & 14) | (x & 1)) << 1)) & 3;
                const uint16_t v = sample(unsigned(x), unsigned(y));
                sums[c] += v;
                sums[4 + c] += 1;
                sums[8] += v == 0;
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
    levels = device_->create_buffer(36);
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
        device_->readback(plan, *head, [head, t_start, streams, chunks]
                          {
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
    s.auto_exposure = false;
    std::memcpy(s.input_to_xyz, kXyzFrom709, sizeof s.input_to_xyz);
    if (const RawMosaic* m = image.mosaic.get())
    {
        // A raw, developed here: the mosaic up as it is, then demosaiced into
        // level 0 (camera RGB, unbalanced).
        const gint64 t_start = g_get_monotonic_time();
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
        if (corrections && ((*m->corrections)[0] || (*m->corrections)[3]))
        {
            const bool whole = (*m->corrections)[3];
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
        pass.program = m->ycc.block_h ? develop_ycc_ : m->channels == 3 ? develop_linear_ : develop_;
        pass.inputs = {sites, green ? green : sites};  // the second read only for X-Trans
        pass.buffers = {levels ? levels : histogram_, corrections ? corrections : histogram_};  // each read only when there
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
        pass.uniforms = bytes(p);
        pass.target = warp ? whole : top;
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
        s.auto_exposure = true;
        s.measured = false;
        return true;
    }
    if (const LinearData* raw = image.linear.get(); raw && raw->rgb.size() == size_t(w) * h * 3)
    {
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
        s.stats = device_->create_buffer(1101 * 4);  // this image's measures
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
        const rap::gpu::texture_ptr& above = s.levels.back();
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

// The compose pass's parameters for showing `s` as `f` has it, but for
// the image's place and the resampling.
static void show_params(const GpuView::Frame& f, float scene_gain, bool tonable, bool tone, bool auto_exposure,
                        const float input_to_xyz[3][3], const float as_shot[3], bool measured, ComposeParams& cp)
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
    for (int j = 0; j < 3; ++j)
    {
        cp.input_x[j] = input_to_xyz[0][j] * wb[j];
        cp.input_y[j] = input_to_xyz[1][j] * wb[j];
        cp.input_z[j] = input_to_xyz[2][j] * wb[j];
    }
    cp.auto_exposure = auto_exposure ? 1 : 0;
    cp.measured = measured ? 1 : 0;
    if (const AdjustTables* t = f.adjust.get())
    {
        cp.adjusting = 1 | (t->channels ? 2 : 0);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                cp.to_working[i][j] = t->to_working[i][j], cp.from_working[i][j] = t->from_working[i][j];
        cp.to_working[0][3] = t->tone_top;
    }
    // The display: sRGB at SDR white until rapport describes the real one.
    std::memcpy(cp.display_r, kSrgbR, sizeof cp.display_r);
    std::memcpy(cp.display_g, kSrgbG, sizeof cp.display_g);
    std::memcpy(cp.display_b, kSrgbB, sizeof cp.display_b);
    cp.display_peak = 1;
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
    std::vector<rap::gpu::texture_ptr> in{image, s.tone ? s.tone : blank_, extra ? extra : blank_, blank_, blank_, blank_};
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
    if (s.measured || !(s.tonable || always))
        return;
    const rap::gpu::texture_ptr counted = counted_level(s);
    device_->clear(s.stats, 0);
    rap::gpu::dispatch d[2];
    for (int i = 0; i < 2; ++i)
    {
        d[i].program = i ? peak_ : stats_program_;
        d[i].inputs = view_inputs(counted, s, nullptr);
        d[i].buffers = {histogram_, s.stats};
        d[i].uniforms = params;  // copied at run()
        d[i].groups[0] = i ? 1 : (counted->width() + 15) / 16;
        d[i].groups[1] = i ? 1 : (counted->height() + 15) / 16;
    }
    device_->run(std::span(d));
    s.measured = true;
}

void GpuView::count(Source& s, const Frame& f)
{
    ComposeParams cp{};
    show_params(f, s.scene_gain, s.tonable, bool(s.tone), s.auto_exposure, s.input_to_xyz, s.as_shot, s.measured, cp);
    const bool fresh = !s.measured;
    measure(s, bytes(cp), true);
    cp.measured = 1;
    // The histogram of what's shown: again when exposure or the image change.
    const std::shared_ptr<Image> image = s.image.lock();
    const uint64_t adjusted = f.adjust ? f.adjust->serial : 0;
    if (histogram_image_.lock() == image && histogram_generation_ == s.generation && histogram_exposure_ == f.exposure &&
        histogram_adjust_ == adjusted && !fresh)
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
    histogram_scale_ = cp.exposure;
    histogram_scene_ = s.tonable;
    histogram_auto_ = s.auto_exposure;
    histogram_stats_ = s.stats;
}

bool GpuView::draw(Source& s, int width, int height, bool alpha, const Frame& f, const rap::gpu::texture_ptr& frame, int x0, int x1,
                   rap::gpu::ticket& ready, std::string& error)
{
    x0 = std::clamp(x0, 0, f.width), x1 = std::clamp(x1, 0, f.width);
    if (x1 <= x0)
        return true;  // `ready` left as it was
    ComposeParams cp{};
    show_params(f, s.scene_gain, s.tonable, bool(s.tone), s.auto_exposure, s.input_to_xyz, s.as_shot, s.measured, cp);
    measure(s, bytes(cp), false);
    cp.measured = s.measured ? 1 : 0;
    const double shown_w = width * f.zoom, shown_h = height * f.zoom;
    cp.image_rect[0] = float(f.pan_x), cp.image_rect[1] = float(f.pan_y);
    cp.image_rect[2] = float(f.pan_x + shown_w), cp.image_rect[3] = float(f.pan_y + shown_h);
    cp.checker_size = f.checkerboard && alpha ? std::max(1, f.checker_size) : 0;

    // A whole-pixel 1:1 is a copy; enlarging with no filter is nearest.
    const bool one_to_one = std::fabs(f.zoom - 1) < 1e-9 && f.pan_x == std::floor(f.pan_x) && f.pan_y == std::floor(f.pan_y);
    const ScaleFilter filter = f.zoom > 1 ? f.enlarge : f.shrink;
    std::vector<rap::gpu::pass> passes;
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
        last.inputs = view_inputs(s.levels[0], s, &f);
    }
    else
    {
        // The level at most twice the size it's shown at.
        int n = 0;
        while (f.zoom * double(1 << (n + 1)) <= 1.0 + 1e-9 && n < 30)
            ++n;
        const rap::gpu::texture_ptr src = level(s, n);
        const int lw = src->width(), lh = src->height();
        const double inv_x = lw / shown_w, inv_y = lh / shown_h;  // level texels per frame pixel

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
        const ResampleParams first_params{0, int32_t(filter), float(inv_x), float((c0 - f.pan_x) * inv_x), lw, r0, {}};
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
        difference_adjust_ != adjusted || (s.tonable && (difference_exposure_ != f.exposure || !same_wb)))
    {
        ComposeParams cp{};
        show_params(f, s.scene_gain, s.tonable, bool(s.tone), s.auto_exposure, s.input_to_xyz, s.as_shot, s.measured, cp);
        measure(s, bytes(cp), false);
        cp.measured = s.measured ? 1 : 0;
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
        std::copy(std::begin(f.white_balance), std::end(f.white_balance), difference_wb_);
    }
    // Edited already, as A's colours went into it.
    Frame plain = f;
    plain.adjust.reset();
    return draw(difference_, a->width, a->height, false, plain, out, 0, f.width, ready, error);
}
