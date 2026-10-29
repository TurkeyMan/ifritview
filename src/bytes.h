#pragma once
// Integers as file formats store them, little- or big-endian, from bytes the
// caller has checked are there.

#include <cstddef>
#include <cstdint>
#include <cstring>

inline uint16_t le16(const uint8_t* p)
{
    return uint16_t(p[0] | p[1] << 8);
}
inline uint32_t le24(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
}
inline uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t le64(const uint8_t* p)
{
    return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32;
}
inline uint16_t be16(const uint8_t* p)
{
    return uint16_t(p[0] << 8 | p[1]);
}
inline uint32_t be32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
inline uint64_t be64(const uint8_t* p)
{
    return uint64_t(be32(p)) << 32 | be32(p + 4);
}
// Either order, as a file's header says.
inline uint16_t rd16(const uint8_t* p, bool big_endian)
{
    return big_endian ? be16(p) : le16(p);
}
inline uint32_t rd32(const uint8_t* p, bool big_endian)
{
    return big_endian ? be32(p) : le32(p);
}

// A file's bytes `n` long, in its order; what lies past its end reads 0.
struct FileReader
{
    const uint8_t* p;
    size_t n;
    bool big_endian;
    bool fits(size_t at, size_t len) const { return at <= n && len <= n - at; }
    uint16_t u16(size_t at) const { return fits(at, 2) ? rd16(p + at, big_endian) : 0; }
    uint32_t u32(size_t at) const { return fits(at, 4) ? rd32(p + at, big_endian) : 0; }
    float f32(size_t at) const
    {
        const uint32_t v = u32(at);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    double f64(size_t at) const
    {
        const uint64_t v = fits(at, 8) ? big_endian ? be64(p + at) : le64(p + at) : 0;
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
};
