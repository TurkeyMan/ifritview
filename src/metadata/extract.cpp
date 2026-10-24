// Finding the stored metadata in container formats: JPEG's APP segments,
// PNG's chunks, WebP's RIFF chunks and JPEG XL's boxes. Every length and
// offset is checked against the buffer; anything malformed is skipped.
#include "file_metadata.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace
{

uint32_t be16(const uint8_t* p)
{
    return uint32_t(p[0]) << 8 | p[1];
}

uint32_t be32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool starts_with(const uint8_t* d, size_t n, const char* prefix, size_t len)
{
    return n >= len && std::memcmp(d, prefix, len) == 0;
}

// An EXIF blob as a TIFF structure: some writers keep JPEG's "Exif\0\0".
std::vector<uint8_t> tiff_of(const uint8_t* d, size_t n)
{
    if (starts_with(d, n, "Exif\0\0", 6))
        d += 6, n -= 6;
    if (n < 8 || !(starts_with(d, n, "II*\0", 4) || starts_with(d, n, "MM\0*", 4)))
        return {};
    return std::vector<uint8_t>(d, d + n);
}

// zlib data, up to a sane size (a profile or a packet, not a bomb).
std::vector<uint8_t> inflate_all(const uint8_t* d, size_t n)
{
    constexpr size_t kLimit = size_t(64) << 20;
    z_stream z{};
    if (inflateInit(&z) != Z_OK)
        return {};
    std::vector<uint8_t> out(std::max<size_t>(n * 4, 4096));
    z.next_in = const_cast<Bytef*>(d);
    z.avail_in = uInt(n);
    int r = Z_OK;
    while (r == Z_OK)
    {
        if (z.total_out == out.size())
        {
            if (out.size() >= kLimit)
                break;
            out.resize(std::min(out.size() * 2, kLimit));
        }
        z.next_out = out.data() + z.total_out;
        z.avail_out = uInt(out.size() - z.total_out);
        r = inflate(&z, Z_NO_FLUSH);
    }
    out.resize(r == Z_STREAM_END ? z.total_out : 0);
    inflateEnd(&z);
    return out;
}

// Photoshop's image resource blocks ("8BIM"): the IPTC is resource 0x0404.
std::vector<uint8_t> iptc_from_irb(const uint8_t* d, size_t n)
{
    size_t p = 0;
    while (p + 12 <= n && std::memcmp(d + p, "8BIM", 4) == 0)
    {
        const uint32_t id = be16(d + p + 4);
        const size_t name = size_t(d[p + 6]) + 1;
        p += 6 + name + (name & 1);  // the Pascal name, padded to even
        if (p + 4 > n)
            break;
        const size_t len = be32(d + p);
        p += 4;
        if (len > n - p)
            break;
        if (id == 0x0404)
            return std::vector<uint8_t>(d + p, d + p + len);
        p += len + (len & 1);
    }
    return {};
}

// ImageMagick's "Raw profile type ..." text: a line naming it, the length,
// then the bytes in hex.
std::vector<uint8_t> raw_profile(const uint8_t* d, size_t n)
{
    size_t p = 0, lines = 0;
    while (p < n && lines < 3)
        if (d[p++] == '\n')
            ++lines;
    std::vector<uint8_t> out;
    int high = -1;
    for (; p < n; ++p)
    {
        const char c = char(d[p]);
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            continue;
        if (high < 0)
            high = v;
        else
        {
            out.push_back(uint8_t(high << 4 | v));
            high = -1;
        }
    }
    return out;
}

void from_jpeg(const uint8_t* d, size_t n, FileMetadata& m)
{
    std::vector<std::vector<uint8_t>> icc_chunks;
    size_t p = 2;
    while (p + 4 <= n)
    {
        if (d[p] != 0xFF)
            break;
        const uint8_t marker = d[p + 1];
        if (marker == 0xFF)
        {
            ++p;  // fill byte
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA)  // end, or the scan: no metadata after
            break;
        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01)
        {
            p += 2;
            continue;
        }
        const size_t len = be16(d + p + 2);
        if (len < 2 || p + 2 + len > n)
            break;
        const uint8_t* s = d + p + 4;
        const size_t sn = len - 2;
        if (marker == 0xE1 && m.exif.empty() && starts_with(s, sn, "Exif\0\0", 6))
            m.exif = tiff_of(s, sn);
        else if (marker == 0xE1 && m.xmp.empty() && starts_with(s, sn, "http://ns.adobe.com/xap/1.0/\0", 29))
            m.xmp.assign(s + 29, s + sn);
        else if (marker == 0xE2 && starts_with(s, sn, "ICC_PROFILE\0", 12) && sn > 14)
        {
            const size_t seq = s[12], count = s[13];
            if (seq >= 1 && seq <= count)
            {
                icc_chunks.resize(std::max(icc_chunks.size(), count));
                icc_chunks[seq - 1].assign(s + 14, s + sn);
            }
        }
        else if (marker == 0xED && m.iptc.empty() && starts_with(s, sn, "Photoshop 3.0\0", 14))
            m.iptc = iptc_from_irb(s + 14, sn - 14);
        p += 2 + len;
    }
    if (!icc_chunks.empty() && std::none_of(icc_chunks.begin(), icc_chunks.end(), [](const auto& c) { return c.empty(); }))
        for (const auto& c : icc_chunks)
            m.icc.insert(m.icc.end(), c.begin(), c.end());
}

void from_png(const uint8_t* d, size_t n, FileMetadata& m)
{
    size_t p = 8;
    while (p + 12 <= n)
    {
        const size_t len = be32(d + p);
        if (len > n - p - 12)
            break;
        const uint8_t* type = d + p + 4;
        const uint8_t* s = d + p + 8;
        const auto is = [type](const char* t) { return std::memcmp(type, t, 4) == 0; };
        if (is("IEND"))
            break;
        const uint8_t* nul = static_cast<const uint8_t*>(std::memchr(s, 0, len));
        const size_t key_len = nul ? size_t(nul - s) : len;
        const std::string key(reinterpret_cast<const char*>(s), key_len);
        if (is("eXIf") && m.exif.empty())
            m.exif = tiff_of(s, len);
        else if (is("iCCP") && m.icc.empty() && nul && key_len + 2 <= len)
            m.icc = inflate_all(s + key_len + 2, len - key_len - 2);
        else if (is("iTXt") && m.xmp.empty() && key == "XML:com.adobe.xmp" && nul && key_len + 3 <= len)
        {
            // keyword, compressed?, method, language, translated keyword, text
            const bool compressed = s[key_len + 1] != 0;
            size_t q = key_len + 3;
            for (int skip = 0; skip < 2 && q < len; ++q)
                if (s[q] == 0)
                    ++skip;
            if (q <= len)
                m.xmp = compressed ? inflate_all(s + q, len - q) : std::vector<uint8_t>(s + q, s + len);
        }
        else if ((is("zTXt") || is("tEXt")) && nul && key.rfind("Raw profile type ", 0) == 0)
        {
            const std::vector<uint8_t> text = is("zTXt") ? (key_len + 2 <= len ? inflate_all(s + key_len + 2, len - key_len - 2) : std::vector<uint8_t>())
                                                         : std::vector<uint8_t>(s + key_len + 1, s + len);
            const std::vector<uint8_t> bytes = raw_profile(text.data(), text.size());
            const std::string kind = key.substr(17);
            if ((kind == "exif" || kind == "APP1") && m.exif.empty())
                m.exif = tiff_of(bytes.data(), bytes.size());
            else if (kind == "iptc" && m.iptc.empty())
                m.iptc = starts_with(bytes.data(), bytes.size(), "8BIM", 4) ? iptc_from_irb(bytes.data(), bytes.size()) : bytes;
            else if (kind == "xmp" && m.xmp.empty())
                m.xmp = bytes;
            else if (kind == "icc" && m.icc.empty())
                m.icc = bytes;
        }
        p += 12 + len;
    }
}

void from_webp(const uint8_t* d, size_t n, FileMetadata& m)
{
    size_t p = 12;
    while (p + 8 <= n)
    {
        const size_t len = le32(d + p + 4);
        if (len > n - p - 8)
            break;
        const uint8_t* s = d + p + 8;
        if (std::memcmp(d + p, "EXIF", 4) == 0 && m.exif.empty())
            m.exif = tiff_of(s, len);
        else if (std::memcmp(d + p, "XMP ", 4) == 0 && m.xmp.empty())
            m.xmp.assign(s, s + len);
        else if (std::memcmp(d + p, "ICCP", 4) == 0 && m.icc.empty())
            m.icc.assign(s, s + len);
        p += 8 + len + (len & 1);
    }
}

void from_jxl_boxes(const uint8_t* d, size_t n, FileMetadata& m)
{
    size_t p = 0;
    while (p + 8 <= n)
    {
        uint64_t size = be32(d + p), header = 8;
        if (size == 1)
        {
            if (p + 16 > n)
                break;
            size = uint64_t(be32(d + p + 8)) << 32 | be32(d + p + 12);
            header = 16;
        }
        else if (size == 0)
            size = n - p;
        if (size < header || size > n - p)
            break;
        const uint8_t* s = d + p + header;
        const size_t len = size_t(size - header);
        if (std::memcmp(d + p + 4, "Exif", 4) == 0 && m.exif.empty() && len >= 4)
        {
            const size_t offset = be32(s);  // to the TIFF header
            if (offset <= len - 4)
                m.exif = tiff_of(s + 4 + offset, len - 4 - offset);
        }
        else if (std::memcmp(d + p + 4, "xml ", 4) == 0 && m.xmp.empty())
            m.xmp.assign(s, s + len);
        p += size_t(size);
    }
}

}  // namespace

void FileMetadata::merge(const FileMetadata& other)
{
    if (exif.empty())
        exif = other.exif;
    if (xmp.empty())
        xmp = other.xmp;
    if (iptc.empty())
        iptc = other.iptc;
    if (icc.empty())
        icc = other.icc;
}

FileMetadata extract_metadata(const uint8_t* d, size_t n)
{
    FileMetadata m;
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8)
        from_jpeg(d, n, m);
    else if (starts_with(d, n, "\x89PNG\r\n\x1a\n", 8))
        from_png(d, n, m);
    else if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0)
        from_webp(d, n, m);
    else if (starts_with(d, n, "\0\0\0\x0CJXL \r\n\x87\n", 12))
        from_jxl_boxes(d, n, m);
    return m;
}
