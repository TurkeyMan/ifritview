#pragma once

#include "platform.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

// Allocator for big pixel buffers. Two things a std::vector does by default
// cost real time on a 48 MB image: zero-filling it (every decoder overwrites
// it all anyway), and faulting it in 4 KB pages on first write (~12,000
// faults). Here, construction leaves the bytes alone, and buffers over 4 MB
// are 2 MB aligned and marked for transparent huge pages.
template <typename T>
struct PixelAllocator
{
    using value_type = T;
    PixelAllocator() = default;
    template <typename U>
    PixelAllocator(const PixelAllocator<U>&) noexcept
    {
    }

    T* allocate(std::size_t n)
    {
        if (void* p = platform::allocate_pixels(n * sizeof(T)))
            return static_cast<T*>(p);
        throw std::bad_alloc();
    }
    void deallocate(T* p, std::size_t) noexcept { platform::free_pixels(p); }

    // Default-initialise: no zero fill for resize()/construction.
    template <typename U>
    void construct(U* p) noexcept
    {
        ::new (static_cast<void*>(p)) U;
    }
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args)
    {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    bool operator==(const PixelAllocator<U>&) const noexcept
    {
        return true;
    }
};

using PixelBuffer = std::vector<uint8_t, PixelAllocator<uint8_t>>;
using LinearBuffer = std::vector<uint16_t, PixelAllocator<uint16_t>>;
using HalfBuffer = std::vector<uint16_t, PixelAllocator<uint16_t>>;  // IEEE half floats (half.h)
