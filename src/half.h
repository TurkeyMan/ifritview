#pragma once
// IEEE 754 half-precision floats (binary16), as stored in SourceData and in
// half-float image formats: conversion to and from float.

#include <cstdint>
#include <cstring>

inline float half_to_float(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0)
    {
        if (mant == 0)
            bits = sign;
        else
        {
            // Subnormal: normalise it.
            int e = -1;
            uint32_t m = mant;
            do
            {
                ++e;
                m <<= 1;
            } while (!(m & 0x400));
            bits = sign | uint32_t(127 - 15 - e) << 23 | (m & 0x3FF) << 13;
        }
    }
    else if (exp == 31)
        bits = sign | 0x7F800000u | mant << 13;  // infinity or NaN
    else
        bits = sign | (exp + 127 - 15) << 23 | mant << 13;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Rounded to nearest; overflow goes to infinity, NaN stays NaN.
inline uint16_t float_to_half(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
    const uint32_t abs = bits & 0x7FFFFFFF;
    if (abs >= 0x7F800000u)
        return uint16_t(sign | 0x7C00 | (abs > 0x7F800000u ? 0x200 : 0));
    if (abs >= 0x477FF000u)  // rounds past 65504
        return uint16_t(sign | 0x7C00);
    if (abs < 0x38800000u)
    {
        // Subnormal or zero in half: shift the mantissa, with the implicit bit, into place.
        if (abs < 0x33000000u)
            return sign;
        const int shift = 126 - int(abs >> 23);  // 14..24
        const uint32_t m = (abs & 0x7FFFFF) | 0x800000;
        uint32_t h = m >> shift;
        const uint32_t rest = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (h & 1)))
            ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = ((abs - 0x38000000u) >> 13);
    const uint32_t rest = abs & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1)))
        ++h;
    return uint16_t(sign | h);
}
