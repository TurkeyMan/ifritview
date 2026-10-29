#pragma once
// Integers as file formats store them, little- or big-endian, from bytes the
// caller has checked are there.

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#include <stdlib.h>
#endif

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
// A bit reader's eight bytes at once (one load, swapped).
inline uint64_t be64(const uint8_t* p)
{
    uint64_t v;
    std::memcpy(&v, p, 8);
#ifdef _MSC_VER
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
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
