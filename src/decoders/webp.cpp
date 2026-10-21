#include "decoder.h"
#include "icc.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 12 && std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WEBP", 4) == 0;
}

bool read_file(const std::string& path, std::vector<uint8_t>& data)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data.resize(size > 0 ? size_t(size) : 0);
    const bool ok = data.empty() || std::fread(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    std::vector<uint8_t> file;
    if (!read_file(path, file))
    {
        error = "cannot read file";
        return false;
    }
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(file.data(), file.size(), &features) != VP8_STATUS_OK)
    {
        error = "not a valid WebP stream";
        return false;
    }

    // The animation decoder handles still images too and composes the first
    // frame onto the canvas for us; it also outputs premultiplied BGRA.
    WebPAnimDecoderOptions options;
    WebPAnimDecoderOptionsInit(&options);
    options.color_mode = MODE_bgrA;
    options.use_threads = 1;
    WebPData data{file.data(), file.size()};
    WebPAnimDecoder* dec = WebPAnimDecoderNew(&data, &options);
    if (!dec)
    {
        error = "WebP decode failed";
        return false;
    }
    WebPAnimInfo info;
    WebPAnimDecoderGetInfo(dec, &info);
    uint8_t* frame = nullptr;
    int timestamp = 0;
    if (!WebPAnimDecoderGetNext(dec, &frame, &timestamp))
    {
        WebPAnimDecoderDelete(dec);
        error = "WebP frame decode failed";
        return false;
    }
    out.allocate(int(info.canvas_width), int(info.canvas_height));
    out.from_bgra8(frame, out.width * 4, /*premultiplied=*/true);
    if (features.has_animation && info.frame_count > 1)
    {
        auto animation = std::make_shared<Animation>();
        animation->loops = int(info.loop_count);
        const size_t budget = size_t(512) << 20;
        size_t used = 0;
        int prev_ts = timestamp;
        animation->frames.push_back({out.pixels, 100});  // duration set once the next timestamp is known
        while (WebPAnimDecoderHasMoreFrames(dec) && WebPAnimDecoderGetNext(dec, &frame, &timestamp))
        {
            animation->frames.back().delay_ms = std::max(10, timestamp - prev_ts);
            prev_ts = timestamp;
            Image tmp;
            tmp.allocate(out.width, out.height);
            tmp.from_bgra8(frame, out.width * 4, true);
            used += tmp.pixels.size();
            animation->frames.push_back({std::move(tmp.pixels), 100});
            if (used > budget)
                break;
        }
        if (animation->frames.size() > 1)
            out.animation = animation;
    }
    WebPAnimDecoderDelete(dec);

    out.has_alpha = features.has_alpha;
    out.format = "WebP";
    if (features.format == 2)
    {
        out.pixel_format = PixelFormat::packed(PixelFormat::Model::RGB, features.has_alpha ? "ARGB" : "RGB", 8);
        out.pixel_format.notes.push_back("lossless");
    }
    else
    {
        out.pixel_format = PixelFormat::ycbcr(8, 2, 2);
        if (features.has_alpha)
            out.pixel_format.channels.push_back({'A', 8});
        out.pixel_format.notes.push_back("lossy");
    }
    out.add("WebP", "Coding", features.format == 2 ? "lossless" : features.format == 1 ? "lossy" : "mixed/undefined");
    if (features.has_animation)
        out.add("WebP", "Frames", std::to_string(info.frame_count));
    if (WebPDemuxer* demux = WebPDemux(&data))
    {
        WebPChunkIterator it;
        if (WebPDemuxGetChunk(demux, "ICCP", 1, &it))
        {
            out.color_space = icc_profile_description(it.chunk.bytes, it.chunk.size);
            WebPDemuxReleaseChunkIterator(&it);
        }
#ifdef HAVE_EXIF
        if (WebPDemuxGetChunk(demux, "EXIF", 1, &it))
        {
            const ExifSummary exif = read_exif_blob(it.chunk.bytes, it.chunk.size, out);
            out.info = exif.camera;
            if (out.color_space.empty())
                out.color_space = exif.color_space;
            WebPDemuxReleaseChunkIterator(&it);
        }
#endif
        WebPDemuxDelete(demux);
    }
    if (features.format == 2)
        out.format += ", lossless";
    if (features.has_animation)
        out.format += ", animated (" + std::to_string(info.frame_count) + " frames)";
    return true;
}

}  // namespace

const Decoder& webp_decoder()
{
    static const Decoder d{"WebP", {"webp"}, sniff, decode};
    return d;
}
