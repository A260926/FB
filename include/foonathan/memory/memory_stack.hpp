// Copyright (C) 2015-2025 Jonathan Müller and foonathan/memory contributors
// SPDX-License-Identifier: Zlib

#ifndef FOONATHAN_MEMORY_MEMORY_STACK_HPP_INCLUDED
#define FOONATHAN_MEMORY_MEMORY_STACK_HPP_INCLUDED

/// \file
/// Class \ref foonathan::memory::memory_stack and its \ref foonathan::memory::allocator_traits specialization.

// Inform that foonathan::memory::memory_stack::min_block_size API is available
#define FOONATHAN_MEMORY_MEMORY_STACK_HAS_MIN_BLOCK_SIZE

#include <cstdint>
#include <type_traits>

#include "detail/assert.hpp"
#include "detail/asan.hpp"
#include "detail/memory_stack.hpp"
#include "config.hpp"
#include "error.hpp"
#include "memory_arena.hpp"

namespace foonathan
{
    namespace memory
    {
#if !defined(DOXYGEN)
        template <class Impl>
        class memory_stack;
#endif

        namespace detail
        {
            class stack_marker
            {
                std::size_t index;
                char*       top;
                const char* end;

                stack_marker(std::size_t i, const detail::fixed_memory_stack& s,
                             const char* e) noexcept
                : index(i), top(s.top()), end(e)
                {
                }

                friend bool operator==(const stack_marker& lhs, const stack_marker& rhs) noexcept
                {
                    if (lhs.index != rhs.index)
                        return false;
                    FOONATHAN_MEMORY_ASSERT_MSG(lhs.end == rhs.end, "you must not compare two "
                                                                    "stack markers from different "
                                                                    "stacks");
                    return lhs.top == rhs.top;
                }

                friend bool operator!=(const stack_marker& lhs, const stack_marker& rhs) noexcept
                {
                    return !(rhs == lhs);
                }

                friend bool operator<(const stack_marker& lhs, const stack_marker& rhs) noexcept
                {
                    if (lhs.index != rhs.index)
                        return lhs.index < rhs.index;
                    FOONATHAN_MEMORY_ASSERT_MSG(lhs.end == rhs.end, "you must not compare two "
                                                                    "stack markers from different "
                                                                    "stacks");
                    return lhs.top < rhs.top;
                }

                friend bool operator>(const stack_marker& lhs, const stack_marker& rhs) noexcept
                {
                    return rhs < lhs;
                }

                friend bool operator<=(const stack_marker& lhs, const stack_marker& rhs) noexcept
                {
                    return !(rhs < lhs);
                }

                friend bool operator>=(const stack_marker& lhs, const stack_marker& rhs) noexcept
                {
                    return !(lhs < rhs);
                }

                template <class Impl>
                friend class memory::memory_stack;
            };

            struct memory_stack_leak_handler
            {
                void operator()(std::ptrdiff_t amount);
            };
        } // namespace detail

        /// A stateful \concept{concept_rawallocator,RawAllocator} that provides stack-like (LIFO) allocations.
        /// It uses a \ref memory_arena with a given \c BlockOrRawAllocator defaulting to \ref growing_block_allocator to allocate huge blocks
        /// and saves a marker to the current top.
        /// Allocation simply moves this marker by the appropriate number of bytes and returns the pointer at the old marker position,
        /// deallocation is not directly supported, only setting the marker to a previously queried position.
        /// \ingroup allocator
        template <class BlockOrRawAllocator = default_allocator>
        class memory_stack
        : FOONATHAN_EBO(detail::default_leak_checker<detail::memory_stack_leak_handler>)
        {
        public:
            using allocator_type = make_block_allocator_t<BlockOrRawAllocator>;

            /// \returns The minimum block size required for a stack containing the given amount of memory.
            /// If a stack is created with the result of `min_block_size(n)`, the resulting capacity will be exactly `n`.
            /// \requires `byte_size` must be a positive number.
            /// \note Due to debug fence sizes, the actual amount of usable memory can vary.
            /// However, this is impossible to compute without knowing the exact allocation pattern before,
            /// so this is just a rough estimate.
            /// \note When `FOONATHAN_MEMORY_ASAN` is enabled, the estimate also covers the
            /// redzone and shadow-granule rounding overhead, so the capacity reported by
            /// \ref capacity_left() can be larger while fewer user bytes fit.
            static constexpr std::size_t min_block_size(std::size_t byte_size) noexcept
            {
#if FOONATHAN_MEMORY_ASAN
                // in ASan mode an allocation can need up to granularity - 1 bytes of
                // rounding plus a redzone of one granule; the capacity estimate is
                // deliberately generous
                return detail::saturating_add(
                    detail::memory_block_stack::implementation_offset(),
                    detail::saturating_add(byte_size, 2u * detail::asan_granularity));
#else
                return detail::memory_block_stack::implementation_offset() + byte_size;
#endif
            }

            /// \effects Creates it with a given initial block size and and other constructor arguments for the \concept{concept_blockallocator,BlockAllocator}.
            /// It will allocate the first block and sets the top to its beginning.
            /// \requires \c block_size must be at least \c min_block_size(1).
            template <typename... Args>
            explicit memory_stack(std::size_t block_size, Args&&... args)
            : arena_(block_size_with_asan(block_size), detail::forward<Args>(args)...),
              stack_(arena_.allocate_block().memory)
            {
#if FOONATHAN_MEMORY_ASAN
                arena_.set_asan_managed(true);
                // start on a granule boundary; the leading partial granule stays
                // inaccessible but is never handed out
                stack_ = detail::fixed_memory_stack(asan_align_up(stack_.top()));
                asan_poison_current_block();
#endif
            }

            /// \effects Allocates a memory block of given size and alignment.
            /// It simply moves the top marker.
            /// If there is not enough space on the current memory block,
            /// a new one will be allocated by the \concept{concept_blockallocator,BlockAllocator} or taken from a cache
            /// and used for the allocation.
            /// \returns A \concept{concept_node,node} with given size and alignment.
            /// \throws Anything thrown by the \concept{concept_blockallocator,BlockAllocator} on growth
            /// or \ref bad_allocation_size if \c size is too big.
            /// \requires \c size and \c alignment must be valid.
            void* allocate(std::size_t size, std::size_t alignment)
            {
#if FOONATHAN_MEMORY_ASAN
                return asan_allocate(size, alignment);
#else
                auto fence  = detail::debug_fence_size;
                auto offset = detail::align_offset(stack_.top() + fence, alignment);

                if (!stack_.top()
                    || fence + offset + size + fence > std::size_t(block_end() - stack_.top()))
                {
                    // need to grow
                    auto block = arena_.allocate_block();
                    stack_     = detail::fixed_memory_stack(block.memory);

                    // new alignment required for over-aligned types
                    offset = detail::align_offset(stack_.top() + fence, alignment);

                    auto needed = fence + offset + size + fence;
                    detail::check_allocation_size<bad_allocation_size>(needed, block.size, info());
                }

                return stack_.allocate_unchecked(size, offset);
#endif
            }

            /// \effects Allocates a memory block of given size and alignment,
            /// similar to \ref allocate().
            /// But it does not attempt a growth if the arena is empty.
            /// \returns A \concept{concept_node,node} with given size and alignment
            /// or `nullptr` if there wasn't enough memory available.
            void* try_allocate(std::size_t size, std::size_t alignment) noexcept
            {
#if FOONATHAN_MEMORY_ASAN
                return asan_try_allocate(size, alignment);
#else
                return stack_.allocate(block_end(), size, alignment);
#endif
            }

            /// The marker type that is used for unwinding.
            /// The exact type is implementation defined,
            /// it is only required that it is efficiently copyable
            /// and has all the comparision operators defined for two markers on the same stack.
            /// Two markers are equal, if they are copies or created from two `top()` calls without a call to `unwind()` or `allocate()`.
            /// A marker `a` is less than marker `b`, if after `a` was obtained, there was one or more call to `allocate()` and no call to `unwind()`.
            using marker = FOONATHAN_IMPL_DEFINED(detail::stack_marker);

            /// \returns A marker to the current top of the stack.
            marker top() const noexcept
            {
                return {arena_.size() - 1, stack_, block_end()};
            }

            /// \effects Unwinds the stack to a certain marker position.
            /// This sets the top pointer of the stack to the position described by the marker
            /// and has the effect of deallocating all memory allocated since the marker was obtained.
            /// If any memory blocks are unused after the operation,
            /// they are not deallocated but put in a cache for later use,
            /// call \ref shrink_to_fit() to actually deallocate them.
            /// \requires The marker must point to memory that is still in use and was the whole time,
            /// i.e. it must have been pointed below the top at all time.
            void unwind(marker m) noexcept
            {
                FOONATHAN_MEMORY_ASSERT(m <= top());
                detail::debug_check_pointer([&] { return m.index <= arena_.size() - 1; }, info(),
                                            m.top);

                if (std::size_t to_deallocate = (arena_.size() - 1) - m.index) // different index
                {
#if FOONATHAN_MEMORY_ASAN
                    // all blocks above the marker are dead in their entirety;
                    // poison them before they are put into the cache so that stale
                    // pointers into cached blocks are still caught by ASan
                    for (std::size_t i = 0; i != to_deallocate; ++i)
                    {
                        auto block = arena_.current_block();
                        if (asan_block_ok(static_cast<char*>(block.memory)))
                            detail::asan_poison_aligned(
                                block.memory,
                                asan_block_usable(static_cast<char*>(block.memory), block.size));
                        arena_.deallocate_block();
                    }
#else
                    arena_.deallocate_block();
                    for (std::size_t i = 1; i != to_deallocate; ++i)
                        arena_.deallocate_block();
#endif

                    detail::debug_check_pointer(
                        [&]
                        {
                            auto cur = arena_.current_block();
#if FOONATHAN_MEMORY_ASAN
                            return m.end
                                   == static_cast<char*>(cur.memory)
                                          + asan_block_usable(static_cast<char*>(cur.memory),
                                                              cur.size);
#else
                            return m.end == static_cast<char*>(cur.memory) + cur.size;
#endif
                        },
                        info(), m.top);

#if FOONATHAN_MEMORY_ASAN
                    // mark memory from new top to end of the block as freed;
                    // the arena header in front of the block stays accessible
                    asan_poison_dead(m.top, const_cast<char*>(m.end));
                    stack_ = detail::fixed_memory_stack(m.top);
#else
                    // mark memory from new top to end of the block as freed
                    detail::debug_fill_free(m.top, std::size_t(m.end - m.top), 0);
                    stack_ = detail::fixed_memory_stack(m.top);
#endif
                }
                else // same index
                {
                    detail::debug_check_pointer([&] { return stack_.top() >= m.top; }, info(),
                                                m.top);
#if FOONATHAN_MEMORY_ASAN
                    asan_poison_dead(m.top, stack_.top());
                    stack_.set_top(m.top);
#else
                    stack_.unwind(m.top);
#endif
                }
            }

            /// \effects \ref unwind() does not actually do any deallocation of blocks on the \concept{concept_blockallocator,BlockAllocator},
            /// unused memory is stored in a cache for later reuse.
            /// This function clears that cache.
            void shrink_to_fit() noexcept
            {
                arena_.shrink_to_fit();
            }

            /// \returns The amount of memory remaining in the current block.
            /// This is the number of bytes that are available for allocation
            /// before the cache or \concept{concept_blockallocator,BlockAllocator} needs to be used.
            std::size_t capacity_left() const noexcept
            {
                auto top = stack_.top();
                if (!top)
                    return 0u;
#if FOONATHAN_MEMORY_ASAN
                // only the usable, granule aligned part of the block can be used
                auto misalignment =
                    std::size_t(reinterpret_cast<std::uintptr_t>(top) % detail::asan_granularity);
                auto remaining = std::size_t(block_end() - top);
                if (misalignment >= remaining)
                    return 0u;
                return (remaining - misalignment) & ~(detail::asan_granularity - 1u);
#else
                return std::size_t(block_end() - top);
#endif
            }

            /// \returns The size of the next memory block after the current block is exhausted and the arena grows.
            /// This function just forwards to the \ref memory_arena.
            /// \note All of it is available for the stack to use, but due to fences and alignment buffers,
            /// this may not be the exact amount of memory usable for the user.
            std::size_t next_capacity() const noexcept
            {
                return arena_.next_block_size();
            }

            /// \returns A reference to the \concept{concept_blockallocator,BlockAllocator} used for managing the arena.
            /// \requires It is undefined behavior to move this allocator out into another object.
            allocator_type& get_allocator() noexcept
            {
                return arena_.get_allocator();
            }

        private:
            allocator_info info() const noexcept
            {
                return {FOONATHAN_MEMORY_LOG_PREFIX "::memory_stack", this};
            }

#if FOONATHAN_MEMORY_ASAN
            // asks the block allocator for a block whose user region is a whole number
            // of shadow granules, so poison/unpoison never crosses the allocation
            static std::size_t block_size_with_asan(std::size_t block_size) noexcept
            {
                std::size_t rounded;
                if (!detail::checked_add(block_size, detail::asan_granularity - 1u, rounded))
                    return block_size;
                return rounded & ~(detail::asan_granularity - 1u);
            }
#else
            static constexpr std::size_t block_size_with_asan(std::size_t block_size) noexcept
            {
                return block_size;
            }
#endif

            const char* block_end() const noexcept
            {
                auto block = arena_.current_block();
#if FOONATHAN_MEMORY_ASAN
                // only whole shadow granules are managed: the end is rounded down
                // to an absolute granule boundary
                auto end = reinterpret_cast<std::uintptr_t>(block.memory) + block.size;
                end &= ~static_cast<std::uintptr_t>(detail::asan_granularity - 1u);
                return reinterpret_cast<const char*>(end);
#else
                return static_cast<const char*>(block.memory) + block.size;
#endif
            }

#if FOONATHAN_MEMORY_ASAN
            // Aligns a stack top (an absolute address) up to the next shadow granule
            // boundary, so all allocations start on a granule boundary independently
            // of the block's alignment.
            static char* asan_align_up(char* top) noexcept
            {
                auto value = reinterpret_cast<std::uintptr_t>(top);
                value      = (value + (detail::asan_granularity - 1u))
                        & ~static_cast<std::uintptr_t>(detail::asan_granularity - 1u);
                return reinterpret_cast<char*>(value);
            }

            // The ASan mode uses a different sub-allocation layout.
            // A block is split into shadow-granule aligned pieces: every non-zero
            // allocation gets at least `alignment` (and at least the 8 byte shadow
            // granularity), is rounded up to a granule boundary and is followed by a
            // redzone of at least 8 bytes. Only the requested bytes of the allocation
            // are unpoisoned. Since a following piece always starts on the next granule
            // boundary, the last (partial) granule of an object can never be partly
            // shared with the next object: the trailing bytes belong to the redzone.
            static constexpr std::size_t asan_redzone_size = detail::asan_granularity;

            // A block's user region only needs to be poisoned if the block starts on a
            // granule boundary; its end is truncated to whole granules. Block
            // allocations from the default allocators are granule aligned, but the
            // check keeps the invariant safe for custom block allocators.
            static bool asan_block_ok(const char* begin) noexcept
            {
                return reinterpret_cast<std::uintptr_t>(begin) % detail::asan_granularity == 0u;
            }

            // number of bytes of the block that lie on whole granules
            static std::size_t asan_block_usable(const char* begin, std::size_t size) noexcept
            {
                auto start = reinterpret_cast<std::uintptr_t>(begin);
                auto end   = start + size;
                end &= ~static_cast<std::uintptr_t>(detail::asan_granularity - 1u);
                return end > start ? std::size_t(end - start) : 0u;
            }

            // poisons the whole usable user region of the current block; the arena
            // management header lives in front of block.memory and is left untouched
            void asan_poison_current_block() noexcept
            {
                auto block = arena_.current_block();
                if (asan_block_ok(static_cast<char*>(block.memory)))
                    detail::asan_poison_aligned(block.memory,
                                                asan_block_usable(static_cast<char*>(block.memory),
                                                                  block.size));
            }

            // calculates the layout of one allocation inside [top, end),
            // returns false if it does not fit or the arithmetic would overflow
            static bool asan_layout(char* top, const char* end, std::size_t size,
                                    std::size_t alignment, char*& memory,
                                    std::size_t& needed) noexcept
            {
                // the allocation starts on a granule boundary so that its accessible
                // bytes never share a granule with the preceding alignment gap
                auto align = alignment < detail::asan_granularity ? detail::asan_granularity
                                                                   : alignment;
                if (!detail::is_valid_alignment(align))
                    return false;

                auto misalignment = std::size_t(reinterpret_cast<std::uintptr_t>(top) % align);
                auto offset       = misalignment != 0u ? align - misalignment : 0u;
                if (offset > std::size_t(end - top))
                    return false;

                // round the allocation up to a granule boundary, then add the redzone;
                // a zero-sized allocation still consumes a redzone so that repeated
                // zero-sized allocations do not all alias the same granule
                std::size_t end_of_object, padded;
                if (!detail::checked_add(offset, size, end_of_object)
                    || !detail::checked_add(end_of_object, detail::asan_granularity - 1u,  // NOLINT
                                            padded)
                    || !detail::checked_add(padded & ~(detail::asan_granularity - 1u),  // NOLINT
                                            asan_redzone_size, needed))
                    return false;

                memory = top + offset;
                return true;
            }

            // switches the stack to a block and makes all of it inaccessible
            void asan_begin_block(char* begin) noexcept
            {
                stack_ = detail::fixed_memory_stack(asan_align_up(begin));
                asan_poison_current_block();
            }

            void* asan_allocate(std::size_t size, std::size_t alignment)
            {
                auto top = stack_.top();
                if (top)
                {
                    char*       memory;
                    std::size_t needed;
                    if (asan_layout(top, block_end(), size, alignment, memory, needed)
                        && needed <= std::size_t(block_end() - top))
                    {
                        detail::asan_unpoison(memory, size);
                        detail::debug_fill(memory, size, debug_magic::new_memory);
                        stack_.set_top(top + needed);
                        return memory;
                    }
                }

                // need to grow
                auto block = arena_.allocate_block();
                auto begin = asan_align_up(static_cast<char*>(block.memory));

                // poison the new block, then hand out the first allocation from it
                asan_begin_block(begin);

                char*       memory;
                std::size_t needed;
                if (!asan_layout(begin, block_end(), size, alignment, memory, needed))
                    FOONATHAN_THROW(bad_allocation_size(info(), size, block.size));

                detail::check_allocation_size<bad_allocation_size>(
                    needed, std::size_t(block_end() - begin), info());

                detail::asan_unpoison(memory, size);
                detail::debug_fill(memory, size, debug_magic::new_memory);
                stack_.set_top(begin + needed);
                return memory;
            }

            void* asan_try_allocate(std::size_t size, std::size_t alignment) noexcept
            {
                auto top = stack_.top();
                if (!top)
                    return nullptr;

                char*       memory;
                std::size_t needed;
                if (!asan_layout(top, block_end(), size, alignment, memory, needed)
                    || needed > std::size_t(block_end() - top))
                    return nullptr;

                detail::asan_unpoison(memory, size);
                detail::debug_fill(memory, size, debug_magic::new_memory);
                stack_.set_top(top + needed);
                return memory;
            }

            // poisons [begin, end) again; begin is a stack top and end is a block end or
            // a stack top, both granule aligned
            void asan_poison_dead(char* begin, char* end) noexcept
            {
                auto size = std::size_t(end - begin);
                if (size == 0u)
                    return;

                // the region must be made accessible for the debug fill;
                // it is poisoned in its entirety afterwards, the fill only writes
                // within the region and the redzone stays poisoned
                detail::asan_unpoison(begin, size);
                detail::debug_fill(begin, size, debug_magic::freed_memory);
                detail::asan_poison_aligned(begin, size);
            }
#endif // FOONATHAN_MEMORY_ASAN

            memory_arena<allocator_type> arena_;
            detail::fixed_memory_stack   stack_;

            friend allocator_traits<memory_stack<BlockOrRawAllocator>>;
            friend composable_allocator_traits<memory_stack<BlockOrRawAllocator>>;
        };

        /// Simple utility that automatically unwinds a `Stack` to a previously saved location.
        /// A `Stack` is anything that provides a `marker`, a `top()` function returning a `marker`
        /// and an `unwind()` function to unwind to a `marker`,
        /// like a \ref foonathan::memory::memory_stack
        /// \ingroup allocator
        template <class Stack = memory_stack<>>
        class memory_stack_raii_unwind
        {
        public:
            using stack_type  = Stack;
            using marker_type = typename stack_type::marker;

            /// \effects Same as `memory_stack_raii_unwind(stack, stack.top())`.
            explicit memory_stack_raii_unwind(stack_type& stack) noexcept
            : memory_stack_raii_unwind(stack, stack.top())
            {
            }

            /// \effects Creates the unwinder by giving it the stack and the marker.
            /// \requires The stack must live longer than this object.
            memory_stack_raii_unwind(stack_type& stack, marker_type marker) noexcept
            : marker_(marker), stack_(&stack)
            {
            }

            /// \effects Move constructs the unwinder by taking the saved position from `other`.
            /// `other.will_unwind()` will return `false` after it.
            memory_stack_raii_unwind(memory_stack_raii_unwind&& other) noexcept
            : marker_(other.marker_), stack_(other.stack_)
            {
                other.stack_ = nullptr;
            }

            /// \effects Unwinds to the previously saved location,
            /// if there is any, by calling `unwind()`.
            ~memory_stack_raii_unwind() noexcept
            {
                if (stack_)
                    stack_->unwind(marker_);
            }

            /// \effects Move assigns the unwinder by taking the saved position from `other`.
            /// `other.will_unwind()` will return `false` after it.
            memory_stack_raii_unwind& operator=(memory_stack_raii_unwind&& other) noexcept
            {
                if (stack_)
                    stack_->unwind(marker_);

                marker_ = other.marker_;
                stack_  = other.stack_;

                other.stack_ = nullptr;

                return *this;
            }

            /// \effects Removes the location without unwinding it.
            /// `will_unwind()` will return `false`.
            void release() noexcept
            {
                stack_ = nullptr;
            }

            /// \effects Unwinds to the saved location explictly.
            /// \requires `will_unwind()` must return `true`.
            void unwind() noexcept
            {
                FOONATHAN_MEMORY_ASSERT(will_unwind());
                stack_->unwind(marker_);
            }

            /// \returns Whether or not the unwinder will actually unwind.
            /// \note It will not unwind if it is in the moved-from state.
            bool will_unwind() const noexcept
            {
                return stack_ != nullptr;
            }

            /// \returns The saved marker, if there is any.
            /// \requires `will_unwind()` must return `true`.
            marker_type get_marker() const noexcept
            {
                FOONATHAN_MEMORY_ASSERT(will_unwind());
                return marker_;
            }

            /// \returns The stack it will unwind.
            /// \requires `will_unwind()` must return `true`.
            stack_type& get_stack() const noexcept
            {
                FOONATHAN_MEMORY_ASSERT(will_unwind());
                return *stack_;
            }

        private:
            marker_type marker_;
            stack_type* stack_;
        };

#if FOONATHAN_MEMORY_EXTERN_TEMPLATE
        extern template class memory_stack<>;
        extern template class memory_stack_raii_unwind<memory_stack<>>;
#endif

        /// Specialization of the \ref allocator_traits for \ref memory_stack classes.
        /// \note It is not allowed to mix calls through the specialization and through the member functions,
        /// i.e. \ref memory_stack::allocate() and this \c allocate_node().
        /// \ingroup allocator
        template <class BlockAllocator>
        class allocator_traits<memory_stack<BlockAllocator>>
        {
        public:
            using allocator_type = memory_stack<BlockAllocator>;
            using is_stateful    = std::true_type;

            /// \returns The result of \ref memory_stack::allocate().
            static void* allocate_node(allocator_type& state, std::size_t size,
                                       std::size_t alignment)
            {
                auto mem = state.allocate(size, alignment);
                state.on_allocate(size);
                return mem;
            }

            /// \returns The result of \ref memory_stack::allocate().
            static void* allocate_array(allocator_type& state, std::size_t count, std::size_t size,
                                        std::size_t alignment)
            {
#if FOONATHAN_MEMORY_ASAN
                // the ASan layout arithmetic must not overflow on the total byte count
                std::size_t total;
                if (!detail::checked_mul(count, size, total))
                    FOONATHAN_THROW(bad_array_size(state.info(), count * size,
                                                   max_array_size(state)));
                return allocate_node(state, total, alignment);
#else
                return allocate_node(state, count * size, alignment);
#endif
            }

            /// @{
            /// \effects Does nothing besides bookmarking for leak checking, if that is enabled.
            /// Actual deallocation can only be done via \ref memory_stack::unwind().
            static void deallocate_node(allocator_type& state, void*, std::size_t size,
                                        std::size_t) noexcept
            {
                state.on_deallocate(size);
            }

            static void deallocate_array(allocator_type& state, void* ptr, std::size_t count,
                                         std::size_t size, std::size_t alignment) noexcept
            {
                deallocate_node(state, ptr, count * size, alignment);
            }
            /// @}

            /// @{
            /// \returns The maximum size which is \ref memory_stack::next_capacity().
            static std::size_t max_node_size(const allocator_type& state) noexcept
            {
                return state.next_capacity();
            }

            static std::size_t max_array_size(const allocator_type& state) noexcept
            {
                return state.next_capacity();
            }
            /// @}

            /// \returns The maximum possible value since there is no alignment restriction
            /// (except indirectly through \ref memory_stack::next_capacity()).
            static std::size_t max_alignment(const allocator_type&) noexcept
            {
                return std::size_t(-1);
            }
        };

        /// Specialization of the \ref composable_allocator_traits for \ref memory_stack classes.
        /// \ingroup allocator
        template <class BlockAllocator>
        class composable_allocator_traits<memory_stack<BlockAllocator>>
        {
        public:
            using allocator_type = memory_stack<BlockAllocator>;

            /// \returns The result of \ref memory_stack::try_allocate().
            static void* try_allocate_node(allocator_type& state, std::size_t size,
                                           std::size_t alignment) noexcept
            {
                return state.try_allocate(size, alignment);
            }

            /// \returns The result of \ref memory_stack::try_allocate().
            static void* try_allocate_array(allocator_type& state, std::size_t count,
                                            std::size_t size, std::size_t alignment) noexcept
            {
#if FOONATHAN_MEMORY_ASAN
                std::size_t total;
                if (!detail::checked_mul(count, size, total))
                    return nullptr;
                return state.try_allocate(total, alignment);
#else
                return state.try_allocate(count * size, alignment);
#endif
            }

            /// @{
            /// \effects Does nothing.
            /// \returns Whether the memory will be deallocated by \ref memory_stack::unwind().
            static bool try_deallocate_node(allocator_type& state, void* ptr, std::size_t,
                                            std::size_t) noexcept
            {
                return state.arena_.owns(ptr);
            }

            static bool try_deallocate_array(allocator_type& state, void* ptr, std::size_t count,
                                             std::size_t size, std::size_t alignment) noexcept
            {
                return try_deallocate_node(state, ptr, count * size, alignment);
            }
            /// @}
        };

#if FOONATHAN_MEMORY_EXTERN_TEMPLATE
        extern template class allocator_traits<memory_stack<>>;
        extern template class composable_allocator_traits<memory_stack<>>;
#endif
    } // namespace memory
} // namespace foonathan

#endif // FOONATHAN_MEMORY_MEMORY_STACK_HPP_INCLUDED
