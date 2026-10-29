#include "decoder.h"
#include "icc.h"

#include <jxl/decode.h>
#ifdef HAVE_JXL_THREADS
#include <jxl/resizable_parallel_runner.h>
#endif

#include <cstdio>
#include <cstring>
#include <memory>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    static const uint8_t container[12] = {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};
    return (n >= 2 && h[0] == 0xFF && h[1] == 0x0A) || (n >= 12 && std::memcmp(h, container, 12) == 0);
}

bool read_file(const std::string& path, std::vector<uint8_t>& data)
{
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (!file_bytes(path, bytes, size))
        return false;
    data.assign(bytes, bytes + size);
    return true;
}

// A decoder on `size` bytes at `data` (all there is of it), the events
// subscribed; on threads of its own unless `threaded` is false, as many as
// the image's size suggests once its basic info is in (threads_for).
struct Decoding
{
    std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> dec{JxlDecoderCreate(nullptr), JxlDecoderDestroy};
#ifdef HAVE_JXL_THREADS
    std::unique_ptr<void, decltype(&JxlResizableParallelRunnerDestroy)> runner{nullptr, JxlResizableParallelRunnerDestroy};
#endif
    bool start(const uint8_t* data, size_t size, int events, bool threaded = true)
    {
#ifdef HAVE_JXL_THREADS
        if (threaded)
        {
            runner.reset(JxlResizableParallelRunnerCreate(nullptr));
            JxlDecoderSetParallelRunner(dec.get(), JxlResizableParallelRunner, runner.get());
        }
#else
        (void)threaded;
#endif
        if (!dec || JxlDecoderSubscribeEvents(dec.get(), events) != JXL_DEC_SUCCESS)
            return false;
        JxlDecoderSetInput(dec.get(), data, size);
        JxlDecoderCloseInput(dec.get());
        return true;
    }
    void threads_for(const JxlBasicInfo& info)
    {
#ifdef HAVE_JXL_THREADS
        if (runner)
            JxlResizableParallelRunnerSetThreads(runner.get(), JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
#else
        (void)info;
#endif
    }
};

bool decode(const std::string& path, Image& out, std::string& error)
{
    std::vector<uint8_t> file;
    if (!read_file(path, file))
    {
        error = "cannot read file";
        return false;
    }
    Decoding decoding;
    const auto& dec = decoding.dec;
    if (!decoding.start(file.data(), file.size(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE))
    {
        error = "JXL decoder setup failed";
        return false;
    }

    const JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlBasicInfo info{};
    for (;;)
    {
        JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
        if (status == JXL_DEC_BASIC_INFO)
        {
            if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS)
            {
                error = "JXL: cannot read basic info";
                return false;
            }
            decoding.threads_for(info);
            out.allocate(int(info.xsize), int(info.ysize));
        }
        else if (status == JXL_DEC_COLOR_ENCODING)
        {
            JxlColorEncoding enc;
            if (JxlDecoderGetColorAsEncodedProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &enc) == JXL_DEC_SUCCESS)
            {
                // As H.273 code points, which JPEG XL's mostly are: P3 is 12
                // with a D65 white, 11 (DCI) with DCI's; gamma is the encoding
                // exponent, its inverse the curve.
                const bool gamma = enc.transfer_function == JXL_TRANSFER_FUNCTION_GAMMA && enc.gamma > 0;
                const int transfer = gamma ? 0 : int(enc.transfer_function);
                int primaries = int(enc.primaries);
                if (enc.primaries == JXL_PRIMARIES_P3)
                    primaries = enc.white_point == JXL_WHITE_POINT_DCI ? 11 : 12;
                CicpSpace cicp = from_cicp(primaries, transfer, gamma ? 1.0 / enc.gamma : 0);
                if (enc.primaries == JXL_PRIMARIES_CUSTOM && enc.color_space == JXL_COLOR_SPACE_RGB)
                {
                    // Its own primaries: with the curve the code points gave.
                    const CicpSpace curve = from_cicp(1, transfer, gamma ? 1.0 / enc.gamma : 0);
                    if (curve.space)
                    {
                        cicp.space = ColorSpace::from_primaries({enc.primaries_red_xy[0], enc.primaries_red_xy[1]}, {enc.primaries_green_xy[0], enc.primaries_green_xy[1]},
                                                                {enc.primaries_blue_xy[0], enc.primaries_blue_xy[1]}, {enc.white_point_xy[0], enc.white_point_xy[1]},
                                                                curve.space->transfer, curve.space->gamma, "JPEG XL primaries");
                        cicp.label = cicp.space->name;
                    }
                }
                if (enc.color_space == JXL_COLOR_SPACE_GRAY)
                    out.color_space = "Grey" + (cicp.label.empty() ? std::string() : ", " + cicp.label);
                else
                {
                    out.color_space = cicp.label;
                    out.declared_space = cicp.space;
                }
            }
            else
            {
                size_t n = 0;
                if (JxlDecoderGetICCProfileSize(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &n) == JXL_DEC_SUCCESS && n)
                {
                    std::vector<uint8_t> icc(n);
                    if (JxlDecoderGetColorAsICCProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, icc.data(), n) == JXL_DEC_SUCCESS)
                    {
                        out.color_space = icc_profile_description(icc.data(), n);
                        FileMetadata stored;  // the boxes' EXIF and XMP join it from the container pass
                        stored.icc = std::move(icc);
                        out.stored = std::make_shared<const FileMetadata>(std::move(stored));
                    }
                }
            }
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
        {
            size_t size = 0;
            JxlDecoderImageOutBufferSize(dec.get(), &format, &size);
            if (size != out.pixels.size())
            {
                error = "JXL: unexpected buffer size";
                return false;
            }
            JxlDecoderSetImageOutBuffer(dec.get(), &format, out.pixels.data(), size);
        }
        else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS)
        {
            break;
        }
        else
        {
            error = status == JXL_DEC_NEED_MORE_INPUT ? "JXL: truncated file" : "JXL decode error";
            return false;
        }
    }
    out.from_rgba8(out.pixels.data(), out.stride, info.alpha_premultiplied);
    out.has_alpha = info.alpha_bits > 0;
    out.bits_per_sample = int(info.bits_per_sample);
    out.format = "JPEG XL";
    out.pixel_format =
        PixelFormat::packed(info.num_color_channels == 1 ? PixelFormat::Model::L : PixelFormat::Model::RGB,
                            info.num_color_channels == 1 ? (info.alpha_bits ? "LA" : "L") : (info.alpha_bits ? "RGBA" : "RGB"), int(info.bits_per_sample));
    if (info.exponent_bits_per_sample)
        out.pixel_format.sample = PixelFormat::Sample::Float;
    if (info.alpha_premultiplied)
        out.pixel_format.premultiplied = true;
    if (info.uses_original_profile)
        out.pixel_format.notes.push_back("lossless");
    if (info.have_animation)
        out.format += ", animated";
    return true;
}

}  // namespace

bool decode_jxl_samples(const uint8_t* data, size_t size, std::vector<uint16_t>& samples, int& width, int& height, int& channels, int& bits,
                        std::string& error, bool threaded)
{
    Decoding decoding;
    const auto& dec = decoding.dec;
    if (!decoding.start(data, size, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE, threaded))
        return error = "JXL decoder setup failed", false;
    JxlPixelFormat format{1, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
    for (;;)
    {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
        if (status == JXL_DEC_BASIC_INFO)
        {
            JxlBasicInfo info{};
            if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS)
                return error = "JXL: cannot read basic info", false;
            if (info.num_extra_channels || uint64_t(info.xsize) * info.ysize * info.num_color_channels > (uint64_t(1) << 30))
                return error = "JXL: extra channels, or too large", false;
            decoding.threads_for(info);
            width = int(info.xsize), height = int(info.ysize), channels = int(info.num_color_channels);
            bits = info.exponent_bits_per_sample ? 0 : int(info.bits_per_sample);
            format.num_channels = info.num_color_channels;
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
        {
            size_t n = 0;
            JxlDecoderImageOutBufferSize(dec.get(), &format, &n);
            samples.resize(n / 2);
            JxlDecoderSetImageOutBuffer(dec.get(), &format, samples.data(), n);
        }
        else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS)
            return true;
        else
            return error = status == JXL_DEC_NEED_MORE_INPUT ? "JXL: truncated" : "JXL decode error", false;
    }
}

const Decoder& jxl_decoder()
{
    static const Decoder d{"JPEG XL", {"jxl"}, sniff, decode};
    return d;
}
