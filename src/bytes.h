#pragma once
// Integers as file formats store them, little- or big-endian, from bytes the
// caller has checked are there.

#include <cstddef>
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
// Eight at once (one load, swapped: a bit reader's).
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

// A stream's bits, most significant first, `size` bytes of them; past the
// end, zeros. Up to 64 in hand (`held`), from the top of `peek`, zeros
// under them.
class MsbBits
{
public:
    MsbBits(const uint8_t* data, uint64_t size) : data_(data), size_(size) {}
    // At least 57 in hand.
    void fill()
    {
        if (at_ + 8 <= size_)
        {
            cache_ |= be64(data_ + at_) >> count_;
            at_ += size_t(63 - count_) >> 3;
            count_ |= 56;
            cache_ &= ~uint64_t(0) << (64 - count_);  // the next byte's first bits not yet counted
            return;
        }
        for (; count_ <= 56; count_ += 8, ++at_)
            if (at_ < size_)
                cache_ |= uint64_t(data_[at_]) << (56 - count_);
    }
    int held() const { return count_; }
    uint64_t peek() const { return cache_; }
    // `n` (0..32) of those in hand.
    uint32_t take(int n)
    {
        const uint32_t v = n ? uint32_t(cache_ >> (64 - n)) : 0;
        skip(n);
        return v;
    }
    void skip(int n)
    {
        cache_ = n < 64 ? cache_ << n : 0;
        count_ -= n;
    }
    // Whether every byte's been taken into hand.
    bool drained() const { return at_ >= size_; }

private:
    const uint8_t* data_;
    uint64_t size_, at_ = 0;
    uint64_t cache_ = 0;
    int count_ = 0;
};
