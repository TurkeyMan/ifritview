#include "decoder.h"
#include "icc.h"

#include <libheif/heif.h>

#include <cstring>
#include <vector>

namespace
{

bool sniff(const uint8_t* h, size_t n)
{
    if (n < 12 || std::memcmp(h + 4, "ftyp", 4) != 0)
        return false;
    static const char* brands[] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1", "avif", "avis", "heif"};
    for (const char* b : brands)
        if (std::memcmp(h + 8, b, 4) == 0)
            return true;
    return false;
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    heif_context* ctx = heif_context_alloc();
    heif_error err = heif_context_read_from_file(ctx, path.c_str(), nullptr);
    if (err.code != heif_error_Ok)
    {
        error = err.message;
        heif_context_free(ctx);
        return false;
    }
    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok)
    {
        error = err.message;
        heif_context_free(ctx);
        return false;
    }
    const bool alpha = heif_image_handle_has_alpha_channel(handle);
    heif_image* img = nullptr;
    err = heif_decode_image(handle, &img, heif_colorspace_RGB, alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, nullptr);
    if (err.code != heif_error_Ok)
    {
        error = err.message;
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return false;
    }
    int stride = 0;
    const uint8_t* plane = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
    out.allocate(heif_image_get_width(img, heif_channel_interleaved), heif_image_get_height(img, heif_channel_interleaved));
    if (alpha)
        out.from_rgba8(plane, stride, heif_image_handle_is_premultiplied_alpha(handle));
    else
        out.from_rgb8(plane, stride);
    out.has_alpha = alpha;
    out.bits_per_sample = heif_image_handle_get_luma_bits_per_pixel(handle);

    uint8_t head[12] = {};
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        if (std::fread(head, 1, 12, f) == 12 && std::memcmp(head + 8, "avi", 3) == 0)
            out.format = "AVIF";
        std::fclose(f);
    }
    if (out.format.empty())
        out.format = "HEIF";
    if (heif_image_handle_get_number_of_depth_images(handle) > 0)
        out.info = "has depth map";
    {
        heif_colorspace cs = heif_colorspace_undefined;
        heif_chroma ch = heif_chroma_undefined;
        heif_image_handle_get_preferred_decoding_colorspace(handle, &cs, &ch);
        const int bits = out.bits_per_sample;
        if (cs == heif_colorspace_RGB)
            out.pixel_format = PixelFormat::packed(PixelFormat::Model::RGB, alpha ? "RGBA" : "RGB", bits);
        else if (cs == heif_colorspace_monochrome || ch == heif_chroma_monochrome)
            out.pixel_format = PixelFormat::packed(PixelFormat::Model::Gray, alpha ? "YA" : "Y", bits);
        else
        {
            out.pixel_format = PixelFormat::ycbcr(bits, ch == heif_chroma_444 ? 1 : 2, ch == heif_chroma_420 ? 2 : 1);
            if (alpha)
                out.pixel_format.channels.push_back({'A', bits});
        }
    }
    {
        const heif_color_profile_type type = heif_image_handle_get_color_profile_type(handle);
        if (type == heif_color_profile_type_nclx)
        {
            heif_color_profile_nclx* nclx = nullptr;
            if (heif_image_handle_get_nclx_color_profile(handle, &nclx).code == heif_error_Ok && nclx)
            {
                const char* primaries = nclx->color_primaries == heif_color_primaries_ITU_R_BT_709_5               ? "BT.709"
                                        : nclx->color_primaries == heif_color_primaries_ITU_R_BT_2020_2_and_2100_0 ? "BT.2020"
                                        : nclx->color_primaries == heif_color_primaries_SMPTE_EG_432_1             ? "Display P3"
                                        : nclx->color_primaries == heif_color_primaries_SMPTE_RP_431_2             ? "DCI-P3"
                                                                                                                   : nullptr;
                const char* transfer = nclx->transfer_characteristics == heif_transfer_characteristic_IEC_61966_2_1         ? "sRGB"
                                       : nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_PQ  ? "PQ"
                                       : nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_HLG ? "HLG"
                                       : nclx->transfer_characteristics == heif_transfer_characteristic_linear              ? "linear"
                                       : nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_709_5      ? "BT.709"
                                                                                                                            : nullptr;
                if (primaries && transfer)
                {
                    if (std::string(primaries) == "BT.709" && std::string(transfer) == "sRGB")
                        out.color_space = "sRGB";
                    else
                        out.color_space = std::string(primaries) + " " + transfer;
                }
                else if (primaries)
                    out.color_space = primaries;
                heif_nclx_color_profile_free(nclx);
            }
        }
        else if (type == heif_color_profile_type_rICC || type == heif_color_profile_type_prof)
        {
            const size_t n = heif_image_handle_get_raw_color_profile_size(handle);
            std::vector<uint8_t> icc(n);
            if (n && heif_image_handle_get_raw_color_profile(handle, icc.data()).code == heif_error_Ok)
                out.color_space = icc_profile_description(icc.data(), n);
        }
    }
    out.add("HEIF", "Bits per pixel", std::to_string(out.bits_per_sample));
    out.add("HEIF", "Alpha channel", alpha ? "yes" : "no");
#ifdef HAVE_EXIF
    heif_item_id ids[4];
    const int n = heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", ids, 4);
    for (int i = 0; i < n; ++i)
    {
        const size_t size = heif_image_handle_get_metadata_size(handle, ids[i]);
        if (size < 8)
            continue;
        std::vector<uint8_t> data(size);
        if (heif_image_handle_get_metadata(handle, ids[i], data.data()).code != heif_error_Ok)
            continue;
        // Payload starts with a big-endian offset to the TIFF header.
        const size_t off = 4 + ((size_t(data[0]) << 24) | (size_t(data[1]) << 16) | (size_t(data[2]) << 8) | data[3]);
        if (off < size)
        {
            const ExifSummary exif = read_exif_blob(data.data() + off, size - off, out);
            if (out.info.empty())
                out.info = exif.camera;
        }
    }
#endif

    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return true;
}

}  // namespace

const Decoder& heif_decoder()
{
    static const Decoder d{"HEIF", {"heic", "heif", "hif", "avif"}, sniff, decode};
    return d;
}
