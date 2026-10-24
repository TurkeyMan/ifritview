#include "decoder.h"
#include "../i18n.h"
#include "icc.h"

#include <libheif/heif.h>

#include <algorithm>
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

bool decode_part(const std::string& path, ImagePart part, Image& out, std::string& error)
{
    heif_context* ctx = heif_context_alloc();
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (!file_bytes(path, bytes, size))
    {
        error = "cannot read file";
        heif_context_free(ctx);
        return false;
    }
    heif_error err = heif_context_read_from_memory_without_copy(ctx, bytes, size, nullptr);
    if (err.code != heif_error_Ok)
    {
        error = err.message;
        heif_context_free(ctx);
        return false;
    }
    // The top-level images are the pages: a burst, a collection, a camera's
    // bracketed set. The primary comes first, so opening the file shows it,
    // then the others in file order.
    std::vector<heif_item_id> pages(size_t(std::max(0, heif_context_get_number_of_top_level_images(ctx))));
    pages.resize(size_t(heif_context_get_list_of_top_level_image_IDs(ctx, pages.data(), int(pages.size()))));
    heif_item_id primary = 0;
    if (heif_context_get_primary_image_ID(ctx, &primary).code == heif_error_Ok)
        if (auto at = std::find(pages.begin(), pages.end(), primary); at != pages.end())
            std::rotate(pages.begin(), at, at + 1);
    out.page_count = std::max(1, int(pages.size()));
    if (part.page >= out.page_count)
    {
        error = "the HEIF has no such page";
        heif_context_free(ctx);
        return false;
    }
    if (pages.size() > 1 && part.page == 0 && pages[0] == primary)
        out.page_label = _("(primary)");
    heif_image_handle* handle = nullptr;
    err = pages.empty() ? heif_context_get_primary_image_handle(ctx, &handle) : heif_context_get_image_handle(ctx, pages[size_t(part.page)], &handle);
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

    if (size >= 12 && std::memcmp(bytes + 8, "avi", 3) == 0)
        out.format = "AVIF";
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
            out.pixel_format = PixelFormat::packed(PixelFormat::Model::L, alpha ? "LA" : "L", bits);
        else
        {
            out.pixel_format = PixelFormat::ycbcr(bits, ch == heif_chroma_444 ? 1 : 2, ch == heif_chroma_420 ? 2 : 1);
            if (alpha)
                out.pixel_format.channels.push_back({'A', bits});
        }
    }
    FileMetadata stored;  // as the file has it, for saving
    {
        const heif_color_profile_type type = heif_image_handle_get_color_profile_type(handle);
        if (type == heif_color_profile_type_nclx)
        {
            heif_color_profile_nclx* nclx = nullptr;
            if (heif_image_handle_get_nclx_color_profile(handle, &nclx).code == heif_error_Ok && nclx)
            {
                // H.273 code points, as JPEG XL's: a profile made for the space
                // travels with the image (PQ and HLG wait for HDR display).
                const CicpSpace cicp = from_cicp(int(nclx->color_primaries), int(nclx->transfer_characteristics));
                out.color_space = cicp.label;
                out.declared_space = cicp.space;
                heif_nclx_color_profile_free(nclx);
            }
        }
        else if (type == heif_color_profile_type_rICC || type == heif_color_profile_type_prof)
        {
            const size_t n = heif_image_handle_get_raw_color_profile_size(handle);
            std::vector<uint8_t> icc(n);
            if (n && heif_image_handle_get_raw_color_profile(handle, icc.data()).code == heif_error_Ok)
            {
                out.color_space = icc_profile_description(icc.data(), n);
                stored.icc = icc;
            }
        }
    }
    out.add(N_("HEIF"), N_("Bits per pixel"), std::to_string(out.bits_per_sample));
    out.add(N_("HEIF"), N_("Alpha channel"), alpha ? "yes" : "no");
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
            if (stored.exif.empty())
                stored.exif.assign(data.begin() + long(off), data.end());
#ifdef HAVE_EXIF
            const ExifSummary exif = read_exif_blob(data.data() + off, size - off, out);
            if (out.info.empty())
                out.info = exif.camera;
#endif
        }
    }
    // XMP: a "mime" block of type application/rdf+xml.
    const int m = heif_image_handle_get_list_of_metadata_block_IDs(handle, "mime", ids, 4);
    for (int i = 0; i < m && stored.xmp.empty(); ++i)
    {
        const char* type = heif_image_handle_get_metadata_content_type(handle, ids[i]);
        const size_t size = heif_image_handle_get_metadata_size(handle, ids[i]);
        if (!type || std::string(type) != "application/rdf+xml" || !size)
            continue;
        std::vector<uint8_t> data(size);
        if (heif_image_handle_get_metadata(handle, ids[i], data.data()).code == heif_error_Ok)
            stored.xmp = std::move(data);
    }
    if (!stored.empty())
        out.stored = std::make_shared<const FileMetadata>(std::move(stored));

    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return true;
}

}  // namespace

const Decoder& heif_decoder()
{
    static const Decoder d{"HEIF", {"heic", "heif", "hif", "avif"}, sniff, [](const std::string& path, Image& out, std::string& error)
                           { return decode_part(path, {}, out, error); }, nullptr, false, decode_part};
    return d;
}
