#include "gpu_view.h"

#include "../image.h"
#include "shaders/compose.h"
#include "shaders/decode.h"
#include "shaders/resample.h"

#include <algorithm>
#include <cmath>

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
    float unused[3];
};

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

bool GpuView::programs(std::string& error)
{
    if (compose_)
        return true;
    for (auto [program, make] : {std::pair{&decode_, shaders::decode}, std::pair{&resample_, shaders::resample},
                                 std::pair{&compose_, shaders::compose}})
    {
        auto r = device_->program(make());
        if (!r.program)
        {
            error = r.error;
            return false;
        }
        *program = r.program;
    }
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
    return true;
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
    if (tone_)
        last.inputs.push_back(tone_);
    last.uniforms = bytes(cp);
    passes.push_back(std::move(last));
    ready = device_->run(passes);
    out = frame;
    return true;
}
