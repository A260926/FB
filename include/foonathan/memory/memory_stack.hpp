// Copyright (C) 2015-2025 Jonathan Müller and foonathan/memory contributors
// SPDX-License-Identifier: Zlib

#ifndef FOONATHAN_MEMORY_MEMORY_STACK_HPP_INCLUDED
#define FOONATHAN_MEMORY_MEMORY_STACK_HPP_INCLUDED

/// \file
/// Class \ref foonathan::memory::memory_stack and its \ref foonathan::memory::allocator_traits specialization.

// Inform that foonathan::memory::memory_stack::min_block_size API is available
#define FOONATHAN_MEMORY_MEMORY_STACK_HAS_MIN_BLOCK_SIZE

#include <cstdint>
#include <limits>
#include <type_traits>

#include "detail/assert.hpp"
#include "detail/memory_stack.hpp"
#include "detail/asan.hpp"
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
            static constexpr std::size_t min_block_size(std::size_t byte_size) noexcept
            {
                return detail::memory_block_stack::implementation_offset() + byte_size;
            }

            /// \effects Creates it with a given initial block size and and other constructor arguments for the \concept{concept_blockallocator,BlockAllocator}.
            /// It will allocate the first block and sets the top to its beginning.
            /// \requires \c block_size must be at least \c min_block_size(1).
            template <typename... Args>
            explicit memory_stack(std::size_t block_size, Args&&... args)
            : arena_(block_size, detail::forward<Args>(args)...),
              stack_(arena_.allocate_block().memory)
            {
#if FOONATHAN_MEMORY_ASAN
                // the freshly obtained block is fully accessible; hide every byte
                // that has not been handed out as a sub-allocation yet, leaving the
                // arena management header (which lives before the user region) alone
                poison_current_block();
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
                return asan_allocate(size, alignment, true);
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
                return asan_allocate(size, alignment, false);
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
                    arena_.deallocate_block();
                    for (std::size_t i = 1; i != to_deallocate; ++i)
                        arena_.deallocate_block();

                    detail::debug_check_pointer(
                        [&]
                        {
                            auto cur = arena_.current_block();
                            return m.end == static_cast<char*>(cur.memory) + cur.size;
                        },
                        info(), m.top);

#if FOONATHAN_MEMORY_ASAN
                    // the surviving block is the current one; open a narrow window
                    // for the freed-memory fill and leave the bytes before the
                    // marker accessible, then poison everything handed back
                    {
                        detail::asan_access_guard guard(m.top,
                                                        std::size_t(m.end - m.top));
                        detail::debug_fill_free(m.top, std::size_t(m.end - m.top), 0);
                    }
                    asan_poison_range(m.top, m.end);
#else
                    // mark memory from new top to end of the block as freed
                    detail::debug_fill_free(m.top, std::size_t(m.end - m.top), 0);
#endif
                    stack_ = detail::fixed_memory_stack(m.top);
                }
                else // same index
                {
                    detail::debug_check_pointer([&] { return stack_.top() >= m.top; }, info(),
                                                m.top);
#if FOONATHAN_MEMORY_ASAN
                    auto old_top = stack_.top();
                    {
                        // narrow window for the freed-memory debug fill
                        detail::asan_access_guard guard(m.top,
                                                        std::size_t(old_top - m.top));
                        stack_.unwind(m.top);
                    }
                    // every byte from the marker up to the old cursor is dead;
                    // the live prefix [block start, m.top) stays accessible
                    asan_poison_range(m.top, old_top);
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
                return std::size_t(block_end() - stack_.top());
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

            const char* block_end() const noexcept
            {
                auto block = arena_.current_block();
                return static_cast<const char*>(block.memory) + block.size;
            }

#if FOONATHAN_MEMORY_ASAN
            // The largest address inside the block that lies on a shadow
            // granularity boundary.  Only this prefix is ever managed by the
            // stack, so every cursor stays 8-byte aligned and poisoning never
            // relies on partial-granule shadow writes.
            const char* asan_block_end() const noexcept
            {
                auto block = arena_.current_block();
                auto base  = reinterpret_cast<std::uintptr_t>(block.memory);
                auto end   = base + (block.size & ~(detail::asan_granularity - 1u));
                return reinterpret_cast<const char*>(end);
            }

            // Poisons the full managed prefix of the current block.
            void poison_current_block() noexcept
            {
                auto block = arena_.current_block();
                asan_poison_range(static_cast<const char*>(block.memory), asan_block_end());
            }

            // Poisons an 8-byte aligned range; the length is a multiple of the
            // granularity, which is the only poisoning form that is reliable on
            // every supported sanitizer runtime.
            static void asan_poison_range(const char* begin, const char* end) noexcept
            {
                auto first = reinterpret_cast<std::uintptr_t>(begin);
                auto last  = reinterpret_cast<std::uintptr_t>(end)
                             & ~(detail::asan_granularity - 1u);
                if (last > first)
                    detail::asan_poison_aligned(begin, std::size_t(last - first));
            }

            // Rounds a pointer up to the requested (power-of-two) alignment and
            // reports whether the address arithmetic stayed in range.
            static bool asan_align_up(std::uintptr_t address, std::size_t alignment,
                                      std::uintptr_t& result) noexcept
            {
                auto pad = alignment - 1u;
                if (pad > std::numeric_limits<std::uintptr_t>::max() - address)
                    return false;
                result = (address + pad) & ~static_cast<std::uintptr_t>(pad);
                return true;
            }

            // Computes the placement of a sub-allocation without changing any
            // state and without ever forming an out-of-range pointer.  The object
            // pointer is always raised to at least the shadow granularity, so
            // aligned unpoisoning can encode its exact size even when the
            // requested alignment is smaller than 8 bytes.  At least one full
            // granule of redzone is reserved behind the object and the next cursor
            // again lands on a granularity boundary.
            //
            // Returns false if any arithmetic overflows; otherwise sets `head`
            // (bytes skipped for alignment) and `span` (total bytes consumed from
            // the cursor, always a granularity multiple).  Both are plain sizes,
            // so fitting is decided before any object pointer is created.
            bool asan_layout(std::size_t size, std::size_t alignment, std::size_t& head,
                             std::size_t& span) const noexcept
            {
                constexpr auto gran = detail::asan_granularity;
                auto           ea   = alignment < gran ? gran : alignment;

                auto base_addr = reinterpret_cast<std::uintptr_t>(stack_.top());
                std::uintptr_t aligned;
                if (!asan_align_up(base_addr, ea, aligned))
                    return false;
                if (aligned < base_addr)
                    return false;
                head = static_cast<std::size_t>(aligned - base_addr);

                std::size_t rounded;
                if (detail::asan_add_overflow(size, gran - 1u, rounded))
                    return false;
                rounded &= ~(gran - 1u);

                std::size_t tail;
                if (detail::asan_add_overflow(rounded, gran, tail))
                    return false;
                if (detail::asan_add_overflow(head, tail, span))
                    return false;
                return true;
            }

            // Shared implementation of allocate()/try_allocate().  When growing is
            // allowed a block that does not fit triggers arena growth, otherwise
            // the call fails cleanly with a null result and no state change.
            void* asan_allocate(std::size_t size, std::size_t alignment, bool allow_growth)
            {
                std::size_t head = 0u;
                std::size_t span = 0u;

                auto capacity = [&] {
                    return static_cast<std::size_t>(asan_block_end() - stack_.top());
                };

                auto fits = [&] {
                    return stack_.top()
                           && asan_layout(size, alignment, head, span)
                           && span <= capacity();
                };

                if (!fits())
                {
                    if (!allow_growth)
                        return nullptr;

                    // need to grow
                    auto block = arena_.allocate_block();
                    stack_     = detail::fixed_memory_stack(block.memory);
                    poison_current_block();

                    if (!asan_layout(size, alignment, head, span))
                        FOONATHAN_THROW(bad_allocation_size(info(), size, block.size));

                    detail::check_allocation_size<bad_allocation_size>(span, block.size, info());
                    if (span > capacity())
                        FOONATHAN_THROW(bad_allocation_size(info(), size, block.size));
                }

                // fit is proven; only now form the in-range pointers
                auto base   = stack_.top();
                auto object = base + head;
                auto new_top = base + span;

#if FOONATHAN_MEMORY_DEBUG_FILL
                {
                    // open one narrow window covering exactly the bytes this
                    // allocation traverses, apply the debug magic, then close it;
                    // the object range is reopened below
                    detail::asan_access_guard guard(base, span);
                    detail::debug_fill(base, head, debug_magic::alignment_memory);
                    detail::debug_fill(object, size, debug_magic::new_memory);
                    detail::debug_fill(object + size, std::size_t(new_top - object) - size,
                                       debug_magic::fence_memory);
                }
#endif

                // hand out exactly the requested bytes; the partial tail of a
                // short object stays poisoned inside its own granule
                detail::asan_unpoison_aligned(object, size);
                stack_.bump(std::size_t(new_top - base));
                return object;
            }
#endif

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
                return allocate_node(state, count * size, alignment);
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
                return state.try_allocate(count * size, alignment);
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
