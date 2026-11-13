// Copyright (C) 2015-2025 Jonathan Müller and foonathan/memory contributors
// SPDX-License-Identifier: Zlib

#ifndef FOONATHAN_MEMORY_DETAIL_ASAN_HPP_INCLUDED
#define FOONATHAN_MEMORY_DETAIL_ASAN_HPP_INCLUDED

#include <cstddef>

#include "../config.hpp"

// AddressSanitizer support for the sub-allocations of memory_stack.
//
// The functions below are no-ops unless FOONATHAN_MEMORY_ASAN is enabled in the
// generated configuration header. In that case the translation unit including
// this header must have been compiled with AddressSanitizer; this is checked
// here so that a mismatching library/user build fails loudly instead of ending
// up with unresolved ASan symbols or silently disabled checking.
#if FOONATHAN_MEMORY_ASAN
#    if defined(__has_feature)
#        if __has_feature(address_sanitizer)
#            define FOONATHAN_MEMORY_IMPL_HAS_ASAN 1
#        endif
#    endif
#    if defined(__SANITIZE_ADDRESS__)
#        define FOONATHAN_MEMORY_IMPL_HAS_ASAN 1
#    endif
#    if !defined(FOONATHAN_MEMORY_IMPL_HAS_ASAN)
#        error "FOONATHAN_MEMORY_ASAN is enabled, but this translation unit is not compiled with AddressSanitizer"
#    endif
#    include <sanitizer/asan_interface.h>
#endif

namespace foonathan
{
    namespace memory
    {
        namespace detail
        {
            // overflow-aware addition
            inline bool checked_add(std::size_t a, std::size_t b, std::size_t& result) noexcept
            {
                if (a > std::size_t(-1) - b)
                    return false;
                result = a + b;
                return true;
            }

            // saturating addition usable in constant expressions
            constexpr std::size_t saturating_add(std::size_t a, std::size_t b) noexcept
            {
                return a > std::size_t(-1) - b ? std::size_t(-1) : a + b;
            }

#if FOONATHAN_MEMORY_ASAN
            // overflow-aware multiplication, used to validate array sizes
            inline bool checked_mul(std::size_t a, std::size_t b, std::size_t& result) noexcept
            {
                if (a != 0u && b > std::size_t(-1) / a)
                    return false;
                result = a * b;
                return true;
            }

            // The AddressSanitizer shadow memory has a granularity of 8 bytes.
            // Its encoding can express "the first k bytes of a granule are accessible"
            // for the last granule of an unpoisoned region, so an exact unpoison of an
            // aligned region leaves all following bytes inaccessible. Poisoning is
            // only ever done on regions that consist of whole granules: the caller
            // keeps all stack tops and block sizes granule-aligned, which also avoids
            // touching the redzone of the underlying allocation.
            constexpr std::size_t asan_granularity = 8u;

            // marks [memory, memory + size) as inaccessible;
            // memory and size must be multiples of the granularity
            inline void asan_poison_aligned(void* memory, std::size_t size) noexcept
            {
                if (size != 0u)
                    __asan_poison_memory_region(memory, size);
            }

            // marks exactly [memory, memory + size) as accessible;
            // memory must be a multiple of the granularity, size need not be
            inline void asan_unpoison(void* memory, std::size_t size) noexcept
            {
                if (size != 0u)
                    __asan_unpoison_memory_region(memory, size);
            }
#else
            constexpr std::size_t asan_granularity = 1u;

            inline void asan_poison_aligned(void*, std::size_t) noexcept {}
            inline void asan_unpoison(void*, std::size_t) noexcept {}
#endif
        } // namespace detail
    } // namespace memory
} // namespace foonathan

#endif // FOONATHAN_MEMORY_DETAIL_ASAN_HPP_INCLUDED
