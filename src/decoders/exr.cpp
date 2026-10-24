// OpenEXR: ILM's scene-linear high-dynamic-range format, written from the
// specification. A version field, one header of attributes per part, a table
// of chunk offsets per part, then the chunks: blocks of scanlines or tiles,
// each compressed on its own. Single and multi-part files (the first image
// part with colour is shown), scanlines and tiles (level 0 of a mipmap or
// ripmap), pixel types half, float and uint, and the compressions none, RLE,
// ZIPS, ZIP, PIZ, PXR24, B44 and B44A. DWAA/DWAB, HTJ2K, LJ2K and ZSTD
// aren't read, nor are deep parts or luminance/chroma (Y, RY, BY) images.
// Chunks decode in parallel.
//
// The samples are kept as the image's source in their own type (half or
// float; uint becomes float), un-premultiplied, and the 8-bit pixels are
// rendered from there with an exposure when they go past white. R, G, B and
// A are shown, unprefixed by preference, else from the first layer that has
// them ("diffuse.R"); Y alone is grey. Other channels are listed only.
#include "decoder.h"
#include "../half.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

uint16_t le16(const uint8_t* p)
{
    return uint16_t(p[0] | p[1] << 8);
}

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

int32_t le32s(const uint8_t* p)
{
    return int32_t(le32(p));
}

uint64_t le64(const uint8_t* p)
{
    return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32;
}

float lef(const uint8_t* p)
{
    const uint32_t v = le32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

constexpr uint32_t kMagic = 20000630;  // 76 2f 31 01

bool sniff(const uint8_t* h, size_t n)
{
    return n >= 4 && le32(h) == kMagic;
}

// Version field flags.
constexpr uint32_t kTiledFlag = 0x200, kDeepFlag = 0x800, kMultipartFlag = 0x1000;

enum Compression
{
    kNone = 0,
    kRle,
    kZips,
    kZip,
    kPiz,
    kPxr24,
    kB44,
    kB44a,
};

const char* compression_name(int c)
{
    static const char* const names[] = {"none", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24", "B44", "B44A", "DWAA", "DWAB", "HTJ2K256", "HTJ2K32", "LJ2K", "ZSTD"};
    return c >= 0 && c < int(std::size(names)) ? names[c] : nullptr;
}

// Scanlines in one chunk.
int lines_per_chunk(int c)
{
    switch (c)
    {
        case kZip:
        case kPxr24: return 16;
        case kPiz:
        case kB44:
        case kB44a: return 32;
        default: return 1;
    }
}

enum PixelType
{
    kUint = 0,
    kHalf = 1,
    kFloat = 2
};

const char* type_name(int t)
{
    return t == kHalf ? "half" : t == kFloat ? "float" : "uint";
}

int type_size(int t)
{
    return t == kHalf ? 2 : 4;
}

struct Channel
{
    std::string name;
    int type;
    bool linear;  // pLinear: B44 stores it on a log scale
    int xs, ys;   // sampling
};

// Division and remainder rounding towards minus infinity, as the format
// does for coordinates that may be negative.
int divp(int x, int y)
{
    return x >= 0 ? x / y : -((y - 1 - x) / y);
}

int modp(int x, int y)
{
    return x - y * divp(x, y);
}

// Samples of a channel with sampling s in [a, b].
int num_samples(int s, int a, int b)
{
    const int a1 = divp(a, s), b1 = divp(b, s);
    return b1 - a1 + (a1 * s < a ? 0 : 1);
}

struct Box
{
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // inclusive
    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
    bool operator==(const Box& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};

struct Part
{
    std::string name, type;
    std::vector<Channel> channels;
    int compression = -1;
    bool has_data_window = false, has_display_window = false;
    Box data_window, display_window;
    int line_order = 0;
    bool tiled = false;
    uint32_t tile_w = 0, tile_h = 0;
    int level_mode = 0;
    int64_t chunk_count = -1;
    bool has_chromaticities = false;
    float chromaticities[8] = {};
    std::vector<MetadataItem> attributes;  // the others worth showing
};

std::string fmt(const char* f, double a)
{
    char s[64];
    std::snprintf(s, sizeof s, f, a);
    return s;
}

std::string box_text(const Box& b)
{
    return "(" + std::to_string(b.x0) + ", " + std::to_string(b.y0) + ") – (" + std::to_string(b.x1) + ", " + std::to_string(b.y1) + "), " +
           std::to_string(int64_t(b.x1) - b.x0 + 1) + "×" + std::to_string(int64_t(b.y1) - b.y0 + 1);
}

// Text from the file as UTF-8: as it is when it's valid, else read as
// Latin-1 (older writers), control characters dropped either way.
std::string text_of(const uint8_t* p, size_t n)
{
    bool valid = true;
    for (size_t i = 0; i < n && valid;)
    {
        const uint8_t c = p[i];
        const size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 && c >= 0xC2 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 && c <= 0xF4 ? 4 : 0;
        valid = len && len <= n - i;
        for (size_t k = 1; valid && k < len; ++k)
            valid = (p[i + k] & 0xC0) == 0x80;
        i += len ? len : 1;
    }
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const uint8_t c = p[i];
        if (c < 0x20 && c != '\n' && c != '\t')
            continue;
        if (valid || c < 0x80)
            s += char(c);
        else
        {
            s += char(0xC0 | c >> 6);
            s += char(0x80 | (c & 0x3F));
        }
    }
    return s;
}

// A null-terminated name in [p, end), at most 255 characters.
bool read_name(const uint8_t*& p, const uint8_t* end, std::string& s)
{
    const uint8_t* z = static_cast<const uint8_t*>(std::memchr(p, 0, size_t(std::min<ptrdiff_t>(end - p, 256))));
    if (!z)
        return false;
    s = text_of(p, size_t(z - p));
    p = z + 1;
    return true;
}

bool parse_channels(const uint8_t* p, const uint8_t* end, std::vector<Channel>& out)
{
    while (p < end && *p)
    {
        Channel c;
        if (!read_name(p, end, c.name) || end - p < 16)
            return false;
        c.type = le32s(p);
        c.linear = p[4] != 0;
        c.xs = le32s(p + 8);
        c.ys = le32s(p + 12);
        p += 16;
        if (c.type < 0 || c.type > 2 || c.xs < 1 || c.ys < 1 || c.xs > 1 << 16 || c.ys > 1 << 16 || out.size() >= 4096)
            return false;
        out.push_back(std::move(c));
    }
    return p < end && !out.empty();
}

// Attributes shown under their own names, friendlier than the file's.
const char* friendly_name(const std::string& name)
{
    static const struct
    {
        const char* attr;
        const char* label;
    } names[] = {
        {"owner", "Owner"},
        {"comments", "Comments"},
        {"capDate", "Capture date"},
        {"software", "Software"},
        {"whiteLuminance", "White luminance"},
        {"utcOffset", "UTC offset (s)"},
        {"framesPerSecond", "Frames per second"},
        {"multiView", "Views"},
        {"view", "View"},
        {"expTime", "Exposure time"},
        {"aperture", "Aperture"},
        {"isoSpeed", "ISO"},
        {"cameraMake", "Camera make"},
        {"cameraModel", "Camera model"},
        {"lensModel", "Lens"},
        {"envmap", "Environment map"},
        {"adoptedNeutral", "Adopted neutral"},
        {"pixelAspectRatio", "Pixel aspect ratio"},
    };
    for (const auto& n : names)
        if (name == n.attr)
            return n.label;
    return nullptr;
}

// One attribute's value as text, for the types worth showing.
bool attribute_text(const std::string& name, const std::string& type, const uint8_t* v, size_t n, std::string& text)
{
    if (type == "string")
        text = text_of(v, n);
    else if (type == "stringvector")
    {
        size_t i = 0;
        while (i + 4 <= n)
        {
            const uint32_t len = le32(v + i);
            i += 4;
            if (len > n - i)
                break;
            text += (text.empty() ? "" : ", ") + text_of(v + i, len);
            i += len;
        }
    }
    else if (type == "float" && n == 4)
        text = fmt("%g", double(lef(v)));
    else if (type == "double" && n == 8)
    {
        const uint64_t b = le64(v);
        double d;
        std::memcpy(&d, &b, 8);
        text = fmt("%g", d);
    }
    else if (type == "int" && n == 4)
        text = std::to_string(le32s(v));
    else if (type == "v2f" && n == 8)
        text = fmt("%g", double(lef(v))) + ", " + fmt("%g", double(lef(v + 4)));
    else if (type == "v2i" && n == 8)
        text = std::to_string(le32s(v)) + ", " + std::to_string(le32s(v + 4));
    else if (type == "rational" && n == 8)
        text = std::to_string(le32s(v)) + "/" + std::to_string(le32(v + 4));
    else if (type == "envmap" && n == 1)
        text = v[0] == 0 ? "latitude-longitude" : v[0] == 1 ? "cube" : std::to_string(v[0]);
    else
        return false;
    // Defaults every file carries say nothing.
    if ((name == "pixelAspectRatio" || name == "screenWindowWidth") && text == "1")
        return false;
    if (name == "screenWindowCenter" && text == "0, 0")
        return false;
    return !text.empty();
}

// One header: attributes until an empty name.
bool parse_header(const uint8_t* d, size_t size, size_t& pos, Part& part, std::string& error)
{
    const uint8_t* const end = d + size;
    const uint8_t* p = d + pos;
    for (int count = 0;; ++count)
    {
        if (p >= end || count > 100000)
        {
            error = "EXR header is cut short";
            return false;
        }
        if (*p == 0)
        {
            ++p;
            break;
        }
        std::string name, type;
        if (!read_name(p, end, name) || !read_name(p, end, type) || end - p < 4)
        {
            error = "EXR header is cut short";
            return false;
        }
        const int32_t n = le32s(p);
        p += 4;
        if (n < 0 || n > end - p)
        {
            error = "EXR attribute " + name + " is cut short";
            return false;
        }
        const uint8_t* v = p;
        p += n;
        auto bad = [&]
        {
            error = "EXR attribute " + name + " is malformed";
            return false;
        };
        if (name == "channels" && type == "chlist")
        {
            part.channels.clear();
            if (!parse_channels(v, v + n, part.channels))
                return bad();
        }
        else if (name == "compression" && type == "compression")
        {
            if (n != 1)
                return bad();
            part.compression = v[0];
        }
        else if ((name == "dataWindow" || name == "displayWindow") && type == "box2i")
        {
            if (n != 16)
                return bad();
            Box b{le32s(v), le32s(v + 4), le32s(v + 8), le32s(v + 12)};
            if (name == "dataWindow")
                part.data_window = b, part.has_data_window = true;
            else
                part.display_window = b, part.has_display_window = true;
        }
        else if (name == "lineOrder" && type == "lineOrder")
        {
            if (n != 1)
                return bad();
            part.line_order = v[0];
        }
        else if (name == "tiles" && type == "tiledesc")
        {
            if (n != 9)
                return bad();
            part.tile_w = le32(v);
            part.tile_h = le32(v + 4);
            part.level_mode = v[8] & 0xF;
        }
        else if (name == "chunkCount" && type == "int")
        {
            if (n != 4)
                return bad();
            part.chunk_count = le32s(v);
        }
        else if (name == "name" && type == "string")
            part.name = text_of(v, size_t(n));
        else if (name == "type" && type == "string")
            part.type = text_of(v, size_t(n));
        else if (name == "chromaticities" && type == "chromaticities")
        {
            if (n != 32)
                return bad();
            for (int i = 0; i < 8; ++i)
                part.chromaticities[i] = lef(v + 4 * i);
            part.has_chromaticities = true;
        }
        else if (name != "version")
        {
            std::string text;
            if (attribute_text(name, type, v, size_t(n), text))
            {
                const char* label = friendly_name(name);
                if (text.size() > 2000)
                {
                    size_t cut = 2000;
                    while (cut && (uint8_t(text[cut]) & 0xC0) == 0x80)
                        --cut;
                    text = text.substr(0, cut) + "…";
                }
                part.attributes.push_back({label ? label : name, text});
            }
        }
    }
    pos = size_t(p - d);
    return true;
}

// --- Decompression --------------------------------------------------------
//
// Each codec turns a chunk back into its plain layout: for each line of the
// chunk's rectangle, for each channel sampled on that line, its samples in
// little-endian order.

// Working memory, one per thread, reused from chunk to chunk.
struct Scratch
{
    std::vector<uint8_t> plain, tmp;
    std::vector<uint16_t> shorts;
    std::vector<uint64_t> hcode;
    struct Dec
    {
        int len;
        uint32_t lit;
    };
    std::vector<Dec> hdec;
    std::vector<std::vector<uint32_t>> hlong;  // long codes sharing a table entry
    std::vector<uint16_t> lut;
};

struct Chunk
{
    const Part* part;
    Box box;  // clipped to the data window
    const uint8_t* data = nullptr;
    size_t size = 0;   // compressed
    size_t plain = 0;  // uncompressed
};

// Bytes of a chunk in the plain layout.
size_t plain_size(const Chunk& c)
{
    size_t plain = 0;
    for (int y = c.box.y0; y <= c.box.y1; ++y)
        for (const Channel& ch : c.part->channels)
            if (modp(y, ch.ys) == 0)
                plain += size_t(num_samples(ch.xs, c.box.x0, c.box.x1)) * type_size(ch.type);
    return plain;
}

bool inflate_exact(const uint8_t* in, size_t n, uint8_t* out, size_t out_n)
{
    uLongf len = uLongf(out_n);
    return uncompress(out, &len, in, uLong(n)) == Z_OK && len == out_n;
}

// ZIP and RLE store bytes delta-coded, the odd bytes after the even ones.
void unpredict(uint8_t* t, size_t n, uint8_t* out)
{
    for (size_t i = 1; i < n; ++i)
        t[i] = uint8_t(t[i - 1] + t[i] - 128);
    const uint8_t* t1 = t;
    const uint8_t* t2 = t + (n + 1) / 2;
    for (size_t i = 0; i + 1 < n; i += 2)
    {
        out[i] = *t1++;
        out[i + 1] = *t2++;
    }
    if (n & 1)
        out[n - 1] = *t1;
}

bool rle_decode(const uint8_t* in, size_t n, uint8_t* out, size_t out_n)
{
    size_t i = 0, o = 0;
    while (i < n)
    {
        const int count = int8_t(in[i++]);
        if (count < 0)
        {
            const size_t len = size_t(-count);
            if (len > n - i || len > out_n - o)
                return false;
            std::memcpy(out + o, in + i, len);
            i += len;
            o += len;
        }
        else
        {
            const size_t len = size_t(count) + 1;
            if (i >= n || len > out_n - o)
                return false;
            std::memset(out + o, in[i++], len);
            o += len;
        }
    }
    return o == out_n;
}

// PXR24: zlib over byte planes of each line's channel, delta-coded, floats
// cut to 24 bits.
bool pxr24_decode(const Chunk& c, const uint8_t* in, size_t n, uint8_t* out, size_t out_n, Scratch& s)
{
    size_t packed = 0;
    for (int y = c.box.y0; y <= c.box.y1; ++y)
        for (const Channel& ch : c.part->channels)
            if (modp(y, ch.ys) == 0)
                packed += size_t(num_samples(ch.xs, c.box.x0, c.box.x1)) * (ch.type == kHalf ? 2 : ch.type == kFloat ? 3 : 4);
    s.tmp.resize(packed);
    if (!inflate_exact(in, n, s.tmp.data(), packed))
        return false;
    const uint8_t* t = s.tmp.data();
    uint8_t* o = out;
    for (int y = c.box.y0; y <= c.box.y1; ++y)
        for (const Channel& ch : c.part->channels)
        {
            if (modp(y, ch.ys) != 0)
                continue;
            const size_t m = size_t(num_samples(ch.xs, c.box.x0, c.box.x1));
            uint32_t pixel = 0;
            if (ch.type == kHalf)
            {
                for (size_t j = 0; j < m; ++j)
                {
                    pixel += uint32_t(t[j]) << 8 | t[m + j];
                    *o++ = uint8_t(pixel);
                    *o++ = uint8_t(pixel >> 8);
                }
                t += 2 * m;
            }
            else
            {
                const bool f = ch.type == kFloat;
                for (size_t j = 0; j < m; ++j)
                {
                    pixel += f ? uint32_t(t[j]) << 24 | uint32_t(t[m + j]) << 16 | uint32_t(t[2 * m + j]) << 8
                               : uint32_t(t[j]) << 24 | uint32_t(t[m + j]) << 16 | uint32_t(t[2 * m + j]) << 8 | t[3 * m + j];
                    for (int k = 0; k < 4; ++k)
                        *o++ = uint8_t(pixel >> (8 * k));
                }
                t += (f ? 3 : 4) * m;
            }
        }
    return size_t(o - out) == out_n;
}

// Channels stored one after another (PIZ, B44), each nx × ny samples, back
// to the plain layout.
void interleave(const Chunk& c, const uint8_t* planes, uint8_t* out)
{
    std::vector<const uint8_t*> at;
    const uint8_t* p = planes;
    for (const Channel& ch : c.part->channels)
    {
        at.push_back(p);
        p += size_t(num_samples(ch.xs, c.box.x0, c.box.x1)) * num_samples(ch.ys, c.box.y0, c.box.y1) * type_size(ch.type);
    }
    for (int y = c.box.y0; y <= c.box.y1; ++y)
        for (size_t i = 0; i < c.part->channels.size(); ++i)
        {
            const Channel& ch = c.part->channels[i];
            if (modp(y, ch.ys) != 0)
                continue;
            const size_t n = size_t(num_samples(ch.xs, c.box.x0, c.box.x1)) * type_size(ch.type);
            std::memcpy(out, at[i], n);
            at[i] += n;
            out += n;
        }
}

// B44 compresses pLinear (perceptually linear) halves as exp(x / 8), so
// they come back through 8 log(x); negatives, infinities and NaNs as 0.
const std::vector<uint16_t>& b44_log_table()
{
    static const std::vector<uint16_t> table = []
    {
        std::vector<uint16_t> t(65536);
        for (int i = 0; i < 65536; ++i)
        {
            const uint16_t h = uint16_t(i);
            t[size_t(i)] = (h & 0x7C00) == 0x7C00 || h > 0x8000 ? 0 : float_to_half(8 * std::log(half_to_float(h)));
        }
        return t;
    }();
    return table;
}

// Sixteen halves from a 14-byte block: the first value, a shift, and 6-bit
// differences down the columns and across the top row.
void unpack14(const uint8_t b[14], uint16_t s[16])
{
    s[0] = uint16_t(b[0] << 8 | b[1]);
    const int shift = b[2] >> 2;
    const int bias = 0x20 << shift;
    s[4] = uint16_t(s[0] + ((((b[2] << 4) | (b[3] >> 4)) & 0x3f) << shift) - bias);
    s[8] = uint16_t(s[4] + ((((b[3] << 2) | (b[4] >> 6)) & 0x3f) << shift) - bias);
    s[12] = uint16_t(s[8] + ((b[4] & 0x3f) << shift) - bias);
    s[1] = uint16_t(s[0] + ((b[5] >> 2) << shift) - bias);
    s[5] = uint16_t(s[4] + ((((b[5] << 4) | (b[6] >> 4)) & 0x3f) << shift) - bias);
    s[9] = uint16_t(s[8] + ((((b[6] << 2) | (b[7] >> 6)) & 0x3f) << shift) - bias);
    s[13] = uint16_t(s[12] + ((b[7] & 0x3f) << shift) - bias);
    s[2] = uint16_t(s[1] + ((b[8] >> 2) << shift) - bias);
    s[6] = uint16_t(s[5] + ((((b[8] << 4) | (b[9] >> 4)) & 0x3f) << shift) - bias);
    s[10] = uint16_t(s[9] + ((((b[9] << 2) | (b[10] >> 6)) & 0x3f) << shift) - bias);
    s[14] = uint16_t(s[13] + ((b[10] & 0x3f) << shift) - bias);
    s[3] = uint16_t(s[2] + ((b[11] >> 2) << shift) - bias);
    s[7] = uint16_t(s[6] + ((((b[11] << 4) | (b[12] >> 4)) & 0x3f) << shift) - bias);
    s[11] = uint16_t(s[10] + ((((b[12] << 2) | (b[13] >> 6)) & 0x3f) << shift) - bias);
    s[15] = uint16_t(s[14] + ((b[13] & 0x3f) << shift) - bias);
    // Stored as ordered integers: back to sign and magnitude.
    for (int i = 0; i < 16; ++i)
        s[i] = s[i] & 0x8000 ? uint16_t(s[i] & 0x7fff) : uint16_t(~s[i]);
}

// A flat block (B44A): one value for all sixteen.
void unpack3(const uint8_t b[3], uint16_t s[16])
{
    uint16_t v = uint16_t(b[0] << 8 | b[1]);
    v = v & 0x8000 ? uint16_t(v & 0x7fff) : uint16_t(~v);
    for (int i = 0; i < 16; ++i)
        s[i] = v;
}

// B44, B44A: halves in 4×4 blocks of 14 bytes (or 3 when flat); other
// types as they are.
bool b44_decode(const Chunk& c, const uint8_t* in, size_t n, uint8_t* out, size_t out_n, Scratch& s)
{
    s.tmp.resize(out_n);
    uint8_t* plane = s.tmp.data();
    for (const Channel& ch : c.part->channels)
    {
        const int nx = num_samples(ch.xs, c.box.x0, c.box.x1), ny = num_samples(ch.ys, c.box.y0, c.box.y1);
        if (ch.type != kHalf)
        {
            const size_t bytes = size_t(nx) * ny * 4;
            if (n < bytes)
                return false;
            std::memcpy(plane, in, bytes);
            in += bytes, n -= bytes, plane += bytes;
            continue;
        }
        const std::vector<uint16_t>* log = ch.linear ? &b44_log_table() : nullptr;
        for (int y = 0; y < ny; y += 4)
            for (int x = 0; x < nx; x += 4)
            {
                uint16_t v[16];
                if (n < 3)
                    return false;
                if (in[2] >= 13 << 2)
                {
                    unpack3(in, v);
                    in += 3, n -= 3;
                }
                else
                {
                    if (n < 14)
                        return false;
                    unpack14(in, v);
                    in += 14, n -= 14;
                }
                if (log)
                    for (uint16_t& h : v)
                        h = (*log)[h];
                // Only the part of the block inside the rectangle.
                for (int r = 0; r < 4 && y + r < ny; ++r)
                    for (int k = 0; k < 4 && x + k < nx; ++k)
                    {
                        uint8_t* p = plane + (size_t(y + r) * nx + x + k) * 2;
                        p[0] = uint8_t(v[r * 4 + k]);
                        p[1] = uint8_t(v[r * 4 + k] >> 8);
                    }
            }
        plane += size_t(nx) * ny * 2;
    }
    interleave(c, s.tmp.data(), out);
    return true;
}

// PIZ's Huffman coder: a code-length table (run-length coded), canonical
// codes, then the bit stream, with the largest symbol meaning "repeat the
// last value".
constexpr int kHufEncSize = (1 << 16) + 1, kHufDecBits = 14, kHufDecSize = 1 << kHufDecBits;

struct BitReader
{
    const uint8_t* data;
    size_t size, pos = 0;
    uint64_t c = 0;
    int lc = 0;
    void get_char()
    {
        c = c << 8 | (pos < size ? data[pos] : 0);
        ++pos;
        lc += 8;
    }
    uint64_t get_bits(int n)
    {
        while (lc < n)
            get_char();
        lc -= n;
        return (c >> lc) & ((uint64_t(1) << n) - 1);
    }
};

bool huf_decode(const uint8_t* in, size_t n, uint16_t* raw, size_t n_raw, Scratch& s, std::string& error)
{
    error = "EXR PIZ data is corrupt";
    if (n == 0 || n < 20)
        return n_raw == 0;
    const uint32_t im = le32(in), i_max = le32(in + 4), n_bits = le32(in + 12);
    if (im >= uint32_t(kHufEncSize) || i_max >= uint32_t(kHufEncSize))
        return false;
    // Code lengths of the symbols im..i_max, six bits each; 59..62 are short
    // runs of zeros, 63 a long one with an 8-bit count.
    s.hcode.assign(kHufEncSize, 0);
    uint64_t* hcode = s.hcode.data();
    BitReader table{in + 20, n - 20};
    for (uint32_t i = im; i <= i_max; ++i)
    {
        if (table.pos >= table.size)
            return false;
        const uint64_t l = hcode[i] = table.get_bits(6);
        if (l == 63)
        {
            if (table.pos > table.size)
                return false;
            const uint32_t run = uint32_t(table.get_bits(8)) + 6;
            if (i + run > i_max + 1)
                return false;
            for (uint32_t k = 0; k < run; ++k)
                hcode[i++] = 0;
            --i;
        }
        else if (l >= 59)
        {
            const uint32_t run = uint32_t(l) - 59 + 2;
            if (i + run > i_max + 1)
                return false;
            for (uint32_t k = 0; k < run; ++k)
                hcode[i++] = 0;
            --i;
        }
    }
    if (table.pos > table.size)
        return false;
    // Canonical codes: hcode = code << 6 | length.
    {
        uint64_t count[59] = {};
        for (int i = 0; i < kHufEncSize; ++i)
            count[hcode[i]] += 1;
        uint64_t code = 0;
        for (int i = 58; i > 0; --i)
        {
            const uint64_t next = (code + count[i]) >> 1;
            count[i] = code;
            code = next;
        }
        for (int i = 0; i < kHufEncSize; ++i)
        {
            const int l = int(hcode[i]);
            if (l > 0)
                hcode[i] = uint64_t(l) | count[l]++ << 6;
        }
    }
    // Decoding table: codes up to 14 bits fill their entries, longer ones
    // are listed under the entry of their first 14 bits.
    s.hdec.assign(kHufDecSize, Scratch::Dec{0, 0});
    s.hlong.resize(kHufDecSize);
    for (auto& v : s.hlong)
        v.clear();
    for (uint32_t i = im; i <= i_max; ++i)
    {
        const uint64_t code = hcode[i] >> 6;
        const int l = int(hcode[i] & 63);
        if (code >> l)
            return false;
        if (l > kHufDecBits)
        {
            const size_t e = size_t(code >> (l - kHufDecBits));
            if (s.hdec[e].len)
                return false;
            s.hlong[e].push_back(i);
        }
        else if (l)
        {
            const size_t e = size_t(code << (kHufDecBits - l));
            for (size_t k = 0; k < size_t(1) << (kHufDecBits - l); ++k)
            {
                if (s.hdec[e + k].len || !s.hlong[e + k].empty())
                    return false;
                s.hdec[e + k] = {l, i};
            }
        }
    }
    const size_t data_size = n - 20 - table.pos;
    if (n_bits > 8 * uint64_t(data_size))
        return false;
    BitReader bits{in + 20 + table.pos, data_size};
    const size_t end = (size_t(n_bits) + 7) / 8;
    size_t o = 0;
    const uint32_t rlc = i_max;
    auto put = [&](uint32_t sym)
    {
        if (sym == rlc)
        {
            if (bits.lc < 8)
                bits.get_char();
            bits.lc -= 8;
            const uint8_t run = uint8_t(bits.c >> bits.lc);
            if (run > n_raw - o || o == 0)
                return false;
            const uint16_t v = raw[o - 1];
            for (int k = 0; k < run; ++k)
                raw[o++] = v;
        }
        else
        {
            if (o >= n_raw)
                return false;
            raw[o++] = uint16_t(sym);
        }
        return true;
    };
    while (bits.pos < end)
    {
        bits.get_char();
        while (bits.lc >= kHufDecBits)
        {
            const size_t e = size_t(bits.c >> (bits.lc - kHufDecBits)) & (kHufDecSize - 1);
            const Scratch::Dec pl = s.hdec[e];
            if (pl.len)
            {
                bits.lc -= pl.len;
                if (!put(pl.lit))
                    return false;
                continue;
            }
            const std::vector<uint32_t>& list = s.hlong[e];
            if (list.empty())
                return false;
            bool found = false;
            for (uint32_t sym : list)
            {
                const int l = int(hcode[sym] & 63);
                while (bits.lc < l && bits.pos < end)
                    bits.get_char();
                if (bits.lc >= l && (hcode[sym] >> 6) == ((bits.c >> (bits.lc - l)) & ((uint64_t(1) << l) - 1)))
                {
                    bits.lc -= l;
                    if (!put(sym))
                        return false;
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
    }
    // The last short codes, past the padding bits.
    const int pad = int((8 - n_bits) & 7);
    bits.c >>= pad;
    bits.lc -= pad;
    while (bits.lc > 0)
    {
        const Scratch::Dec pl = s.hdec[size_t(bits.c << (kHufDecBits - bits.lc)) & (kHufDecSize - 1)];
        if (!pl.len || pl.len > bits.lc)
            return false;
        bits.lc -= pl.len;
        if (!put(pl.lit))
            return false;
    }
    if (o != n_raw)
        return false;
    error.clear();
    return true;
}

// PIZ's Haar wavelet, undone level by level from the coarsest; 14-bit
// arithmetic when the values fit, else modulo 2^16.
void wdec14(uint16_t l, uint16_t h, uint16_t& a, uint16_t& b)
{
    const int ls = int16_t(l), hi = int16_t(h);
    const int ai = ls + (hi & 1) + (hi >> 1);
    a = uint16_t(int16_t(ai));
    b = uint16_t(int16_t(ai - hi));
}

void wdec16(uint16_t l, uint16_t h, uint16_t& a, uint16_t& b)
{
    const int m = l, d = h;
    const int bb = (m - (d >> 1)) & 0xFFFF;
    const int aa = (d + bb - 0x8000) & 0xFFFF;
    b = uint16_t(bb);
    a = uint16_t(aa);
}

void wav2_decode(uint16_t* in, int nx, int ox, int ny, int oy, uint16_t mx)
{
    const bool w14 = mx < (1 << 14);
    auto dec = [w14](uint16_t l, uint16_t h, uint16_t& a, uint16_t& b) { w14 ? wdec14(l, h, a, b) : wdec16(l, h, a, b); };
    const int n = std::min(nx, ny);
    int p = 1;
    while (p <= n)
        p <<= 1;
    p >>= 1;
    int p2 = p;
    p >>= 1;
    while (p >= 1)
    {
        const ptrdiff_t oy1 = ptrdiff_t(oy) * p, oy2 = ptrdiff_t(oy) * p2, ox1 = ptrdiff_t(ox) * p, ox2 = ptrdiff_t(ox) * p2;
        const ptrdiff_t ey = ptrdiff_t(oy) * (ny - p2);
        ptrdiff_t py = 0;
        for (; py <= ey; py += oy2)
        {
            ptrdiff_t px = py;
            const ptrdiff_t ex = py + ptrdiff_t(ox) * (nx - p2);
            for (; px <= ex; px += ox2)
            {
                const ptrdiff_t p01 = px + ox1, p10 = px + oy1, p11 = p10 + ox1;
                uint16_t i00, i01, i10, i11;
                dec(in[px], in[p10], i00, i10);
                dec(in[p01], in[p11], i01, i11);
                dec(i00, i01, in[px], in[p01]);
                dec(i10, i11, in[p10], in[p11]);
            }
            // An odd column.
            if (nx & p)
            {
                const ptrdiff_t p10 = px + oy1;
                uint16_t i00;
                dec(in[px], in[p10], i00, in[p10]);
                in[px] = i00;
            }
        }
        // An odd line.
        if (ny & p)
        {
            ptrdiff_t px = py;
            const ptrdiff_t ex = py + ptrdiff_t(ox) * (nx - p2);
            for (; px <= ex; px += ox2)
            {
                const ptrdiff_t p01 = px + ox1;
                uint16_t i00;
                dec(in[px], in[p01], i00, in[p01]);
                in[px] = i00;
            }
        }
        p2 = p;
        p >>= 1;
    }
}

// PIZ: a bitmap of the 16-bit values used, Huffman-coded indices into it,
// and each channel's 16-bit planes wavelet-transformed.
bool piz_decode(const Chunk& c, const uint8_t* in, size_t n, uint8_t* out, size_t out_n, Scratch& s, std::string& error)
{
    error = "EXR PIZ data is corrupt";
    if (n < 4)
        return false;
    const uint16_t min_nz = le16(in), max_nz = le16(in + 2);
    in += 4, n -= 4;
    constexpr int kBitmapSize = 8192;
    if (min_nz >= kBitmapSize || max_nz >= kBitmapSize)
        return false;
    uint8_t bitmap[kBitmapSize] = {};
    if (min_nz <= max_nz)
    {
        const size_t len = size_t(max_nz) - min_nz + 1;
        if (n < len)
            return false;
        std::memcpy(bitmap + min_nz, in, len);
        in += len, n -= len;
    }
    // The values present, in order: an index into them was coded.
    s.lut.assign(65536, 0);
    int k = 0;
    for (int v = 0; v < 65536; ++v)
        if (v == 0 || (bitmap[v >> 3] & (1 << (v & 7))))
            s.lut[size_t(k++)] = uint16_t(v);
    const uint16_t max_value = uint16_t(k - 1);
    if (n < 4)
        return false;
    const int32_t length = le32s(in);
    in += 4, n -= 4;
    if (length < 0 || size_t(length) > n)
        return false;
    const size_t n_shorts = out_n / 2;
    s.shorts.resize(n_shorts);
    if (!huf_decode(in, size_t(length), s.shorts.data(), n_shorts, s, error))
        return false;
    uint16_t* plane = s.shorts.data();
    for (const Channel& ch : c.part->channels)
    {
        const int nx = num_samples(ch.xs, c.box.x0, c.box.x1), ny = num_samples(ch.ys, c.box.y0, c.box.y1);
        const int words = type_size(ch.type) / 2;
        for (int j = 0; j < words; ++j)
            wav2_decode(plane + j, nx, words, ny, nx * words, max_value);
        plane += size_t(nx) * ny * words;
    }
    for (uint16_t& v : s.shorts)
        v = s.lut[v];
    s.tmp.resize(out_n);
    for (size_t i = 0; i < n_shorts; ++i)
    {
        s.tmp[2 * i] = uint8_t(s.shorts[i]);
        s.tmp[2 * i + 1] = uint8_t(s.shorts[i] >> 8);
    }
    interleave(c, s.tmp.data(), out);
    error.clear();
    return true;
}

// A chunk's data into s.plain, in the plain layout.
bool decompress(const Chunk& c, Scratch& s, std::string& error)
{
    const uint8_t* in = c.data;
    const size_t n = c.size, plain = c.plain;
    s.plain.resize(plain);
    const int comp = c.part->compression;
    // Stored as it is when compressing didn't make it smaller.
    if (comp == kNone || n >= plain)
    {
        if (n < plain)
        {
            error = "EXR chunk is cut short";
            return false;
        }
        std::memcpy(s.plain.data(), in, plain);
        return true;
    }
    bool ok = false;
    switch (comp)
    {
        case kRle:
        case kZips:
        case kZip:
            s.tmp.resize(plain);
            ok = comp == kRle ? rle_decode(in, n, s.tmp.data(), plain) : inflate_exact(in, n, s.tmp.data(), plain);
            if (ok)
                unpredict(s.tmp.data(), plain, s.plain.data());
            break;
        case kPxr24: ok = pxr24_decode(c, in, n, s.plain.data(), plain, s); break;
        case kB44:
        case kB44a: ok = b44_decode(c, in, n, s.plain.data(), plain, s); break;
        case kPiz: return piz_decode(c, in, n, s.plain.data(), plain, s, error);
    }
    if (!ok)
        error = std::string("EXR ") + compression_name(comp) + " data is corrupt";
    return ok;
}

// --- Channels shown -------------------------------------------------------

struct Selection
{
    std::vector<int> dest;      // per file channel: index in the source, -1 = not shown
    int channels = 0;           // 1 grey, 2 grey + alpha, 3 RGB, 4 RGBA
    bool missing = false;       // a colour is absent: zero
    std::string shown;          // a note when it's not plain R, G, B
};

void split_name(const std::string& n, std::string& layer, std::string& base)
{
    const size_t dot = n.rfind('.');
    layer = dot == std::string::npos ? "" : n.substr(0, dot);
    base = dot == std::string::npos ? n : n.substr(dot + 1);
}

bool select_channels(const Part& part, Selection& sel, std::vector<std::string>& layers, std::string& error)
{
    struct Layer
    {
        std::string name;
        int r = -1, g = -1, b = -1, a = -1, y = -1, ry = -1, by = -1;
    };
    std::vector<Layer> ls;
    for (size_t i = 0; i < part.channels.size(); ++i)
    {
        std::string layer, base;
        split_name(part.channels[i].name, layer, base);
        auto it = std::find_if(ls.begin(), ls.end(), [&](const Layer& l) { return l.name == layer; });
        if (it == ls.end())
        {
            ls.push_back({layer});
            it = ls.end() - 1;
        }
        const int c = int(i);
        if (base == "R")
            it->r = c;
        else if (base == "G")
            it->g = c;
        else if (base == "B")
            it->b = c;
        else if (base == "A")
            it->a = c;
        else if (base == "Y")
            it->y = c;
        else if (base == "RY")
            it->ry = c;
        else if (base == "BY")
            it->by = c;
    }
    for (const Layer& l : ls)
        if (!l.name.empty())
            layers.push_back(l.name);
    auto rgb_all = [](const Layer& l) { return l.r >= 0 && l.g >= 0 && l.b >= 0; };
    auto rgb_any = [](const Layer& l) { return l.r >= 0 || l.g >= 0 || l.b >= 0; };
    const Layer* pick = nullptr;
    bool grey = false;
    // Unprefixed first, then the first layer with all of R, G, B, then any
    // with some of them, then any with Y.
    auto unprefixed = std::find_if(ls.begin(), ls.end(), [](const Layer& l) { return l.name.empty(); });
    if (unprefixed != ls.end() && (rgb_any(*unprefixed) || unprefixed->y >= 0))
        pick = &*unprefixed, grey = !rgb_any(*unprefixed);
    for (const Layer& l : ls)
        if (!pick && rgb_all(l))
            pick = &l;
    for (const Layer& l : ls)
        if (!pick && rgb_any(l))
            pick = &l;
    for (const Layer& l : ls)
        if (!pick && l.y >= 0)
            pick = &l, grey = true;
    sel.dest.assign(part.channels.size(), -1);
    if (!pick)
    {
        // Nothing to call colour: the first channel, as grey.
        sel.dest[0] = 0;
        sel.channels = 1;
        sel.shown = part.channels[0].name + " as grey";
    }
    else if (grey)
    {
        if (pick->ry >= 0 || pick->by >= 0)
        {
            error = "EXR luminance/chroma images (Y, RY, BY) aren't supported";
            return false;
        }
        sel.dest[size_t(pick->y)] = 0;
        if (pick->a >= 0)
            sel.dest[size_t(pick->a)] = 1;
        sel.channels = pick->a >= 0 ? 2 : 1;
    }
    else
    {
        const int rgb[3] = {pick->r, pick->g, pick->b};
        for (int k = 0; k < 3; ++k)
            if (rgb[k] >= 0)
                sel.dest[size_t(rgb[k])] = k;
            else
                sel.missing = true;
        if (pick->a >= 0)
            sel.dest[size_t(pick->a)] = 3;
        sel.channels = pick->a >= 0 ? 4 : 3;
    }
    if (pick && !pick->name.empty())
        sel.shown = "layer " + pick->name;
    for (size_t i = 0; i < part.channels.size(); ++i)
    {
        const Channel& ch = part.channels[i];
        if (sel.dest[i] >= 0 && (ch.xs != 1 || ch.ys != 1))
        {
            error = "EXR channel " + ch.name + " is subsampled (" + std::to_string(ch.xs) + "×" + std::to_string(ch.ys) + "), which isn't supported";
            return false;
        }
    }
    return true;
}

// The shown channels of a chunk's plain data into the source.
void extract(const Chunk& c, const uint8_t* plain, const Selection& sel, const Box& dw, SourceData& src)
{
    const bool f32 = src.sample == SourceData::Sample::F32;
    const size_t px = src.pixel_bytes();
    const uint8_t* p = plain;
    for (int y = c.box.y0; y <= c.box.y1; ++y)
        for (size_t i = 0; i < c.part->channels.size(); ++i)
        {
            const Channel& ch = c.part->channels[i];
            if (modp(y, ch.ys) != 0)
                continue;
            const int n = num_samples(ch.xs, c.box.x0, c.box.x1);
            const int d = sel.dest[i];
            if (d >= 0)
            {
                uint8_t* o = src.pixel(c.box.x0 - dw.x0, y - dw.y0) + size_t(d) * src.sample_bytes();
                for (int x = 0; x < n; ++x, o += px)
                {
                    const uint8_t* s = p + size_t(x) * type_size(ch.type);
                    if (!f32)
                    {
                        const uint16_t h = le16(s);
                        std::memcpy(o, &h, 2);
                        continue;
                    }
                    const float v = ch.type == kHalf ? half_to_float(le16(s)) : ch.type == kFloat ? lef(s) : float(le32(s));
                    std::memcpy(o, &v, 4);
                }
            }
            p += size_t(n) * type_size(ch.type);
        }
}

// The file stores colour multiplied by alpha; the source has it straight.
// Where alpha is 0 (or less) the colour is kept as it is: it's light added
// without covering anything, and shows as transparent either way. So are
// infinities and NaNs.
void unpremultiply(SourceData& src, int x0, int y0, int x1, int y1)
{
    const int alpha = src.channels - 1, colours = src.channels == 4 ? 3 : 1;
    const bool f32 = src.sample == SourceData::Sample::F32;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            uint8_t* p = src.pixel(x, y);
            const float a = src.value(p, alpha);
            if (!(a > 0) || a == 1)
                continue;
            for (int c = 0; c < colours; ++c)
            {
                const float v = src.value(p, c);
                if (!std::isfinite(v))
                    continue;
                if (f32)
                {
                    const float u = v / a;
                    std::memcpy(p + 4 * c, &u, 4);
                }
                else
                {
                    const uint16_t h = float_to_half(v / a);
                    std::memcpy(p + 2 * c, &h, 2);
                }
            }
        }
}

std::string colour_space(const float* c)
{
    auto near = [c](std::initializer_list<float> v)
    {
        int i = 0;
        for (float f : v)
            if (std::fabs(c[i++] - f) > 0.002f)
                return false;
        return true;
    };
    if (near({0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f}))
        return "Linear Rec. 709";
    if (near({0.7347f, 0.2653f, 0.0f, 1.0f, 0.0001f, -0.0770f, 0.32168f, 0.33767f}))
        return "ACES2065-1 (AP0)";
    if (near({0.713f, 0.293f, 0.165f, 0.830f, 0.128f, 0.044f, 0.32168f, 0.33767f}))
        return "ACEScg (AP1)";
    if (near({0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f}))
        return "Linear Rec. 2020";
    if (near({1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f / 3, 1.0f / 3}))
        return "CIE XYZ";
    return "Linear, custom primaries";
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    const uint8_t* d = nullptr;
    size_t size = 0;
    if (!file_bytes(path, d, size) || size < 8 || !sniff(d, size))
    {
        error = "not an EXR file";
        return false;
    }
    const uint32_t version = le32(d + 4);
    if ((version & 0xFF) != 2)
    {
        error = "EXR version " + std::to_string(version & 0xFF) + " isn't supported";
        return false;
    }
    const bool multipart = version & kMultipartFlag;
    if (!multipart && (version & kDeepFlag))
    {
        error = "EXR deep images aren't supported";
        return false;
    }

    // Headers: one, or one per part ended by an empty one.
    std::vector<Part> parts;
    size_t pos = 8;
    do
    {
        if (pos >= size)
        {
            error = "EXR header is cut short";
            return false;
        }
        if (multipart && d[pos] == 0)
        {
            ++pos;
            break;
        }
        if (parts.size() >= 10000)
        {
            error = "EXR file has too many parts";
            return false;
        }
        parts.emplace_back();
        if (!parse_header(d, size, pos, parts.back(), error))
            return false;
    } while (multipart);
    if (parts.empty())
    {
        error = "EXR file has no parts";
        return false;
    }

    // The first part with image data, one with colour (R, G, B or Y in any
    // layer) by preference to one with only depth or the like.
    int chosen = -1, first = -1;
    for (size_t i = 0; i < parts.size() && chosen < 0; ++i)
    {
        const std::string& t = parts[i].type;
        if (!t.empty() && t != "scanlineimage" && t != "tiledimage")
            continue;
        if (first < 0)
            first = int(i);
        for (const Channel& ch : parts[i].channels)
        {
            std::string layer, base;
            split_name(ch.name, layer, base);
            if (base == "R" || base == "G" || base == "B" || base == "Y")
                chosen = int(i);
        }
    }
    if (chosen < 0)
        chosen = first;
    if (chosen < 0)
    {
        error = "EXR file has only deep data, which isn't supported";
        return false;
    }
    Part& part = parts[size_t(chosen)];
    // Tiled by the version flag (one part) or by its type (several).
    part.tiled = multipart ? part.type == "tiledimage" : (version & kTiledFlag) != 0;
    if (part.channels.empty() || !part.has_data_window || part.compression < 0)
    {
        error = "EXR header lacks channels, data window or compression";
        return false;
    }
    if (part.compression > kB44a)
    {
        const char* name = compression_name(part.compression);
        error = "EXR " + (name ? std::string(name) : "compression " + std::to_string(part.compression)) + " compression isn't supported";
        return false;
    }
    const Box& dw = part.data_window;
    constexpr int kCoordLimit = 1 << 30;
    if (dw.x1 < dw.x0 || dw.y1 < dw.y0 || dw.x0 < -kCoordLimit || dw.y0 < -kCoordLimit || dw.x1 > kCoordLimit || dw.y1 > kCoordLimit ||
        (int64_t(dw.x1) - dw.x0 + 1) * (int64_t(dw.y1) - dw.y0 + 1) > (int64_t(1) << 30))
    {
        error = "bad EXR data window";
        return false;
    }
    const int w = dw.width(), h = dw.height();
    if (part.tiled && (part.tile_w < 1 || part.tile_h < 1 || part.tile_w > uint32_t(kCoordLimit) || part.tile_h > uint32_t(kCoordLimit)))
    {
        error = "bad EXR tile size";
        return false;
    }

    // The chunks of level 0, and where their offsets are.
    std::vector<Chunk> chunks;
    if (part.tiled)
    {
        const int tw = int(part.tile_w), th = int(part.tile_h);
        const int nx = int((int64_t(w) + tw - 1) / tw), ny = int((int64_t(h) + th - 1) / th);
        if (int64_t(nx) * ny > int64_t(size / 8))
        {
            error = "EXR offset table is cut short";
            return false;
        }
        for (int ty = 0; ty < ny; ++ty)
            for (int tx = 0; tx < nx; ++tx)
            {
                Box b;
                b.x0 = dw.x0 + tx * tw;
                b.y0 = dw.y0 + ty * th;
                b.x1 = int(std::min<int64_t>(dw.x1, int64_t(b.x0) + tw - 1));
                b.y1 = int(std::min<int64_t>(dw.y1, int64_t(b.y0) + th - 1));
                chunks.push_back({&part, b});
            }
    }
    else
    {
        const int lines = lines_per_chunk(part.compression);
        for (int64_t y = dw.y0; y <= dw.y1; y += lines)
        {
            Box b{dw.x0, int(y), dw.x1, int(std::min<int64_t>(dw.y1, y + lines - 1))};
            chunks.push_back({&part, b});
        }
    }
    // Every part's offset table, one after another.
    size_t table = pos;
    for (int i = 0; i < chosen; ++i)
    {
        if (parts[size_t(i)].chunk_count < 0 || uint64_t(parts[size_t(i)].chunk_count) > (size - table) / 8)
        {
            error = "EXR offset table is cut short";
            return false;
        }
        table += size_t(parts[size_t(i)].chunk_count) * 8;
    }
    if (multipart && part.chunk_count < int64_t(chunks.size()))
    {
        error = "EXR part's chunk count is wrong";
        return false;
    }
    if (chunks.size() > (size - std::min(size, table)) / 8)
    {
        error = "EXR offset table is cut short";
        return false;
    }
    const size_t data_start = table + chunks.size() * 8;
    // Each chunk's size, plain, must fit an int as the format's own do.
    {
        int64_t bytes_per_pixel = 0;
        for (const Channel& ch : part.channels)
            bytes_per_pixel += type_size(ch.type);
        const int64_t rows = part.tiled ? part.tile_h : lines_per_chunk(part.compression);
        const int64_t cols = part.tiled ? std::min<int64_t>(part.tile_w, w) : w;
        if (bytes_per_pixel * std::min<int64_t>(rows, h) * cols > INT32_MAX)
        {
            error = "EXR chunks are too large";
            return false;
        }
    }

    // Every chunk found and its size checked before anything is allocated:
    // a cut-short file is caught here, and so is one claiming more than
    // its data could hold (zlib, the best of the codecs, packs at most
    // about 1032 to 1).
    for (size_t i = 0; i < chunks.size(); ++i)
    {
        Chunk& c = chunks[i];
        const uint64_t off = le64(d + table + i * 8);
        const size_t head = (multipart ? 4 : 0) + (part.tiled ? 20 : 8);
        if (off < data_start || off >= size || size - off < head)
        {
            error = "EXR chunk offset is out of range (file cut short?)";
            return false;
        }
        const uint8_t* p = d + off;
        if (multipart)
        {
            if (le32s(p) != chosen)
            {
                error = "EXR chunk belongs to another part";
                return false;
            }
            p += 4;
        }
        if (part.tiled)
        {
            if (int64_t(le32s(p)) * part.tile_w != c.box.x0 - dw.x0 || int64_t(le32s(p + 4)) * part.tile_h != c.box.y0 - dw.y0 || le32s(p + 8) ||
                le32s(p + 12))
            {
                error = "EXR tile is out of place";
                return false;
            }
            p += 16;
        }
        else
        {
            if (le32s(p) != c.box.y0)
            {
                error = "EXR scanline block is out of place";
                return false;
            }
            p += 4;
        }
        const int32_t n = le32s(p);
        p += 4;
        c.plain = plain_size(c);
        if (n < 0 || size_t(n) > size_t(d + size - p) || c.plain / 1100 > size_t(n))
        {
            error = "EXR chunk is cut short";
            return false;
        }
        c.data = p;
        c.size = size_t(n);
    }

    Selection sel;
    std::vector<std::string> layers;
    if (!select_channels(part, sel, layers, error))
        return false;
    bool all_half = true, all_uint = true;
    for (size_t i = 0; i < part.channels.size(); ++i)
        if (sel.dest[i] >= 0)
        {
            all_half &= part.channels[i].type == kHalf;
            all_uint &= part.channels[i].type == kUint;
        }
    using S = SourceData::Sample;
    auto src = std::make_shared<SourceData>(all_half ? S::F16 : S::F32, sel.channels, w, h, true);
    if (sel.missing)
        std::memset(src->data.data(), 0, src->data.size());

    // Chunks decode on their own, so they're shared between threads.
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::string first_error;
    auto fail = [&](std::string e)
    {
        std::lock_guard lock(error_mutex);
        if (!failed.exchange(true))
            first_error = std::move(e);
    };
    auto work = [&]
    {
        Scratch s;
        std::string e;
        while (!failed)
        {
            const size_t i = next++;
            if (i >= chunks.size())
                return;
            const Chunk& c = chunks[i];
            if (!decompress(c, s, e))
            {
                fail(e);
                return;
            }
            extract(c, s.plain.data(), sel, dw, *src);
            if (src->has_alpha())
                unpremultiply(*src, c.box.x0 - dw.x0, c.box.y0 - dw.y0, c.box.x1 - dw.x0, c.box.y1 - dw.y0);
        }
    };
    const int threads = int(std::clamp<size_t>(std::thread::hardware_concurrency(), 1, std::min<size_t>(8, chunks.size())));
    if (threads <= 1 || int64_t(w) * h < 256 * 256)
        work();
    else
    {
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back(work);
        for (std::thread& t : pool)
            t.join();
    }
    if (failed)
    {
        error = first_error;
        return false;
    }
    out.set_source(std::move(src));

    out.format = "EXR";
    out.bits_per_sample = all_half ? 16 : 32;
    using M = PixelFormat::Model;
    PixelFormat& pf = out.pixel_format;
    pf.model = sel.channels >= 3 ? M::RGB : M::L;
    {
        const char* names = sel.channels == 1 ? "L" : sel.channels == 2 ? "LA" : sel.channels == 3 ? "RGB" : "RGBA";
        std::vector<int> bits(size_t(sel.channels), all_half ? 16 : 32);
        for (size_t i = 0; i < part.channels.size(); ++i)
            if (sel.dest[i] >= 0)
                bits[size_t(sel.dest[i])] = type_size(part.channels[i].type) * 8;
        for (int k = 0; k < sel.channels; ++k)
            pf.channels.push_back({names[k], bits[size_t(k)]});
        pf.bits_per_pixel = 0;
    }
    pf.sample = all_uint ? PixelFormat::Sample::Unsigned : PixelFormat::Sample::Float;
    pf.premultiplied = sel.channels == 2 || sel.channels == 4;
    if (part.compression != kNone)
        pf.notes.push_back(std::string(compression_name(part.compression)) + " compression");
    if (part.tiled)
        pf.notes.push_back("tiled");
    if (part.has_chromaticities)
        out.color_space = colour_space(part.chromaticities);

    out.add("EXR", "Compression", compression_name(part.compression));
    {
        // "half" when they're all one type, else each channel's.
        bool uniform = true;
        for (const Channel& ch : part.channels)
            uniform &= ch.type == part.channels[0].type;
        std::string types, list;
        for (const Channel& ch : part.channels)
        {
            types += (types.empty() ? "" : ", ") + ch.name + " " + type_name(ch.type);
            list += (list.empty() ? "" : ", ") + ch.name;
            if (ch.xs != 1 || ch.ys != 1)
                list += " (" + std::to_string(ch.xs) + "×" + std::to_string(ch.ys) + " sampling)";
        }
        out.add("EXR", "Pixel types", uniform ? type_name(part.channels[0].type) : types);
        out.add("EXR", "Channels", list);
    }
    if (!layers.empty())
    {
        std::string text;
        for (const std::string& l : layers)
            text += (text.empty() ? "" : ", ") + l;
        out.add("EXR", "Layers", text);
    }
    if (!sel.shown.empty())
        out.add("EXR", "Shown", sel.shown);
    if (part.has_display_window && !(part.display_window == dw))
    {
        out.add("EXR", "Data window", box_text(dw));
        out.add("EXR", "Display window", box_text(part.display_window));
    }
    else
        out.add("EXR", "Window", box_text(dw));
    static const char* const orders[] = {"increasing y", "decreasing y", "random y"};
    out.add("EXR", "Line order", part.line_order >= 0 && part.line_order < 3 ? orders[part.line_order] : std::to_string(part.line_order));
    if (part.tiled)
    {
        static const char* const modes[] = {"one level", "mipmap, level 0 shown", "ripmap, level 0 shown"};
        out.add("EXR", "Tiles", std::to_string(part.tile_w) + "×" + std::to_string(part.tile_h) + ", " +
                                    (part.level_mode < 3 ? modes[part.level_mode] : "unknown levels"));
    }
    else
        out.add("EXR", "Storage", "scanlines");
    if (multipart)
    {
        std::string names;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            const std::string n = parts[i].name.empty() ? "unnamed" : parts[i].name;
            names += (i ? ", " : "") + n + (parts[i].type == "deepscanline" || parts[i].type == "deeptile" ? " (deep)" : "");
        }
        out.add("EXR", "Parts", std::to_string(parts.size()) + ": " + names);
        out.add("EXR", "Part shown", part.name.empty() ? std::to_string(chosen + 1) : part.name);
    }
    if (part.has_chromaticities)
    {
        const float* c = part.chromaticities;
        char s[200];
        std::snprintf(s, sizeof s, "red %.4g, %.4g · green %.4g, %.4g · blue %.4g, %.4g · white %.4g, %.4g", double(c[0]), double(c[1]), double(c[2]),
                      double(c[3]), double(c[4]), double(c[5]), double(c[6]), double(c[7]));
        out.add("EXR", "Chromaticities", s);
    }
    for (MetadataItem& m : part.attributes)
        out.add("EXR", std::move(m.key), std::move(m.value));
    return true;
}

}  // namespace

const Decoder& exr_decoder()
{
    static const Decoder d{"EXR", {"exr"}, sniff, decode};
    return d;
}
