// Copyright (C) 2015-2025 Jonathan Müller and foonathan/memory contributors
// SPDX-License-Identifier: Zlib

#ifndef FOONATHAN_MEMORY_DETAIL_ASAN_HPP_INCLUDED
#define FOONATHAN_MEMORY_DETAIL_ASAN_HPP_INCLUDED

/// \file
/// Narrow AddressSanitizer support used to give the sub-allocations of a stack
/// allocator a real poison lifecycle.
///
/// When \ref FOONATHAN_MEMORY_ASAN is enabled the whole translation unit (the
/// library and every user that includes the generated configuration) is compiled
/// with AddressSanitizer, so the interface declared below is provided by the
/// compiler's own sanitizer runtime.  The helpers here never introduce an ASan
/// symbol dependency when the option is disabled and they do not disable
/// sanitizer checking for any production allocation; they only touch the shadow
/// memory of the exact ranges the stack allocator is managing.

#include "../config.hpp"

#if FOONATHAN_MEMORY_ASAN
#include <sanitizer/asan_interface.h>
#include <cstddef>
#endif

namespace foonathan
{
    namespace memory
    {
        namespace detail
        {
#if FOONATHAN_MEMORY_ASAN
            // The AddressSanitizer shadow granularity.  Every shadow byte describes
            // exactly this many application bytes, so all ranges handed to the
            // poisoning primitives have to be reasoned about at this resolution.
            constexpr std::size_t asan_granularity = 8u;

            // Poisons a fully addressable range.  The range must start at an
            // 8-byte boundary and its length must be a multiple of the granularity;
            // partial-granule poisoning is not reliable across the supported
            // sanitizer runtimes and is therefore never used.
            inline void asan_poison_aligned(const void* addr, std::size_t size) noexcept
            {
                if (size != 0u)
                    __asan_poison_memory_region(addr, size);
            }

            // Makes exactly `size` bytes accessible starting at an 8-byte
            // boundary.  Aligned unpoisoning encodes a partial tail in a single
            // shadow byte, so the bytes behind a short object stay poisoned even
            // when they share a granule with the object.  This is the primitive
            // that allows two neighbouring objects to be handled independently.
            inline void asan_unpoison_aligned(const void* addr, std::size_t size) noexcept
            {
                if (size != 0u)
                    __asan_unpoison_memory_region(addr, size);
            }

            // Temporarily restores access to a poisoned range for internal book
            // keeping (debug fills, reading an intrusive management header).  The
            // object re-applies the poisoning in its destructor, so the window is
            // limited to the lexical scope that created it.
            class asan_access_guard
            {
            public:
                asan_access_guard(const void* addr, std::size_t size) noexcept
                : addr_(addr), size_(size)
                {
                    if (size_ != 0u)
                        __asan_unpoison_memory_region(addr_, size_);
                }

                ~asan_access_guard() noexcept
                {
                    if (size_ != 0u)
                        __asan_poison_memory_region(addr_, size_);
                }

                asan_access_guard(const asan_access_guard&) = delete;
                asan_access_guard& operator=(const asan_access_guard&) = delete;

            private:
                const void* addr_;
                std::size_t size_;
            };

            // Scoped access to a whole arena block while the library fills it or
            // hands it back to the underlying allocator.
            //
            // Two situations occur at the call sites:
            //  * a block owned by an allocator that does not sub-poison (the
            //    pools) is fully accessible, so the probe finds nothing and the
            //    guard is a no-op - those allocators stay fully instrumented
            //    instead of being blanket-excluded from sanitizer checking;
            //  * a stack block is inaccessible (a cached block) or a mix of
            //    accessible objects and poisoned redzones (a block unwound across
            //    a marker).  The guard briefly opens the whole region for the
            //    internal fill and then re-poisons the managed prefix, which is
            //    exactly the "entire cached user region is inaccessible" state an
            //    unwound block must have, since unwind killed every object in it.
            class asan_block_access_guard
            {
            public:
                asan_block_access_guard(const void* addr, std::size_t size) noexcept
                : addr_(addr), size_(size), restore_(false)
                {
                    if (size_ != 0u
                        && __asan_region_is_poisoned(const_cast<void*>(addr_), size_) != nullptr)
                    {
                        restore_ = true;
                        __asan_unpoison_memory_region(addr_, size_);
                    }
                }

                ~asan_block_access_guard() noexcept
                {
                    if (restore_)
                    {
                        // Re-poison the fully covered granules; block bases are
                        // granularity aligned and the stack keeps every cursor at a
                        // granularity boundary, so this restores the cache state.
                        __asan_poison_memory_region(addr_,
                                                    size_ & ~(asan_granularity - 1u));
                    }
                }

                asan_block_access_guard(const asan_block_access_guard&) = delete;
                asan_block_access_guard& operator=(const asan_block_access_guard&) = delete;

            private:
                const void* addr_;
                std::size_t size_;
                bool        restore_;
            };

            // One-way access restoration used immediately before a block leaves
            // the arena for good (shrink_to_fit/destruction).  The underlying
            // allocator must be able to touch the full range to free it, so the
            // previous poisoning is not re-applied.
            inline void asan_unpoison_for_release(const void* addr, std::size_t size) noexcept
            {
                if (size != 0u)
                    __asan_unpoison_memory_region(addr, size);
            }

            // Adds two non-negative sizes and reports overflow instead of wrapping.
            inline bool asan_add_overflow(std::size_t a, std::size_t b,
                                          std::size_t& result) noexcept
            {
                result = a + b;
                return result < a;
            }
#else
            constexpr std::size_t asan_granularity = 1u;

            inline void asan_poison_aligned(const void*, std::size_t) noexcept {}
            inline void asan_unpoison_aligned(const void*, std::size_t) noexcept {}
            inline void asan_unpoison_for_release(const void*, std::size_t) noexcept {}

            inline bool asan_add_overflow(std::size_t a, std::size_t b,
                                          std::size_t& result) noexcept
            {
                result = a + b;
                return result < a;
            }

            class asan_access_guard
            {
            public:
                asan_access_guard(const void*, std::size_t) noexcept {}
            };

            class asan_block_access_guard
            {
            public:
                asan_block_access_guard(const void*, std::size_t) noexcept {}
            };
#endif
        } // namespace detail
    } // namespace memory
} // namespace foonathan

#endif // FOONATHAN_MEMORY_DETAIL_ASAN_HPP_INCLUDED
