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

bool decode(const std::string& path, Image& out, std::string& error)
{
    std::vector<uint8_t> file;
    if (!read_file(path, file))
    {
        error = "cannot read file";
        return false;
    }
    std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> dec(JxlDecoderCreate(nullptr), JxlDecoderDestroy);
#ifdef HAVE_JXL_THREADS
    std::unique_ptr<void, decltype(&JxlResizableParallelRunnerDestroy)> runner(JxlResizableParallelRunnerCreate(nullptr), JxlResizableParallelRunnerDestroy);
    JxlDecoderSetParallelRunner(dec.get(), JxlResizableParallelRunner, runner.get());
#endif
    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS)
    {
        error = "JXL decoder setup failed";
        return false;
    }
    JxlDecoderSetInput(dec.get(), file.data(), file.size());
    JxlDecoderCloseInput(dec.get());

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
#ifdef HAVE_JXL_THREADS
            JxlResizableParallelRunnerSetThreads(runner.get(), JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
#endif
            out.allocate(int(info.xsize), int(info.ysize));
        }
        else if (status == JXL_DEC_COLOR_ENCODING)
        {
            JxlColorEncoding enc;
            if (JxlDecoderGetColorAsEncodedProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &enc) == JXL_DEC_SUCCESS)
            {
                const char* primaries = enc.primaries == JXL_PRIMARIES_SRGB   ? "BT.709"
                                        : enc.primaries == JXL_PRIMARIES_2100 ? "BT.2020"
                                        : enc.primaries == JXL_PRIMARIES_P3   ? "Display P3"
                                                                              : nullptr;
                const char* transfer = enc.transfer_function == JXL_TRANSFER_FUNCTION_SRGB     ? "sRGB"
                                       : enc.transfer_function == JXL_TRANSFER_FUNCTION_PQ     ? "PQ"
                                       : enc.transfer_function == JXL_TRANSFER_FUNCTION_HLG    ? "HLG"
                                       : enc.transfer_function == JXL_TRANSFER_FUNCTION_LINEAR ? "linear"
                                       : enc.transfer_function == JXL_TRANSFER_FUNCTION_709    ? "BT.709"
                                                                                               : nullptr;
                if (enc.color_space == JXL_COLOR_SPACE_GRAY)
                    out.color_space = std::string("Gray") + (transfer ? std::string(" ") + transfer : "");
                else if (primaries && transfer)
                    out.color_space =
                        (std::string(primaries) == "BT.709" && std::string(transfer) == "sRGB") ? "sRGB" : std::string(primaries) + " " + transfer;
                else if (primaries)
                    out.color_space = primaries;
            }
            else
            {
                size_t n = 0;
                if (JxlDecoderGetICCProfileSize(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, &n) == JXL_DEC_SUCCESS && n)
                {
                    std::vector<uint8_t> icc(n);
                    if (JxlDecoderGetColorAsICCProfile(dec.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, icc.data(), n) == JXL_DEC_SUCCESS)
                        out.color_space = icc_profile_description(icc.data(), n);
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
        PixelFormat::packed(info.num_color_channels == 1 ? PixelFormat::Model::Gray : PixelFormat::Model::RGB,
                            info.num_color_channels == 1 ? (info.alpha_bits ? "YA" : "Y") : (info.alpha_bits ? "RGBA" : "RGB"), int(info.bits_per_sample));
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

const Decoder& jxl_decoder()
{
    static const Decoder d{"JPEG XL", {"jxl"}, sniff, decode};
    return d;
}
