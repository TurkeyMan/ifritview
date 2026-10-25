// The image's samples on rapport's GPU device (rap::gpu: Direct3D 11 on
// Windows, Vulkan elsewhere), through shaders/render.hlsl, which reproduces
// Image::retone_buffer()'s arithmetic step for step and writes
// unpremultiplied 8-bit values; the premultiply is done here as image.cpp's
// pack() does it, so the results match it exactly or within a step where
// float and double rounding part.
#include "gpu.h"

#include "image.h"
#include "shaders/render_float.h"
#include "shaders/render_linear.h"
#include "shaders/render_raw.h"
#include "shaders/render_raw_tone.h"
#include "shaders/render_signed.h"

#include <rapport/gpu.h>

#include <glib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace gpu
{
namespace
{

struct Device
{
    std::shared_ptr<rap::gpu::device> device;
    std::string renderer;
    std::map<std::string, rap::gpu::program_ptr> programs;  // by variant
    std::mutex mutex;                                       // programs
};

Device* device()
{
    static Device* d = []() -> Device*
    {
        if (g_getenv("IFRIT_NO_GPU"))
            return nullptr;
        auto dev = rap::gpu::device::headless();
        if (!dev)
        {
            if (g_getenv("IFRIT_TRACE"))
                std::fprintf(stderr, "gpu: no device\n");
            return nullptr;
        }
        auto* out = new Device;
        out->device = dev;
        out->renderer = std::string(dev->api()) + (dev->software() ? " (software)" : "");
        if (g_getenv("IFRIT_TRACE"))
            std::fprintf(stderr, "gpu: %s\n", out->renderer.c_str());
        return out;
    }();
    return d;
}

rap::gpu::program_ptr program(Device& d, const std::string& variant, rap::gpu::shader (*make)(), std::string& error)
{
    std::lock_guard<std::mutex> lock(d.mutex);
    if (auto it = d.programs.find(variant); it != d.programs.end())
        return it->second;
    auto r = d.device->program(make());
    if (!r.program)
    {
        error = r.error;
        return nullptr;
    }
    d.programs[variant] = r.program;
    return r.program;
}

// The shader's parameters, as its cbuffer has them.
struct Params
{
    int32_t channels;
    float exposure;
    float gain;
    float unused;
};

}  // namespace

bool available()
{
    return device() != nullptr;
}

std::string renderer()
{
    Device* d = device();
    return d ? d->renderer : std::string();
}

bool render(const Image& image, double ev, PixelBuffer& out, std::string& error)
{
    Device* d = device();
    if (!d)
    {
        error = "no GPU";
        return false;
    }
    rap::gpu::device& dev = *d->device;
    const LinearData* raw = image.linear.get();
    const SourceData* s = raw ? nullptr : image.source.get();
    if (!raw && !s)
    {
        error = "nothing beyond its 8-bit pixels";
        return false;
    }
    const int w = image.width, h = image.height;
    if (w <= 0 || h <= 0 || w > dev.max_texture_size() || h > dev.max_texture_size() ||
        (s && (s->width != w || s->height != h)) || (raw && raw->rgb.size() != size_t(w) * h * 3))
    {
        error = "too big for one texture, or its samples don't fit it";
        return false;
    }

    // The samples as they are: rapport expands them to four channels on the way.
    rap::gpu::texture_ptr source, tone;
    rap::gpu::program_ptr p;
    int channels = 3;
    if (raw)
    {
        source = dev.create({w, h, rap::gpu::format::rgba16});
        if (source)
            dev.upload(source, std::as_bytes(std::span(raw->rgb.data(), raw->rgb.size())), {3, rap::gpu::sample_type::u16});
        if (!raw->tone.empty())
        {
            std::vector<float> table(4096, raw->tone.back());
            std::copy_n(raw->tone.begin(), std::min<size_t>(4096, raw->tone.size()), table.begin());
            tone = dev.create({64, 64, rap::gpu::format::r32f});
            if (tone)
                dev.upload(tone, std::as_bytes(std::span(table)), {1, rap::gpu::sample_type::f32});
            p = program(*d, "raw_tone", shaders::render_raw_tone, error);
        }
        else
            p = program(*d, "raw", shaders::render_raw, error);
    }
    else
    {
        channels = s->channels;
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
        source = dev.create({w, h, format});
        if (source)
            dev.upload(source, std::as_bytes(std::span(s->data.data(), s->data.size())), {channels, type});
        if (s->linear)
            p = program(*d, "linear", shaders::render_linear, error);
        else if (s->sample == SourceData::Sample::I16)
            p = program(*d, "signed", shaders::render_signed, error);
        else
            p = program(*d, "float", shaders::render_float, error);
    }
    auto target = dev.create({w, h, rap::gpu::format::rgba8, 1, true});
    if (!source || !target || (raw && !raw->tone.empty() && !tone) || !p)
    {
        if (error.empty())
            error = "the GPU couldn't hold it";
        return false;
    }

    const double exposure = std::pow(2.0, ev);
    const Params params{channels, float(exposure), float(raw ? raw->auto_gain * exposure : exposure), 0};
    rap::gpu::pass pass;
    pass.program = p;
    pass.inputs = {source};
    if (tone)
        pass.inputs.push_back(tone);
    pass.uniforms = std::as_bytes(std::span(&params, 1));
    pass.target = target;
    dev.run(std::span(&pass, 1));
    std::vector<std::byte> rgba(size_t(w) * h * 4);
    dev.readback(target, rgba, size_t(w) * 4).wait();

    // Unpremultiplied RGBA bytes to image.cpp's premultiplied ARGB, as its pack() does.
    out.assign(size_t(w) * h * 4, 0);
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i)
    {
        uint32_t r = std::to_integer<uint32_t>(rgba[i * 4]), g = std::to_integer<uint32_t>(rgba[i * 4 + 1]),
                 b = std::to_integer<uint32_t>(rgba[i * 4 + 2]), a = std::to_integer<uint32_t>(rgba[i * 4 + 3]);
        if (a != 255)
        {
            r = (r * a + 127) / 255;
            g = (g * a + 127) / 255;
            b = (b * a + 127) / 255;
        }
        const uint32_t v = (a << 24) | (r << 16) | (g << 8) | b;
        std::memcpy(out.data() + i * 4, &v, 4);
    }
    return true;
}

}  // namespace gpu
