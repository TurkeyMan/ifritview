/*
 * farbfeld.c - an IfritView format plugin for farbfeld images, as an example
 * of the plugin interface (include/ifritview_plugin.h) end to end: a format
 * that decodes, is recognised by its first bytes, and encodes, with one save
 * setting.
 *
 * farbfeld (tools.suckless.org/farbfeld) is as simple as an image format
 * gets:
 *
 *   bytes   contents
 *   8       "farbfeld"
 *   4       width, big-endian unsigned
 *   4       height, big-endian unsigned
 *   8 * w*h pixels, row by row: R, G, B, A, each a big-endian 16-bit sample,
 *           sRGB, alpha not premultiplied
 *
 * Plain C99 and nothing but the C library, so it builds with any compiler.
 * Every function here may run on several threads at once; none keeps state
 * between calls, so there's nothing to guard.
 */
#include "ifritview_plugin.h"

#include <stdlib.h>
#include <string.h>

/* -- the description ------------------------------------------------------ */

static const ifrit_setting settings[] = {
    /* A demonstration of a save setting: it shows in the Save dialog under
       the format, and as `opaque=1` on the command line. */
    {sizeof(ifrit_setting), "opaque", "Drop transparency", IFRIT_SETTING_TOGGLE, 0, 1, 0, NULL, "every pixel fully opaque", NULL},
};

static const ifrit_format formats[] = {
    {
        sizeof(ifrit_format),
        "farbfeld",
        "suckless.org's lossless 16-bit RGBA format",
        "ff;farbfeld",
        IFRIT_FORMAT_DECODE | IFRIT_FORMAT_ENCODE, /* no metadata: farbfeld has nowhere to put it */
        IFRIT_LAYOUT_BIT(IFRIT_RGBA16),            /* the only layout it stores; IfritView converts */
        settings,
        sizeof settings / sizeof settings[0],
    },
};

static const ifrit_plugin plugin = {
    sizeof(ifrit_plugin), IFRIT_PLUGIN_API_VERSION, "Farbfeld", "1.0", formats, sizeof formats / sizeof formats[0],
};

IFRIT_EXPORT const ifrit_plugin* IFRIT_CALL ifrit_plugin_info(uint32_t host_api_version)
{
    (void)host_api_version; /* version 1 is all this plugin needs */
    return &plugin;
}

/* -- reading --------------------------------------------------------------- */

static const char magic[8] = {'f', 'a', 'r', 'b', 'f', 'e', 'l', 'd'};
enum { header_size = 16 };

static uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A file is farbfeld if it starts with the magic: cheap and certain. */
IFRIT_EXPORT int32_t IFRIT_CALL ifrit_sniff(uint32_t format, const uint8_t* head, size_t size)
{
    (void)format;
    return size >= sizeof magic && memcmp(head, magic, sizeof magic) == 0;
}

IFRIT_EXPORT ifrit_result IFRIT_CALL ifrit_decode(uint32_t format, const uint8_t* data, size_t size, const ifrit_decode_host* host)
{
    (void)format;
    if (size < header_size || memcmp(data, magic, sizeof magic) != 0)
        return IFRIT_UNSUPPORTED; /* named .ff, but something else: let IfritView try the rest */
    const uint32_t width = be32(data + 8), height = be32(data + 12);
    if (width == 0 || height == 0 || width > 65535 || height > 65535)
    {
        host->error(host->context, "farbfeld image has an impossible size");
        return IFRIT_ERROR;
    }
    /* Divided rather than multiplied, so a huge size can't overflow. */
    if ((size - header_size) / 8 / width < height)
    {
        host->error(host->context, "farbfeld pixel data is cut short");
        return IFRIT_ERROR;
    }

    /* The samples go into IfritView's buffer as they are, but in the
       machine's byte order (IFRIT_RGBA16 is native-endian). */
    size_t stride = 0;
    uint8_t* pixels = (uint8_t*)host->pixels(host->context, (int32_t)width, (int32_t)height, IFRIT_RGBA16, &stride);
    if (!pixels)
        return IFRIT_ERROR; /* IfritView has recorded why */
    const uint8_t* src = data + header_size;
    for (uint32_t y = 0; y < height; ++y)
    {
        uint16_t* row = (uint16_t*)(pixels + (size_t)y * stride);
        for (size_t i = 0; i < (size_t)width * 4; ++i, src += 2)
            row[i] = (uint16_t)(src[0] << 8 | src[1]);
    }

    /* A line for the info pane. */
    host->metadata(host->context, "farbfeld", "Samples", "16-bit RGBA, big-endian, straight alpha");
    return IFRIT_OK;
}

/* -- writing --------------------------------------------------------------- */

static void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

IFRIT_EXPORT ifrit_result IFRIT_CALL ifrit_encode(uint32_t format, const ifrit_encode_image* image, const int32_t* values, uint32_t value_count,
                                                  const ifrit_encode_host* host)
{
    (void)format;
    /* encode_layouts names only RGBA16, so that's what arrives. */
    if (image->layout != IFRIT_RGBA16)
    {
        host->error(host->context, "farbfeld: unexpected pixel layout");
        return IFRIT_ERROR;
    }
    const int opaque = value_count > 0 && values[0] != 0;

    uint8_t header[header_size];
    memcpy(header, magic, sizeof magic);
    put_be32(header + 8, (uint32_t)image->width);
    put_be32(header + 12, (uint32_t)image->height);
    if (!host->write(host->context, header, sizeof header))
        return IFRIT_ERROR;

    /* A row at a time, swapped to big-endian. */
    const size_t row_bytes = (size_t)image->width * 8;
    uint8_t* out = (uint8_t*)malloc(row_bytes);
    if (!out)
    {
        host->error(host->context, "out of memory");
        return IFRIT_ERROR;
    }
    ifrit_result result = IFRIT_OK;
    for (int32_t y = 0; y < image->height && result == IFRIT_OK; ++y)
    {
        const uint16_t* row = (const uint16_t*)((const uint8_t*)image->pixels + (size_t)y * image->stride);
        for (size_t i = 0; i < (size_t)image->width * 4; ++i)
        {
            const uint16_t v = opaque && i % 4 == 3 ? 0xFFFF : row[i];
            out[i * 2] = (uint8_t)(v >> 8);
            out[i * 2 + 1] = (uint8_t)v;
        }
        if (!host->write(host->context, out, row_bytes))
            result = IFRIT_ERROR; /* IfritView knows: the disk is full */
    }
    free(out);
    return result;
}
