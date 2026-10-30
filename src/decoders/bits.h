#pragma once

#include "../bytes.h"

#include <bit>
#include <cstddef>
#include <cstdint>

// A code read most significant bit first (a CR3's subbands, a Fuji RAF's
// strips); past its end, zeros, as LibRaw reads no further than its size.
class Bits
{
public:
    Bits(const uint8_t* data, uint64_t size) : data_(data), size_(size) {}
    uint32_t bits(int n)  // 1..32
    {
        if (count_ < n)
            fill();
        const uint32_t v = uint32_t(cache_ >> (64 - n));
        cache_ <<= n;
        count_ -= n;
        return v;
    }
    // A code (as code() reads it) when its zeros, 1 and k bits are all in
    // hand, and fewer than `limit` zeros; else false, nothing taken.
    bool golomb(int k, uint32_t limit, uint32_t& v)
    {
        if (count_ < 32)
            fill();
        const int z = std::countl_zero(cache_);
        if (uint32_t(z) >= limit || z + 1 + k > count_)
            return false;
        const uint64_t c = cache_ << z << 1;
        cache_ = c << k;
        count_ -= z + 1 + k;
        v = uint32_t(z) << k | uint32_t(c >> 1 >> (63 - k));
        return true;
    }
    // The zeros before the next 1 (taken too). LibRaw's for a code running
    // past the end isn't defined; this gives up there.
    uint32_t zeros()
    {
        uint32_t n = 0;
        while (!cache_)
        {
            n += uint32_t(count_);
            count_ = 0;
            if (at_ >= size_)
                return n;
            fill();
        }
        const int z = std::countl_zero(cache_);
        cache_ <<= z;
        cache_ <<= 1;
        count_ -= z + 1;
        return n + uint32_t(z);
    }

private:
    // At least 57 bits in hand.
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
    const uint8_t* data_;
    uint64_t size_, at_ = 0;
    uint64_t cache_ = 0;  // `count_` bits from the top, zeros under them
    int count_ = 0;
};
