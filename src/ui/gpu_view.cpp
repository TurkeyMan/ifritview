#include "gpu_view.h"

#include "../compare.h"
#include "../image.h"
#include "../decoders/decoder.h"
#include "../decoders/ljpeg.h"
#include <glib.h>
#include "shaders/compose.h"
#include "shaders/decode.h"
#include "shaders/develop.h"
#include "shaders/develop_linear.h"
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
#include "shaders/peak.h"
#include "shaders/resample.h"
#include "shaders/stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    int32_t auto_exposure, gain, unused2[2];
};
static_assert(sizeof(ComposeParams) == 240, "as view.hlsli's cbuffer packs");

struct DevelopParams
{
    int32_t colour[4];
    float black[4];
    float as_shot[4];  // w: saturation
    int32_t map_x[4], map_y[4];
    int32_t size[4];
    int32_t filter[4];  // per photosite: LibRaw's colour (0 R, 1 G, 2 B, 3 the other G)
    int32_t ycc[4];     // YCC: block height, formula, hue, luma offset
    int32_t ycc_at[4];  // YCC: the first block's x, y
    float ycc_mul[4];   // YCC: sraw_mul; w: the guided fill's regularisation
};

// ljpeg.hlsli's block.
struct LjpegParams
{
    uint32_t total_bits, chunk_bits, chunks, components;
    uint32_t samples, row, rows, precision;
    uint32_t readout_w, readout_h, slice_n, slice_w;
    uint32_t slice_last, left, top, filters;
    int32_t rect[4];
    uint32_t mosaic_w, mosaic_h, scheme, curve_top;
    uint32_t initial[4];
};
static_assert(sizeof(LjpegParams) == 112, "ljpeg.hlsli's layout");

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

bool GpuView::export_pixels(const std::shared_ptr<Image>& image, uint64_t generation, double exposure, PixelBuffer& out,
                            std::string& error)
{
    // The view's own frame at 1:1, the whole image, as an SDR display shows it.
    Frame f;
    f.width = image->width;
    f.height = image->height;
    f.zoom = 1;
    f.exposure = exposure;
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
                                 std::pair{&develop_, shaders::develop}, std::pair{&develop_linear_, shaders::develop_linear},
                                 std::pair{&develop_ycc_, shaders::develop_ycc}, std::pair{&mosaic_, shaders::mosaic}})
    {
        auto r = device_->program(make());
        if (!r.program)
        {
            error = r.error;
            return false;
        }
        *program = r.program;
    }
    for (auto [program, make] : {std::pair{&stats_program_, shaders::stats}, std::pair{&peak_, shaders::peak},
                                 std::pair{&histogram_program_, shaders::histogram},
                                 std::pair{&histogram_max_, shaders::histogram_max}, std::pair{&ljpeg_guess_, shaders::ljpeg_guess},
                                 std::pair{&ljpeg_sync_, shaders::ljpeg_sync}, std::pair{&ljpeg_resolve_, shaders::ljpeg_resolve},
                                 std::pair{&ljpeg_decode_, shaders::ljpeg_decode}, std::pair{&ljpeg_columns_, shaders::ljpeg_columns},
                                 std::pair{&ljpeg_rows_, shaders::ljpeg_rows}, std::pair{&ljpeg_black_, shaders::ljpeg_black}})
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

bool GpuView::decode_stream(const RawMosaic& m, rap::gpu::texture_ptr& sites, rap::gpu::buffer_ptr& levels, std::string& error)
{
    const RawMosaic::Stream& st = *m.stream;
    const ljpeg::Prepared& j = *st.jpeg;
    static const bool keep = std::getenv("IFRIT_GPU_DECODE_ON_SOFTWARE") != nullptr;
    if (device_->software() && !keep)
    {
        // On the CPU's cores, then up as a photosite a texel: the active area
        // out of the readout (Canon's slices undone), and the masked areas
        // summed for the black level just as ljpeg_black does on the GPU.
        ljpeg::Image cpu;
        if (!ljpeg::decode(j, cpu, error))
            return false;
        const auto sample = [&](unsigned rx, unsigned ry) -> uint16_t
        {
            size_t i = size_t(ry) * st.readout_w + rx;
            if (st.slices[0])
            {
                const unsigned s = std::min(rx / st.slices[1], st.slices[0]), w = s < st.slices[0] ? st.slices[1] : st.slices[2];
                i = size_t(s) * st.slices[1] * st.readout_h + size_t(ry) * w + (rx - s * st.slices[1]);
            }
            return i < cpu.samples.size() ? cpu.samples[i] : 0;
        };
        std::vector<uint16_t> area(size_t(m.width) * m.height);
        for (int y = 0; y < m.height; ++y)
            for (int x = 0; x < m.width; ++x)
                area[size_t(y) * m.width + x] = sample(unsigned(x + st.left), unsigned(y + st.top));
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
        sites = device_->create({m.width, m.height, rap::gpu::format::r16});
        levels = device_->create_buffer(sizeof sums);
        if (!sites || !levels)
            return error = "too big for the GPU", false;
        device_->upload(sites, std::as_bytes(std::span(area)), {1, rap::gpu::sample_type::u16});
        device_->upload(levels, std::as_bytes(std::span(sums)));
        return true;
    }
    const uint64_t samples = uint64_t(j.width) * j.components * j.height;
    const uint32_t row = uint32_t(j.width * j.components);
    // ~1400 samples: the true decode mostly meets a guess within a few dozen
    // (where it doesn't, the sync goes on into the next chunks), and the
    // resolve walks the chunks one by one.
    constexpr uint32_t kChunkBits = 16384;
    const uint64_t chunks = std::max<uint64_t>(1, (j.total_bits + kChunkBits - 1) / kChunkBits);
    if (row % 2 || j.components > 4 || samples * 2 > (uint64_t(128) << 20) || j.total_bits > 0xFFFF0000u || j.height > 65535 ||
        chunks * j.components > 64 * 65535)
    {
        error = "a raw too big, or laid out in a way, the GPU doesn't decode";
        return false;
    }
    LjpegParams p{};
    p.total_bits = uint32_t(j.total_bits);
    p.chunk_bits = kChunkBits;
    p.chunks = uint32_t(chunks);
    p.components = uint32_t(j.components);
    p.samples = uint32_t(samples);
    p.row = row;
    p.rows = uint32_t(j.height);
    p.precision = uint32_t(j.precision);
    p.readout_w = uint32_t(st.readout_w), p.readout_h = uint32_t(st.readout_h);
    p.slice_n = st.slices[0], p.slice_w = st.slices[1], p.slice_last = st.slices[2];
    p.left = uint32_t(st.left), p.top = uint32_t(st.top);
    p.filters = st.filters;
    p.mosaic_w = uint32_t(m.width), p.mosaic_h = uint32_t(m.height);
    p.scheme = (j.nikon ? 1u : 0u) | uint32_t(j.step) << 8;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 4; c += 2)
            p.initial[(c >> 1) * 2 + r] = j.initial[r][c] | uint32_t(j.initial[r][c + 1]) << 16;

    // The stream (whole words: its tail padded), each component's table a
    // word an entry, and the passes' working buffers.
    const size_t whole = j.bits.size() & ~size_t(3);
    auto stream = device_->create_buffer(whole + 4);
    std::vector<uint32_t> lookup(size_t(j.components) * 65536);
    for (int c = 0; c < j.components; ++c)
        std::copy(j.lookup[c].begin(), j.lookup[c].end(), lookup.begin() + size_t(c) * 65536);
    auto table = device_->create_buffer(lookup.size() * 4);
    auto guesses = device_->create_buffer(chunks * j.components * 12);
    auto ways = device_->create_buffer(chunks * j.components * 16);
    auto plan = device_->create_buffer(16 + chunks * 16);
    auto values = device_->create_buffer(samples * 2);
    levels = device_->create_buffer(36);
    // A Nikon's curve, 16 bits an entry (an even count: whole words).
    std::vector<uint16_t> curve_words(j.curve.begin(), j.curve.end());
    if (curve_words.size() % 2)
        curve_words.push_back(curve_words.back());
    auto curve = device_->create_buffer(std::max<size_t>(4, curve_words.size() * 2));
    p.curve_top = j.curve.empty() ? 0 : uint32_t(j.curve.size() - 1);
    sites = device_->create({m.width, m.height, rap::gpu::format::r16, 1, true});
    if (!stream || !table || !guesses || !ways || !plan || !values || !levels || !sites || !curve)
    {
        error = "too big for the GPU";
        return false;
    }
    device_->upload(stream, std::as_bytes(std::span(j.bits.data(), whole)));
    uint8_t tail[4] = {};
    std::copy(j.bits.begin() + whole, j.bits.end(), tail);
    device_->upload(stream, std::as_bytes(std::span(tail)), whole);
    device_->upload(table, std::as_bytes(std::span(lookup)));
    if (!curve_words.empty())
        device_->upload(curve, std::as_bytes(std::span(curve_words)));
    device_->clear(values, 0);
    device_->clear(levels, 0);

    // The masked areas, inside the readout (as crop_masked_pixels clips them).
    std::vector<LjpegParams> blocks{p};
    for (const auto& r : st.mask)
    {
        const int top = std::max(r[0], 0), left = std::max(r[1], 0);
        const int bottom = std::min(r[2], st.readout_h), right = std::min(r[3], st.readout_w);
        if (bottom > top && right > left)
        {
            LjpegParams b = p;
            b.rect[0] = top, b.rect[1] = left, b.rect[2] = bottom, b.rect[3] = right;
            blocks.push_back(b);
        }
    }
    const auto dispatch = [&](rap::gpu::program_ptr program, std::vector<rap::gpu::buffer_ptr> buffers, uint64_t gx, uint64_t gy,
                              const LjpegParams& block)
    {
        rap::gpu::dispatch d;
        d.program = std::move(program);
        d.buffers = std::move(buffers);
        d.uniforms = bytes(block);
        d.groups[0] = int(gx);
        d.groups[1] = int(gy);
        return d;
    };
    const uint64_t ways_in = chunks * j.components;
    std::vector<rap::gpu::dispatch> work{
        dispatch(ljpeg_guess_, {stream, table, guesses}, (ways_in + 63) / 64, 1, blocks[0]),
        dispatch(ljpeg_sync_, {stream, table, guesses, ways}, (ways_in + 63) / 64, 1, blocks[0]),
        dispatch(ljpeg_resolve_, {guesses, ways, plan}, 1, 1, blocks[0]),
        dispatch(ljpeg_decode_, {stream, table, plan, values}, (chunks + 63) / 64, 1, blocks[0]),
        dispatch(ljpeg_columns_, {values}, 1, 1, blocks[0]),
        dispatch(ljpeg_rows_, {values, curve}, uint64_t(j.height), 1, blocks[0]),
    };
    for (size_t b = 1; b < blocks.size(); ++b)
        work.push_back(dispatch(ljpeg_black_, {values, levels}, (uint64_t(blocks[b].rect[3] - blocks[b].rect[1]) + 15) / 16,
                                (uint64_t(blocks[b].rect[2] - blocks[b].rect[0]) + 15) / 16, blocks[b]));
    device_->run(std::span(work));
    rap::gpu::pass pass;
    pass.program = mosaic_;
    pass.buffers = {values};
    pass.uniforms = bytes(blocks[0]);
    pass.target = sites;
    device_->run(std::span(&pass, 1));

    // A chunk whose true decode met none of its guesses (not expected, at
    // this chunk size) leaves the result wrong: said, and checked against
    // the CPU's decode on request (IFRIT_GPU_RAW_CHECK).
    static const bool check = std::getenv("IFRIT_GPU_RAW_CHECK") != nullptr;
    if (check || std::getenv("IFRIT_TRACE"))
    {
        auto head = std::make_shared<std::vector<std::byte>>(plan->size());
        device_->readback(plan, *head, [head]
                          {
                              uint32_t failed;
                              std::memcpy(&failed, head->data(), 4);
                              if (failed)
                                  std::fprintf(stderr, "gpu: a raw's stream didn't resolve on the GPU: its image is wrong\n");
                          });
    }
    if (check)
    {
        const gint64 t0 = g_get_monotonic_time();
        std::vector<std::byte> gpu(values->size()), sums(levels->size());
        device_->readback(values, gpu).wait();
        device_->readback(levels, sums).wait();
        const gint64 t1 = g_get_monotonic_time();
        ljpeg::Image cpu;
        std::string why;
        if (!ljpeg::decode(j, cpu, why))
            std::fprintf(stderr, "gpu raw check: the CPU couldn't decode it: %s\n", why.c_str());
        else
        {
            size_t wrong = 0, first = 0;
            for (size_t i = 0; i < cpu.samples.size(); ++i)
            {
                uint16_t v;
                std::memcpy(&v, gpu.data() + i * 2, 2);
                if (v != cpu.samples[i] && !wrong++)
                    first = i;
            }
            uint32_t w[9];
            std::memcpy(w, sums.data(), sizeof w);
            std::fprintf(stderr, "gpu raw check: %zu of %zu samples differ%s (read back in %.0f ms); black R %u G %u B %u (%u zero)\n", wrong,
                         cpu.samples.size(), wrong ? (", first at " + std::to_string(first)).c_str() : "", (t1 - t0) / 1000.0,
                         w[4] ? w[0] / w[4] : 0, w[5] ? w[1] / w[5] : 0, w[6] ? w[2] / w[6] : 0, w[8]);
        }
    }
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
        for (int i = 0; i < 4; ++i)
        {
            p.colour[i] = m->colour[i];
            p.filter[i] = m->filter[i];
            p.black[i] = m->black[i];
        }
        for (int c = 0; c < 3; ++c)
            p.as_shot[c] = m->as_shot[c];
        p.as_shot[3] = m->white;
        p.size[0] = m->width, p.size[1] = m->height, p.size[2] = levels ? 1 : 0;
        rap::gpu::buffer_ptr corrections;
        if (m->corrections && m->channels == 1 && !m->ycc.block_h &&
            (corrections = device_->create_buffer(m->corrections->size() * 4)))
        {
            device_->upload(corrections, std::as_bytes(std::span(*m->corrections)));
            p.size[3] = 1;
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
        pass.program = m->ycc.block_h ? develop_ycc_ : m->channels == 3 ? develop_linear_ : develop_;
        pass.inputs = {sites};
        pass.buffers = {levels ? levels : histogram_, corrections ? corrections : histogram_};  // each read only when there
        pass.uniforms = bytes(p);
        pass.target = top;
        device_->run(std::span(&pass, 1));
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
        d[i].inputs = {counted, s.tone ? s.tone : blank_};
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
    if (histogram_image_.lock() == image && histogram_generation_ == s.generation && histogram_exposure_ == f.exposure && !fresh)
        return;
    const rap::gpu::texture_ptr counted = counted_level(s);
    device_->clear(histogram_, 0);
    rap::gpu::dispatch d[2];
    d[0].program = histogram_program_;
    d[0].inputs = {counted, s.tone ? s.tone : blank_};
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
        last.inputs = {s.levels[0]};
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
        last.inputs = {across};
    }
    last.inputs.push_back(s.tone ? s.tone : blank_);
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
    if (difference_.levels.empty() || difference_.image.lock() != a || difference_.generation != a_generation || difference_gain_ != gain ||
        (s.tonable && (difference_exposure_ != f.exposure || !same_wb)))
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
        p.inputs = {s.levels[0], s.tone ? s.tone : blank_, errors_};
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
        std::copy(std::begin(f.white_balance), std::end(f.white_balance), difference_wb_);
    }
    return draw(difference_, a->width, a->height, false, f, out, 0, f.width, ready, error);
}
