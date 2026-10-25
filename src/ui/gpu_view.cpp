#include "gpu_view.h"

#include "../image.h"
#include "shaders/compose.h"
#include "shaders/decode.h"
#include "shaders/histogram.h"
#include "shaders/histogram_max.h"
#include "shaders/histogram_view.h"
#include "shaders/peak.h"
#include "shaders/resample.h"
#include "shaders/stats.h"

#include <algorithm>
#include <cmath>
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
};
static_assert(sizeof(ComposeParams) == 176, "as view.hlsli's cbuffer packs");

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

GpuView::GpuView(std::shared_ptr<rap::gpu::device> device) : device_(std::move(device)) {}

void GpuView::read_details(std::function<void(const Details&)> done)
{
    if (!histogram_generation_ || !measured_)
        return;
    struct Read
    {
        std::vector<std::byte> stats = std::vector<std::byte>(1100 * 4), histogram = std::vector<std::byte>(1028 * 4);
    };
    auto read = std::make_shared<Read>();
    const bool scene = tonable_;
    const float exposure = histogram_scale_;
    device_->readback(stats_, read->stats);
    device_->readback(histogram_, read->histogram,
                      [read, scene, exposure, done = std::move(done)]
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
                              d.minimum = as_float(~word(read->stats, 1098)) * exposure;
                              d.maximum = as_float(word(read->stats, 1099)) * exposure;
                              d.peak = as_float(word(read->stats, 1097)) * exposure;
                              // The mean from the log2 histogram's bin centres (to a couple of percent).
                              double sum = 0;
                              for (int bin = 0; bin < 1024; ++bin)
                                  sum += double(word(read->stats, size_t(bin))) * std::exp2((bin + 0.5) * 32 / 1024 - 20);
                              d.average = float(sum / count) * exposure;
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
                                 std::pair{&compose_, shaders::compose}, std::pair{&histogram_view_, shaders::histogram_view}})
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
                                 std::pair{&histogram_max_, shaders::histogram_max}})
    {
        auto r = device_->compute_program(make());
        if (!r.program)
        {
            error = r.error;
            return false;
        }
        *program = r.program;
    }
    stats_ = device_->create_buffer(1100 * 4);
    histogram_ = device_->create_buffer(1028 * 4);
    blank_ = device_->create({1, 1, rap::gpu::format::r32f});
    if (!stats_ || !histogram_ || !blank_)
    {
        error = "no room for the measures";
        return false;
    }
    const float zero = 0;
    device_->upload(blank_, std::as_bytes(std::span(&zero, 1)), {1, rap::gpu::sample_type::f32});
    return true;
}

bool GpuView::upload(const Image& image, std::string& error)
{
    levels_.clear();
    tone_.reset();
    const int w = image.width, h = image.height;
    rap::gpu::texture_ptr data;
    DecodeParams dp{1, 3, 0, 0};
    scene_gain_ = 1;
    tonable_ = image.tonable();
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
            tone_ = device_->create({4096, 1, rap::gpu::format::r32f});
            if (tone_)
                device_->upload(tone_, std::as_bytes(std::span(curve)), {1, rap::gpu::sample_type::f32});
        }
        else
            scene_gain_ = float(raw->auto_gain);
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
    if (!data || !top || (image.linear && !image.linear->tone.empty() && !tone_))
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
    levels_.push_back(top);
    measured_ = false;
    histogram_generation_ = 0;
    return true;
}

rap::gpu::texture_ptr GpuView::counted_level()
{
    int n = 0;
    while (std::max(levels_[0]->width(), levels_[0]->height()) >> n > 2048)
        ++n;
    return level(n);
}

rap::gpu::texture_ptr GpuView::level(int n)
{
    while (int(levels_.size()) <= n)
    {
        const rap::gpu::texture_ptr& above = levels_.back();
        const int w = above->width(), h = above->height();
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        if (nw == w && nh == h)
            return levels_.back();
        // Halved with Lanczos-3, across then down, in linear light.
        auto across = device_->create({nw, h, rap::gpu::format::rgba16f, 1, true});
        auto next = device_->create({nw, nh, rap::gpu::format::rgba16f, 1, true});
        if (!across || !next)
            return levels_.back();
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
        levels_.push_back(next);
    }
    return levels_[size_t(n)];
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
    if (!programs(error))
        return false;
    if (image_.lock() != image || generation_ != generation || levels_.empty())
    {
        image_.reset();
        if (!upload(*image, error))
            return false;
        image_ = image;
        generation_ = generation;
    }

    // The front buffer.
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
    colour = {};
    colour.premultiplied = false;  // opaque: composited over the background
    colour.sdr_white_nits = f.sdr_white_nits;
    if (f.front == FrontBuffer::Pq10)
        colour.curve = rap::transfer::pq;  // BT.709 primaries until the gamut is mapped to the display's
    else if (f.front == FrontBuffer::ScRgb16)
        colour.curve = rap::transfer::linear;

    ComposeParams cp{};
    set_linear(cp.background, f.background);
    set_linear(cp.checker_a, rap::color{102, 102, 102});
    set_linear(cp.checker_b, rap::color{140, 140, 140});
    const double shown_w = image->width * f.zoom, shown_h = image->height * f.zoom;
    cp.image_rect[0] = float(f.pan_x), cp.image_rect[1] = float(f.pan_y);
    cp.image_rect[2] = float(f.pan_x + shown_w), cp.image_rect[3] = float(f.pan_y + shown_h);
    cp.checker_size = f.checkerboard && image->has_alpha ? std::max(1, f.checker_size) : 0;
    cp.exposure = scene_gain_ * float(tonable_ ? std::pow(2.0, f.exposure) : 1.0);
    cp.tone = tone_ ? 1 : 0;
    cp.output = int32_t(f.front);
    cp.white = f.sdr_white_nits;
    // The display: sRGB at SDR white until rapport describes the real one.
    std::memcpy(cp.display_r, kSrgbR, sizeof cp.display_r);
    std::memcpy(cp.display_g, kSrgbG, sizeof cp.display_g);
    std::memcpy(cp.display_b, kSrgbB, sizeof cp.display_b);
    cp.display_peak = 1;

    // Raw and HDR images measured once (their peak, their extent in each
    // hue), on the device: the mapping reads them there.
    std::vector<rap::gpu::dispatch> counts;
    const bool measure = !measured_ && (tonable_ || f.count_histogram);  // the panel shows them for any image
    const rap::gpu::texture_ptr counted = measure || f.count_histogram ? counted_level() : nullptr;
    const auto over = [&](rap::gpu::program_ptr program, std::vector<rap::gpu::buffer_ptr> buffers)
    {
        rap::gpu::dispatch d;
        d.program = std::move(program);
        d.inputs = {counted, tone_ ? tone_ : blank_};
        d.buffers = std::move(buffers);
        d.uniforms = bytes(cp);  // copied at run(); `cp` is final for what they read
        d.groups[0] = (counted->width() + 15) / 16;
        d.groups[1] = (counted->height() + 15) / 16;
        return d;
    };
    if (measure)
    {
        device_->clear(stats_, 0);
        counts.push_back(over(stats_program_, {histogram_, stats_}));
        rap::gpu::dispatch peak = over(peak_, {histogram_, stats_});
        peak.groups[0] = peak.groups[1] = 1;
        counts.push_back(std::move(peak));
        measured_ = true;
    }
    cp.measured = measured_ ? 1 : 0;
    // The histogram of what's shown: again when exposure or the image change.
    if (f.count_histogram && (histogram_generation_ != generation || histogram_exposure_ != f.exposure || counts.size()))
    {
        device_->clear(histogram_, 0);
        counts.push_back(over(histogram_program_, {histogram_, stats_}));
        rap::gpu::dispatch tallest = over(histogram_max_, {histogram_});
        tallest.inputs.clear();
        tallest.groups[0] = tallest.groups[1] = 1;
        counts.push_back(std::move(tallest));
        histogram_generation_ = generation;
        histogram_exposure_ = f.exposure;
        histogram_scale_ = cp.exposure;
    }
    if (f.histogram)
    {
        cp.show_histogram = 1;
        cp.histogram_channels = uint32_t(f.histogram_channels);
        cp.panel[0] = float(f.panel[0]), cp.panel[1] = float(f.panel[1]);
        cp.panel[2] = float(f.panel[0] + f.panel[2]), cp.panel[3] = float(f.panel[1] + f.panel[3]);
    }

    // A whole-pixel 1:1 is a copy; enlarging with no filter is nearest.
    const bool one_to_one = std::fabs(f.zoom - 1) < 1e-9 && f.pan_x == std::floor(f.pan_x) && f.pan_y == std::floor(f.pan_y);
    const ScaleFilter filter = f.zoom > 1 ? f.enlarge : f.shrink;
    std::vector<rap::gpu::pass> passes;
    rap::gpu::pass last;
    last.program = compose_;
    last.target = frame;
    if (one_to_one || filter == ScaleFilter::Nearest)
    {
        cp.nearest = 1;
        cp.inv = cp.inv_x = float(1 / f.zoom);
        cp.offset_x = float(-f.pan_x / f.zoom);
        cp.offset = float(-f.pan_y / f.zoom);
        cp.columns = image->width;
        cp.rows = image->height;
        last.inputs = {levels_[0]};
    }
    else
    {
        // The level at most twice the size it's shown at.
        int n = 0;
        while (f.zoom * double(1 << (n + 1)) <= 1.0 + 1e-9 && n < 30)
            ++n;
        const rap::gpu::texture_ptr src = level(n);
        const int lw = src->width(), lh = src->height();
        const double inv_x = lw / shown_w, inv_y = lh / shown_h;  // level texels per frame pixel

        // The frame columns the image covers, and the level rows the second
        // pass reads for the frame rows it covers.
        const int x0 = std::clamp(int(std::floor(f.pan_x)), 0, f.width), x1 = std::clamp(int(std::ceil(f.pan_x + shown_w)), 0, f.width);
        const int y0 = std::clamp(int(std::floor(f.pan_y)), 0, f.height), y1 = std::clamp(int(std::ceil(f.pan_y + shown_h)), 0, f.height);
        const double r = reach(filter, inv_y) + 1;
        const int r0 = std::clamp(int(std::floor((y0 - f.pan_y) * inv_y - r)), 0, lh);
        const int r1 = std::clamp(int(std::ceil((y1 - f.pan_y) * inv_y + r)), 0, lh);
        const int cols = std::max(1, x1 - x0), rows = std::max(1, r1 - r0);
        if (!rows_ || rows_->width() < cols || rows_->height() < rows)
            rows_ = device_->create({std::max(cols, rows_ ? rows_->width() : 0), std::max(rows, rows_ ? rows_->height() : 0),
                                     rap::gpu::format::rgba16f, 1, true});
        if (!rows_)
        {
            error = "no room for the first pass";
            return false;
        }
        const ResampleParams across{0, int32_t(filter), float(inv_x), float((x0 - f.pan_x) * inv_x), lw, r0, {}};
        rap::gpu::pass first;
        first.program = resample_;
        first.inputs = {src};
        first.uniforms = bytes(across);
        first.target = rows_;
        first.region = {0, 0, cols, rows};
        passes.push_back(std::move(first));
        cp.filter = int32_t(filter);
        cp.inv = float(inv_y);
        cp.offset = float(-f.pan_y * inv_y - r0);  // into the rows, which start at level row r0
        cp.rows = rows;
        cp.column = x0;
        last.inputs = {rows_};
    }
    last.inputs.push_back(tone_ ? tone_ : blank_);
    last.buffers = {stats_, histogram_};
    last.uniforms = bytes(cp);
    passes.push_back(std::move(last));
    if (!counts.empty())
        device_->run(counts);
    ready = device_->run(passes);
    out = frame;
    return true;
}
