/*
 * ifritview_plugin.h - the IfritView image format plugin interface.
 *
 * A plugin is a shared library (a .so on Linux, a .dll on Windows) that
 * teaches IfritView to read, and optionally write, one or more image
 * formats. It talks to IfritView through the plain C functions and structs
 * declared here, so it can be written in C, C++, Rust, Zig or anything else
 * that can export C functions, with any compiler, and it does not have to be
 * built with the same compiler or C runtime as IfritView.
 *
 * This header is the whole of the documentation. It is installed beside the
 * plugins themselves, and on Linux also as <ifritview/ifritview_plugin.h>.
 *
 *
 * WHERE PLUGINS GO
 *
 * IfritView looks in these folders, in this order, for files ending in .so
 * (Linux) or .dll (Windows). When two folders hold a plugin with the same
 * file name, the earlier one is used and the later one ignored.
 *
 *   1. Each folder in the environment variable IFRITVIEW_PLUGIN_PATH
 *      (separated by ':' on Linux, ';' on Windows). For development.
 *   2. Per user:
 *        Linux:   ~/.local/lib/ifritview/plugins
 *        Windows: %LOCALAPPDATA%\IfritView\plugins
 *   3. Beside the program, for a build tree or an unpacked archive:
 *        <folder of the ifritview executable>/plugins
 *      (on Windows this is the installed folder:
 *        C:\Program Files\IfritView\plugins)
 *   4. Linux, the installed system folder, where this header also lives:
 *        <prefix>/lib/<multiarch>/ifritview/plugins  (from a package: /usr/lib/...)
 *      and, for plugins built and installed locally,
 *        /usr/local/lib/ifritview/plugins
 *
 * Run `ifritview --plugins` to see the folders searched, the plugins found,
 * what each provides, and why any plugin was not loaded.
 *
 * Only the folder itself is searched, not subfolders, so a plugin that needs
 * libraries of its own can keep them in a subfolder. On Linux, link the
 * plugin with an rpath of '$ORIGIN/<subfolder>'; on Windows, IfritView loads
 * plugins with LOAD_WITH_ALTERED_SEARCH_PATH, so DLLs beside the plugin are
 * found.
 *
 *
 * WHAT A PLUGIN EXPORTS
 *
 * Required (a library without both is not loaded):
 *
 *   ifrit_plugin_info     describes the plugin and its formats
 *   ifrit_decode          reads an image from the bytes of a file
 *
 * Optional (looked up by name; a missing one means the feature isn't there):
 *
 *   ifrit_sniff           recognises a file by its first bytes, whatever its
 *                         name; without it a format is known by extension only
 *   ifrit_decode_preview  a quick stand-in shown while ifrit_decode runs (an
 *                         embedded thumbnail, a low-resolution pass)
 *   ifrit_encode          writes an image; needed for a format to appear in
 *                         Save As and `ifritview --convert`
 *
 * Export them with the IFRIT_EXPORT macro below, which also gives them C
 * linkage when the plugin is C++. They are found by these exact names.
 *
 *
 * LIFETIME
 *
 * Plugins are loaded once, on a background thread shortly after IfritView
 * starts, and stay loaded until the process exits; they are never unloaded.
 * ifrit_plugin_info is called once, right after loading, and nothing else is
 * called until it has returned. Do any one-time set-up there, or lazily.
 *
 * Built-in formats come first: when a plugin and IfritView both claim an
 * extension, IfritView's own decoder is tried first, and the plugin's only if
 * that fails. Among plugins, the one found first wins.
 *
 *
 * THREADING
 *
 * IfritView decodes several files at once (the one on screen, and the ones
 * either side of it in the folder) on worker threads. So ifrit_sniff,
 * ifrit_decode, ifrit_decode_preview and ifrit_encode may all be called at the
 * same time, from different threads, for different files, in any mix. They
 * must be re-entrant: keep per-call state on the stack or in memory you
 * allocate for that call, and guard anything shared (a cache, a library that
 * isn't thread-safe) with a lock of your own. The `host` structure a call
 * receives, and its callbacks, belong to that call and that thread only: use
 * them only until the call returns and only from the thread that made it.
 *
 *
 * MEMORY
 *
 * Nothing allocated by one side is ever freed by the other, so the plugin and
 * IfritView may use different allocators and C runtimes:
 *
 *   - The description ifrit_plugin_info returns (the formats, settings and
 *     their strings) stays the plugin's and must stay valid while the plugin
 *     is loaded: static data is simplest.
 *   - A decoder asks IfritView for the pixel buffer (host->pixels) and writes
 *     into it; IfritView owns it. Everything else a decoder reports (text,
 *     metadata) goes through callbacks that copy it, so pass temporaries.
 *   - An encoder receives pixels and metadata owned by IfritView, valid until
 *     it returns, and hands its output to host->write, which copies it.
 *
 *
 * ERRORS
 *
 * Functions return an ifrit_result. On IFRIT_ERROR, describe what went wrong
 * through host->error ("truncated pixel data", "unsupported compression 7");
 * IfritView shows it in place of the image. Return IFRIT_UNSUPPORTED when the
 * file simply isn't one you handle (a variant you don't read, or not your
 * format at all), and IfritView quietly tries other decoders. A plugin must
 * never crash on bad input: files come from anywhere, so check every size and
 * offset against `size` before reading.
 *
 *
 * VERSIONING
 *
 * IFRIT_PLUGIN_API_VERSION changes only when the interface changes in a way
 * that breaks existing plugins; IfritView refuses plugins built for a version
 * it doesn't know. Additions that don't break anything are made by adding
 * fields to the end of structs, which is what `struct_size` is for: each side
 * sets it to sizeof() the struct as it knows it, and reads only the fields
 * that fit. Fill in struct_size in every struct you hand to IfritView, and
 * before using a field added to a host struct in a later version, check the
 * host's struct_size covers it (IFRIT_HAS_FIELD). Zero every struct before
 * filling it in, so fields you don't set are 0 / NULL.
 *
 *
 * EXAMPLE
 *
 * A complete plugin for a made-up format: the magic "GREY", a little-endian
 * 32-bit width and height, then 8-bit grey pixels, row by row. A fuller
 * example, with sniffing and an encoder, is farbfeld.c in IfritView's source
 * (examples/plugins/farbfeld).
 *
 *   #include "ifritview_plugin.h"
 *   #include <string.h>
 *
 *   static const ifrit_format formats[] = {{
 *       .struct_size = sizeof(ifrit_format),
 *       .name = "Grey",
 *       .description = "Example 8-bit grey images",
 *       .extensions = "grey",
 *       .flags = IFRIT_FORMAT_DECODE,
 *   }};
 *   static const ifrit_plugin plugin = {
 *       .struct_size = sizeof(ifrit_plugin),
 *       .api_version = IFRIT_PLUGIN_API_VERSION,
 *       .name = "Grey example",
 *       .version = "1.0",
 *       .formats = formats,
 *       .format_count = 1,
 *   };
 *
 *   IFRIT_EXPORT const ifrit_plugin* IFRIT_CALL ifrit_plugin_info(uint32_t host_api_version)
 *   {
 *       (void)host_api_version;
 *       return &plugin;
 *   }
 *
 *   static uint32_t le32(const uint8_t* p)
 *   {
 *       return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
 *   }
 *
 *   IFRIT_EXPORT ifrit_result IFRIT_CALL ifrit_decode(uint32_t format, const uint8_t* data, size_t size,
 *                                                     const ifrit_decode_host* host)
 *   {
 *       (void)format;
 *       if (size < 12 || memcmp(data, "GREY", 4) != 0)
 *           return IFRIT_UNSUPPORTED;
 *       uint32_t w = le32(data + 4), h = le32(data + 8);
 *       if (w == 0 || h == 0 || w > 65535 || h > 65535 || (size - 12) / w < h)
 *       {
 *           host->error(host->context, "bad size or truncated");
 *           return IFRIT_ERROR;
 *       }
 *       size_t stride;
 *       uint8_t* px = (uint8_t*)host->pixels(host->context, (int32_t)w, (int32_t)h, IFRIT_L8, &stride);
 *       if (!px)
 *           return IFRIT_ERROR;  // out of memory; IfritView has said so
 *       for (uint32_t y = 0; y < h; ++y)
 *           memcpy(px + y * stride, data + 12 + (size_t)y * w, w);
 *       return IFRIT_OK;
 *   }
 *
 * Build it as a shared library and drop it into a plugins folder:
 *
 *   cc -shared -fPIC -O2 -fvisibility=hidden -o grey.so grey.c
 *   cl /LD /O2 grey.c /Fe:grey.dll
 */

#ifndef IFRITVIEW_PLUGIN_H
#define IFRITVIEW_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

/* The interface version this header describes. */
#define IFRIT_PLUGIN_API_VERSION 1

#ifdef __cplusplus
#define IFRIT_EXTERN_C extern "C"
#else
#define IFRIT_EXTERN_C
#endif

/* Exports a plugin function, with C linkage. */
#if defined(_WIN32) || defined(__CYGWIN__)
#define IFRIT_EXPORT IFRIT_EXTERN_C __declspec(dllexport)
#define IFRIT_CALL __cdecl
#elif defined(__GNUC__) || defined(__clang__)
#define IFRIT_EXPORT IFRIT_EXTERN_C __attribute__((visibility("default")))
#define IFRIT_CALL
#else
#define IFRIT_EXPORT IFRIT_EXTERN_C
#define IFRIT_CALL
#endif

/* Whether a struct received from the other side, which says how big it is
   in `struct_size`, is big enough to have `field`. */
#define IFRIT_HAS_FIELD(s, type, field) ((s)->struct_size >= offsetof(type, field) + sizeof(((type*)0)->field))

#ifdef __cplusplus
extern "C" {
#endif

/* What a plugin function returns. */
typedef int32_t ifrit_result;
#define IFRIT_OK 0          /* done */
#define IFRIT_ERROR 1       /* failed; the reason was passed to host->error */
#define IFRIT_UNSUPPORTED 2 /* not a file (or variant) this plugin handles; IfritView tries others */

/* -------------------------------------------------------------------------
 * Pixel layouts
 *
 * How pixels are laid out in memory, for decoding and encoding alike. Rows
 * run top to bottom, pixels left to right, channels in the order named.
 * Samples wider than a byte are in the machine's native byte order (a file's
 * big-endian samples must be swapped on x86). Alpha is straight (not
 * premultiplied); 0 is transparent.
 *
 *   8 and 16-bit integer samples are display-encoded, as image files
 *   usually are: 0 is black, the maximum (255, 65535) full intensity, and
 *   the values are treated as sRGB. IfritView keeps 16-bit samples as they
 *   are, beside the 8-bit rendering it draws, so nothing is lost on saving.
 *
 *   Float samples (32-bit and half) are linear light, 0 black and 1 the
 *   brightest the display shows; values above 1 (HDR) are kept, and the
 *   viewer gives such images an exposure control. Alpha is 0..1.
 *
 * L layouts are grey: one luminance value, for R, G and B alike.
 * ------------------------------------------------------------------------- */
typedef int32_t ifrit_layout;
#define IFRIT_RGBA8 1    /* uint8_t  R, G, B, A */
#define IFRIT_RGB8 2     /* uint8_t  R, G, B */
#define IFRIT_L8 3       /* uint8_t  L */
#define IFRIT_LA8 4      /* uint8_t  L, A */
#define IFRIT_RGBA16 5   /* uint16_t R, G, B, A */
#define IFRIT_RGB16 6    /* uint16_t R, G, B */
#define IFRIT_L16 7      /* uint16_t L */
#define IFRIT_LA16 8     /* uint16_t L, A */
#define IFRIT_RGBA32F 9  /* float    R, G, B, A (linear) */
#define IFRIT_RGB32F 10  /* float    R, G, B (linear) */
#define IFRIT_L32F 11    /* float    L (linear) */
#define IFRIT_RGBA16F 12 /* IEEE 754 half-precision R, G, B, A, each a uint16_t (linear) */
#define IFRIT_LAYOUT_COUNT 13

/* A set of layouts, as bits: IFRIT_LAYOUT_BIT(IFRIT_RGBA8) | ... */
#define IFRIT_LAYOUT_BIT(layout) (1u << (layout))

/* -------------------------------------------------------------------------
 * Metadata
 *
 * Blocks of metadata a decoder found in the file, and an encoder is asked to
 * write. Each is passed as the bytes it consists of, without any wrapping
 * the file format adds around it:
 * ------------------------------------------------------------------------- */
typedef int32_t ifrit_blob_kind;
#define IFRIT_BLOB_EXIF 1 /* a TIFF structure, from its byte-order mark ("II*\0" or "MM\0*"); a leading
                             "Exif\0\0" is tolerated from a decoder and removed */
#define IFRIT_BLOB_XMP 2  /* the XMP packet: XML text, UTF-8, no terminating zero needed */
#define IFRIT_BLOB_IPTC 3 /* IPTC-IIM datasets (starting 0x1C), as in Photoshop's resource 0x0404 */
#define IFRIT_BLOB_ICC 4  /* an ICC colour profile */

/* -------------------------------------------------------------------------
 * Describing a plugin
 * ------------------------------------------------------------------------- */

/* Kinds of save setting, as the Save dialog shows them. */
typedef int32_t ifrit_setting_kind;
#define IFRIT_SETTING_RANGE 1  /* a slider from `min` to `max` */
#define IFRIT_SETTING_TOGGLE 2 /* a check box: 0 or 1 */
#define IFRIT_SETTING_CHOICE 3 /* a list: an index into `choices` */

/* One setting an encoder offers. IfritView shows them in the Save dialog when
   the format is chosen, remembers them, and passes their values to
   ifrit_encode. On the command line they're `key=value`:
   `ifritview --convert in.png out.ext quality=80`.

   Two keys mean something to IfritView: an encoder with a "quality" setting
   is taken to lose detail (the Save dialog then offers a preview of the
   result) unless it also has a "lossless" toggle and that is on. */
typedef struct ifrit_setting
{
    uint32_t struct_size;    /* sizeof(ifrit_setting) */
    const char* key;         /* short identifier: lowercase letters, digits, '_' ("quality") */
    const char* label;       /* shown to the user ("Quality") */
    ifrit_setting_kind kind;
    int32_t min, max;        /* RANGE: the range, inclusive; TOGGLE: 0, 1; CHOICE: 0, number of choices - 1 */
    int32_t initial;         /* the value before the user changes it */
    const char* choices;     /* CHOICE: the entries, separated by '|' ("Fast|Normal|Best"); else NULL */
    const char* note;        /* optional: a few words shown beside the setting; NULL for none */
    const char* off_when;    /* optional: the key of a TOGGLE that, when on, makes this setting moot
                                (greyed out), as "quality" under "lossless"; NULL for none */
} ifrit_setting;

/* What a format can do (ifrit_format.flags). */
#define IFRIT_FORMAT_DECODE 0x1 /* ifrit_decode reads it */
#define IFRIT_FORMAT_ENCODE 0x2 /* ifrit_encode writes it (needs the export) */
#define IFRIT_FORMAT_METADATA 0x4 /* ifrit_encode can store metadata blocks: it receives them in
                                     ifrit_encode_image; without the flag it gets none */

/* One format a plugin provides. */
typedef struct ifrit_format
{
    uint32_t struct_size;      /* sizeof(ifrit_format) */
    const char* name;          /* short name, shown in the status bar, the info pane and Save As ("farbfeld") */
    const char* description;   /* optional: a line about it, for `ifritview --plugins`; may be NULL */
    const char* extensions;    /* file extensions, lowercase without the dot, separated by ';' ("ff;farbfeld").
                                  Files with these are offered to the plugin, and become part of the folder
                                  IfritView browses. The first is the one Save As writes. */
    uint32_t flags;            /* IFRIT_FORMAT_* */
    /* Encoding only (IFRIT_FORMAT_ENCODE): */
    uint32_t encode_layouts;   /* the layouts ifrit_encode accepts, as IFRIT_LAYOUT_BIT()s. IfritView picks
                                  the one closest to the image (16-bit for a 16-bit image, alpha when it has
                                  any) and converts. 0 means IFRIT_RGBA8 only. */
    const ifrit_setting* settings; /* the encoder's settings, in the order ifrit_encode receives them */
    uint32_t setting_count;
} ifrit_format;

/* The plugin as a whole, returned by ifrit_plugin_info. */
typedef struct ifrit_plugin
{
    uint32_t struct_size;          /* sizeof(ifrit_plugin) */
    uint32_t api_version;          /* IFRIT_PLUGIN_API_VERSION, as built */
    const char* name;              /* "Farbfeld" */
    const char* version;           /* the plugin's own version, free-form ("1.2.0") */
    const ifrit_format* formats;   /* an array of `format_count` formats. IfritView steps through it by each
                                      element's struct_size, so it may grow in later versions. */
    uint32_t format_count;         /* a format's index in this array is the `format` passed to the functions */
} ifrit_plugin;

/* -------------------------------------------------------------------------
 * Decoding
 * ------------------------------------------------------------------------- */

/* What a decoder may say about the image, besides its pixels (describe()). */
typedef int32_t ifrit_text_kind;
#define IFRIT_TEXT_FORMAT 1      /* the format as shown in the status bar, if more precise than the format's
                                    name ("farbfeld", "Foo, lossless, 12-bit") */
#define IFRIT_TEXT_INFO 2        /* extra detail for the status bar (a camera model, "frame 1 of 3") */
#define IFRIT_TEXT_COLOR_SPACE 3 /* the declared colour space ("Display P3"); when the file has an ICC
                                    profile, IfritView names it from the profile itself */

/* IfritView's side of one decode call: where the pixels go, and how to report
   everything else. Valid only during the call, on the calling thread. Every
   callback takes `context` as its first argument. */
typedef struct ifrit_decode_host
{
    uint32_t struct_size; /* sizeof(ifrit_decode_host) as IfritView knows it */
    void* context;        /* pass to every callback */

    /* Asks for the image buffer: `width` × `height` pixels of `layout`. Returns
       memory for the first row and sets *stride to the bytes from one row to
       the next, which may be more than width × pixel size: write each row at
       its stride. Returns NULL (having recorded the reason) when the size is
       unreasonable or memory runs out; return IFRIT_ERROR then.

       Call it once you know the size, and fill the whole buffer before
       returning IFRIT_OK. The contents start undefined. Calling it again
       replaces the earlier buffer (whose pointer is then invalid). Width and
       height are 1..65535 at most, and IfritView refuses images whose pixels
       would take more memory than it can find. */
    void* (IFRIT_CALL *pixels)(void* context, int32_t width, int32_t height, ifrit_layout layout, size_t* stride);

    /* Explains a failure; shown to the user. `message` is copied. Calling it
       more than once keeps the first message. */
    void (IFRIT_CALL *error)(void* context, const char* message);

    /* A line for the info pane: `value` under `key`, in a section called
       `group` (normally the format's name). Shown in the order given.
       All strings UTF-8, copied. */
    void (IFRIT_CALL *metadata)(void* context, const char* group, const char* key, const char* value);

    /* A metadata block from the file (EXIF, XMP, IPTC, an ICC profile), as
       bytes, copied. IfritView shows what it can of it, honours an EXIF
       orientation (don't turn the pixels yourself), and keeps it to write
       into a saved copy. XMP found as plain text in the file's bytes, and
       XMP sidecar files, are picked up without the plugin's help. */
    void (IFRIT_CALL *blob)(void* context, ifrit_blob_kind kind, const void* data, size_t size);

    /* Text about the image (IFRIT_TEXT_*), UTF-8, copied. */
    void (IFRIT_CALL *describe)(void* context, ifrit_text_kind kind, const char* text);
} ifrit_decode_host;

/* -------------------------------------------------------------------------
 * Encoding
 * ------------------------------------------------------------------------- */

/* The image to write. Owned by IfritView, valid until ifrit_encode returns. */
typedef struct ifrit_encode_image
{
    uint32_t struct_size;  /* sizeof(ifrit_encode_image) as IfritView knows it */
    int32_t width, height;
    ifrit_layout layout;   /* one of the format's encode_layouts */
    size_t stride;         /* bytes from one row to the next */
    const void* pixels;    /* the first row */
    int32_t has_alpha;     /* whether any pixel is less than opaque; when 0 alpha may be left out */
    /* Metadata to store, already made to fit the new file (orientation 1, the
       new size, only what the user chose to keep). Only for formats with
       IFRIT_FORMAT_METADATA; otherwise, and for blocks there are none of,
       NULL and 0. Formats are as described for ifrit_blob_kind. */
    const uint8_t* exif; size_t exif_size;
    const uint8_t* xmp;  size_t xmp_size;
    const uint8_t* iptc; size_t iptc_size;
    const uint8_t* icc;  size_t icc_size;
} ifrit_encode_image;

/* IfritView's side of one encode call. Valid only during the call, on the
   calling thread. */
typedef struct ifrit_encode_host
{
    uint32_t struct_size; /* sizeof(ifrit_encode_host) as IfritView knows it */
    void* context;        /* pass to every callback */

    /* Appends bytes to the file being written; call it as often as you like.
       The bytes are copied. Returns 0 if they couldn't be written (a full
       disk); return IFRIT_ERROR then. IfritView writes to a temporary file and
       puts it in place only when ifrit_encode returns IFRIT_OK, so a failed
       save never leaves half a file. */
    int32_t (IFRIT_CALL *write)(void* context, const void* data, size_t size);

    /* Explains a failure; as for decoding. */
    void (IFRIT_CALL *error)(void* context, const char* message);
} ifrit_encode_host;

/* -------------------------------------------------------------------------
 * The exports
 *
 * `format` is always the index of a format in ifrit_plugin.formats. Each
 * function is also given as a pointer type, for loaders written in C.
 * ------------------------------------------------------------------------- */

/* REQUIRED. Describes the plugin. Called once, after loading, before anything
   else. `host_api_version` is the IFRIT_PLUGIN_API_VERSION IfritView was built
   with; a plugin that needs a newer IfritView may return NULL to decline
   loading. The returned plugin, and everything it points to, must stay valid
   and unchanged while the library is loaded (static data is simplest). */
typedef const ifrit_plugin* (IFRIT_CALL *ifrit_plugin_info_fn)(uint32_t host_api_version);
#define IFRIT_PLUGIN_INFO_SYMBOL "ifrit_plugin_info"

/* REQUIRED. Decodes a whole file, given as `size` bytes at `data` (read-only;
   valid for the call; not zero-terminated). Gets the buffer from
   host->pixels, fills it, and returns IFRIT_OK; or returns IFRIT_ERROR with a
   reason, or IFRIT_UNSUPPORTED when the file isn't one it reads.

   A file with several images (pages, frames, mipmaps, faces) shows its first,
   or main, one. */
typedef ifrit_result (IFRIT_CALL *ifrit_decode_fn)(uint32_t format, const uint8_t* data, size_t size,
                                                   const ifrit_decode_host* host);
#define IFRIT_DECODE_SYMBOL "ifrit_decode"

/* OPTIONAL. Whether a file is in `format`, from its first `size` bytes:
   at least 64 of them, unless the file is shorter. Return nonzero to claim
   it. Lets IfritView open files whose extension is wrong or missing; files
   with one of the format's extensions are decoded without asking. Keep it
   cheap and specific (check a magic number); it's asked about files of every
   kind no built-in decoder recognised. */
typedef int32_t (IFRIT_CALL *ifrit_sniff_fn)(uint32_t format, const uint8_t* head, size_t size);
#define IFRIT_SNIFF_SYMBOL "ifrit_sniff"

/* OPTIONAL. A quick stand-in for a file whose full decode is slow: an
   embedded thumbnail or preview, a first low-resolution pass. Exactly like
   ifrit_decode (same host, same rules), but IfritView shows the result while
   ifrit_decode runs, then replaces it. Return IFRIT_UNSUPPORTED when this file
   has nothing quicker to offer. The stand-in may be smaller than the image;
   it is shown scaled to the image's size. */
typedef ifrit_result (IFRIT_CALL *ifrit_decode_preview_fn)(uint32_t format, const uint8_t* data, size_t size,
                                                           const ifrit_decode_host* host);
#define IFRIT_DECODE_PREVIEW_SYMBOL "ifrit_decode_preview"

/* OPTIONAL. Writes `image` in `format` through host->write. `settings` holds
   one value per entry in the format's settings, in the same order
   (`setting_count` of them; each within its setting's range). Called only for
   formats with IFRIT_FORMAT_ENCODE. */
typedef ifrit_result (IFRIT_CALL *ifrit_encode_fn)(uint32_t format, const ifrit_encode_image* image, const int32_t* settings,
                                                   uint32_t setting_count, const ifrit_encode_host* host);
#define IFRIT_ENCODE_SYMBOL "ifrit_encode"

#ifdef __cplusplus
}
#endif

#endif /* IFRITVIEW_PLUGIN_H */
