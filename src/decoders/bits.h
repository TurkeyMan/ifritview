#pragma once

#include "../bytes.h"

#include <bit>
#include <cstddef>
#include <cstdint>

// A code read most significant bit first (a CR3's subbands, a Fuji RAF's
// strips); past its end, zeros, as LibRaw reads no further than its size.
class Bits : MsbBits
{
public:
    using MsbBits::MsbBits;
    uint32_t bits(int n)  // 1..32
    {
        if (held() < n)
            fill();
        return take(n);
    }
    // A code (as code() reads it) when its zeros, 1 and k bits are all in
    // hand, and fewer than `limit` zeros; else false, nothing taken.
    bool golomb(int k, uint32_t limit, uint32_t& v)
    {
        if (held() < 32)
            fill();
        const int z = std::countl_zero(peek());
        if (uint32_t(z) >= limit || z + 1 + k > held())
            return false;
        skip(z + 1);
        v = uint32_t(z) << k | take(k);
        return true;
    }
    // The zeros before the next 1 (taken too). LibRaw's for a code running
    // past the end isn't defined; this gives up there.
    uint32_t zeros()
    {
        uint32_t n = 0;
        while (!peek())
        {
            n += uint32_t(held());
            skip(held());
            if (drained())
                return n;
            fill();
        }
        const int z = std::countl_zero(peek());
        skip(z + 1);
        return n + uint32_t(z);
    }
};
