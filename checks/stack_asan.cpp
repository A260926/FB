// Copyright (C) 2015-2025 Jonathan Müller and foonathan/memory contributors
// SPDX-License-Identifier: Zlib

// Checks for the FOONATHAN_MEMORY_ASAN support of memory_stack.
//
// Normal mode runs the behavioral test suite and reports the number of cases and
// assertions. If the first argument is a "child:<mode>" string, the process
// performs exactly one out-of-bounds access and is expected to die with an
// AddressSanitizer report; the normal mode spawns itself for each such access and
// verifies that the report comes from the intended instruction.

#include <foonathan/memory/allocator_traits.hpp>
#include <foonathan/memory/memory_stack.hpp>

// Only the enabled mode may reference the sanitizer interface; the disabled build
// is compiled and linked without AddressSanitizer to prove that it introduces no
// ASan symbol dependency.
#if FOONATHAN_MEMORY_ASAN
#    include <sanitizer/asan_interface.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#if defined(__APPLE__)
#    include <mach-o/dyld.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

using namespace foonathan::memory;

namespace
{
    int g_cases      = 0;
    int g_assertions = 0;

    void begin_case(const char* name)
    {
        ++g_cases;
        std::printf("ASAN_STACK case %s\n", name);
        std::fflush(stdout);
    }

    void check(bool condition, const char* expression, int line)
    {
        ++g_assertions;
        if (!condition)
        {
            std::printf("ASAN_STACK assertion failed at line %d: %s\n", line, expression);
            std::fflush(stdout);
            std::exit(2);
        }
    }

#define CHECK(Expr) check((Expr), #Expr, __LINE__)

#if FOONATHAN_MEMORY_ASAN
    bool poisoned(const void* memory, std::size_t size)
    {
        auto bytes = static_cast<const unsigned char*>(memory);
        for (std::size_t i = 0; i != size; ++i)
            if (__asan_address_is_poisoned(bytes + i) == 0)
                return false;
        return true;
    }
#endif

    bool accessible(const void* memory, std::size_t size)
    {
#if FOONATHAN_MEMORY_ASAN
        auto bytes = static_cast<const unsigned char*>(memory);
        for (std::size_t i = 0; i != size; ++i)
            if (__asan_address_is_poisoned(bytes + i) != 0)
                return false;
        return true;
#else
        (void)memory;
        (void)size;
        return true;
#endif
    }

    void fill(unsigned char* memory, std::size_t size, unsigned char value)
    {
        for (std::size_t i = 0; i != size; ++i)
            memory[i] = value;
    }

    bool filled(const unsigned char* memory, std::size_t size, unsigned char value)
    {
        for (std::size_t i = 0; i != size; ++i)
            if (memory[i] != value)
                return false;
        return true;
    }

#if FOONATHAN_MEMORY_ASAN
    // ---------------------------------------------------------------------
    // ASan enabled behavior
    // ---------------------------------------------------------------------
    void test_sizes_and_alignments()
    {
        begin_case("sizes-and-alignments");
        memory_stack<> stack(memory_stack<>::min_block_size(4096));

        const std::size_t sizes[]      = {1, 7, 8, 9, 31, 64, 127};
        const std::size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
        for (auto size : sizes)
            for (auto alignment : alignments)
            {
                auto memory = static_cast<unsigned char*>(stack.allocate(size, alignment));
                CHECK(reinterpret_cast<std::uintptr_t>(memory) % alignment == 0u);
                CHECK(accessible(memory, size));
                for (std::size_t i = 0; i != size; ++i)
                    memory[i] = 0x5A;
                CHECK(filled(memory, size, 0x5A));
                CHECK(poisoned(memory + size, 8));
            }
    }

    void test_neighbor_redzones()
    {
        begin_case("neighbor-redzones");
        memory_stack<> stack(memory_stack<>::min_block_size(4096));

        auto first  = static_cast<unsigned char*>(stack.allocate(1, 1));
        auto second = static_cast<unsigned char*>(stack.allocate(7, 1));
        CHECK(second > first);
        // the first object's tail is still poisoned when the second is accessible;
        // no partial granule is shared between them
        CHECK(poisoned(first + 1, 7));
        CHECK(accessible(second, 7));
        CHECK(poisoned(second + 7, 8));

        // over-aligned allocation: the alignment gap before it is poisoned
        auto before = stack.capacity_left();
        auto aligned
            = static_cast<unsigned char*>(stack.allocate(5, 256));
        auto after = stack.capacity_left();
        CHECK(reinterpret_cast<std::uintptr_t>(aligned) % 256u == 0u);
        CHECK(after < before); // same block
        CHECK(poisoned(aligned - 8, 8)); // gap before the object stays poisoned
        CHECK(accessible(aligned, 5));
        CHECK(poisoned(aligned + 5, 8));
    }

    void test_data_before_marker()
    {
        begin_case("data-before-marker");
        memory_stack<> stack(memory_stack<>::min_block_size(4096));

        auto keep  = static_cast<unsigned char*>(stack.allocate(13, 8));
        fill(keep, 13, 0x11);
        auto marker = stack.top();

        auto dead = static_cast<unsigned char*>(stack.allocate(31, 8));
        fill(dead, 31, 0x22);
        CHECK(accessible(keep, 13));
        CHECK(accessible(dead, 31));

        stack.unwind(marker);
        // data before the marker keeps contents and accessibility,
        // data after it is inaccessible again
        CHECK(accessible(keep, 13));
        CHECK(filled(keep, 13, 0x11));
        CHECK(poisoned(dead, 31));
        CHECK(poisoned(dead + 31, 8));
    }

    void test_cache_reuse()
    {
        begin_case("cache-reuse");
        memory_stack<> stack(memory_stack<>::min_block_size(128));

        auto marker = stack.top();
        auto first  = static_cast<unsigned char*>(stack.allocate(16, 8));
        fill(first, 16, 0x33);
        // force a second block
        auto big = static_cast<unsigned char*>(stack.allocate(200, 8));
        CHECK(stack.top() > marker);
        CHECK(accessible(big, 200));
        stack.unwind(marker);

        // both blocks are in the cache now, the user region is entirely poisoned
        CHECK(poisoned(first, 16));
        CHECK(poisoned(big, 200));

        // reuse: only the newly handed out sub-range is accessible, not the block
        auto reused = static_cast<unsigned char*>(stack.allocate(8, 8));
        CHECK(reused != nullptr);
        CHECK(accessible(reused, 8));
        CHECK(poisoned(reused + 8, 8));
        CHECK(poisoned(reused + 8, 120));
    }

    void test_try_allocate_failure()
    {
        begin_case("try-allocate-failure");
        memory_stack<> stack(memory_stack<>::min_block_size(256));

        auto alive = static_cast<unsigned char*>(stack.allocate(16, 8));
        fill(alive, 16, 0x44);

        auto capacity = stack.capacity_left();
        CHECK(stack.try_allocate(capacity + 1, 8) == nullptr);
        CHECK(stack.try_allocate(static_cast<std::size_t>(-1), 8) == nullptr);
        // failure does not disturb the previous allocation or the stack state
        CHECK(accessible(alive, 16));
        CHECK(filled(alive, 16, 0x44));

        auto next = static_cast<unsigned char*>(stack.allocate(8, 8));
        CHECK(accessible(next, 8));
        CHECK(poisoned(next + 8, 8));
    }

    void test_array_traits()
    {
        begin_case("array-traits");
        memory_stack<> stack(memory_stack<>::min_block_size(4096));
        allocator_traits<memory_stack<>> traits;

        auto node = static_cast<unsigned char*>(traits.allocate_node(stack, 13, 64));
        CHECK(reinterpret_cast<std::uintptr_t>(node) % 64u == 0u);
        CHECK(accessible(node, 13));
        CHECK(poisoned(node + 13, 8));

        std::size_t count     = 5;
        std::size_t node_size = 7;
        auto array = static_cast<unsigned char*>(
            traits.allocate_array(stack, count, node_size, 32));
        CHECK(reinterpret_cast<std::uintptr_t>(array) % 32u == 0u);
        CHECK(accessible(array, count * node_size));
        CHECK(poisoned(array + count * node_size, 8));

        // request size and alignment conventions are unchanged
        traits.deallocate_node(stack, node, 13, 64);
        traits.deallocate_array(stack, array, count, node_size, 32);

        // array size overflow is detected instead of overflowing the layout math
        bool caught = false;
        try
        {
            traits.allocate_array(stack, static_cast<std::size_t>(-1), 2, 8);
        }
        catch (bad_array_size&)
        {
            caught = true;
        }
        CHECK(caught);
        CHECK(stack.try_allocate(static_cast<std::size_t>(-1), 8) == nullptr);
    }

    void test_raii_unwind()
    {
        begin_case("raii-unwind");
        memory_stack<> stack(memory_stack<>::min_block_size(4096));

        auto keep = static_cast<unsigned char*>(stack.allocate(8, 8));

        unsigned char* transient = nullptr;
        {
            memory_stack_raii_unwind<memory_stack<>> unwinder(stack);
            transient = static_cast<unsigned char*>(stack.allocate(24, 8));
            CHECK(accessible(transient, 24));
            CHECK(accessible(keep, 8));
        }
        CHECK(poisoned(transient, 24));
        CHECK(accessible(keep, 8));

        // explicit unwind and release keep the original state rules
        {
            memory_stack_raii_unwind<memory_stack<>> unwinder(stack);
            auto f = static_cast<unsigned char*>(stack.allocate(8, 8));
            unwinder.unwind();
            CHECK(poisoned(f, 8));
            CHECK(unwinder.will_unwind());
        }
        {
            memory_stack_raii_unwind<memory_stack<>> unwinder(stack);
            auto f = static_cast<unsigned char*>(stack.allocate(8, 8));
            unwinder.release();
            CHECK(!unwinder.will_unwind());
            CHECK(accessible(f, 8));
        }
    }

    void test_move()
    {
        begin_case("move");
        memory_stack<> source(memory_stack<>::min_block_size(512));

        auto marker = source.top();
        auto memory = static_cast<unsigned char*>(source.allocate(32, 16));
        fill(memory, 32, 0x66);
        CHECK(accessible(memory, 32));

        memory_stack<> target(std::move(source));

        // existing user pointers stay valid, the receiver owns their poison lifetime
        CHECK(accessible(memory, 32));
        CHECK(filled(memory, 32, 0x66));

        // reusing the moved-from object does not affect the receiver
        auto other = static_cast<unsigned char*>(source.allocate(8, 8));
        CHECK(other != memory);
        CHECK(accessible(other, 8));
        CHECK(accessible(memory, 32));
        CHECK(filled(memory, 32, 0x66));

        auto extra = static_cast<unsigned char*>(target.allocate(8, 8));
        CHECK(accessible(extra, 8));
        target.unwind(marker);
        CHECK(poisoned(memory, 32));
        CHECK(poisoned(extra, 8));
    }

    void test_cleanup()
    {
        begin_case("cleanup");
        // shrink_to_fit and the destructor return blocks to the underlying
        // allocator; debug filling and fences must not trip ASan
        {
            memory_stack<> stack(memory_stack<>::min_block_size(128));
            auto          marker = stack.top();
            for (int i = 0; i != 16; ++i)
            {
                auto memory = static_cast<unsigned char*>(
                    stack.allocate(static_cast<std::size_t>(i * 9 + 1), 8));
                fill(memory, static_cast<std::size_t>(i * 9 + 1), 0x77);
            }
            stack.unwind(marker);
            stack.shrink_to_fit();
        }
        {
            memory_stack<> stack(memory_stack<>::min_block_size(64));
            auto          first = static_cast<unsigned char*>(stack.allocate(8, 8));
            CHECK(accessible(first, 8));
            // leave live allocations so the destructor has to clean poisoned blocks
        }
    }

    void test_zero_length()
    {
        begin_case("zero-length");
        memory_stack<> stack(memory_stack<>::min_block_size(256));
        // zero-length allocations follow the original convention, no byte access is checked;
        // distinct zero allocations must not alias and allocation must continue to work
        auto first  = stack.allocate(0, 8);
        auto second = stack.allocate(0, 8);
        CHECK(first != second);
        auto memory = static_cast<unsigned char*>(stack.allocate(8, 8));
        CHECK(accessible(memory, 8));
        CHECK(poisoned(memory + 8, 8));
    }

    // ---------------------------------------------------------------------
    // Child processes: one intentional invalid access each
    // ---------------------------------------------------------------------
#    if defined(__GNUC__) || defined(__clang__)
#        define NOINLINE __attribute__((noinline))
#    else
#        define NOINLINE
#    endif

    NOINLINE void child_tail_write()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(4096));
        auto memory = static_cast<unsigned char*>(stack.allocate(9, 8));
        std::fprintf(stderr, "STACK_ASAN_CHILD mode=tail addr=%p\n", static_cast<void*>(memory + 9));
        std::fflush(stderr);
        volatile unsigned char* write = memory + 9; // one byte past the requested size
        *write                        = 0x42;
    }

    NOINLINE void child_unwind_same_block_write()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(4096));
        auto marker = stack.top();
        auto memory = static_cast<unsigned char*>(stack.allocate(16, 8));
        stack.unwind(marker);
        std::fprintf(stderr, "STACK_ASAN_CHILD mode=unwind-same addr=%p\n",
                     static_cast<void*>(memory));
        std::fflush(stderr);
        volatile unsigned char* write = memory; // stale pointer into unwound memory
        *write                        = 0x42;
    }

    NOINLINE void child_unwind_cached_block_write()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(512));
        auto marker = stack.top();
        stack.allocate(400, 8);
        auto cross = stack.top();
        // force a second block (the growing allocator doubles the size),
        // then return it to the cache
        auto memory = static_cast<unsigned char*>(stack.allocate(700, 8));
        stack.unwind(cross);
        stack.unwind(marker);
        std::fprintf(stderr, "STACK_ASAN_CHILD mode=unwind-cached addr=%p\n",
                     static_cast<void*>(memory));
        std::fflush(stderr);
        volatile unsigned char* write = memory; // points into a block in the arena cache
        *write                        = 0x42;
    }

    const char* const child_modes[] = {"tail", "unwind-same", "unwind-cached"};

    bool resolve_self(std::string& path)
    {
#    if defined(__APPLE__)
        std::uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size + 1, '\0');
        if (_NSGetExecutablePath(&buffer[0], &size) != 0)
            return false;
        path.assign(buffer.c_str());
        return true;
#    elif defined(__linux__)
        char    buffer[4096];
        ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (length <= 0)
            return false;
        buffer[length] = '\0';
        path.assign(buffer);
        return true;
#    else
        (void)path;
        return false;
#    endif
    }

    std::string read_all(int fd)
    {
        std::string result;
        char        buffer[4096];
        for (;;)
        {
            auto length = ::read(fd, buffer, sizeof(buffer));
            if (length < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (length == 0)
                break;
            result.append(buffer, static_cast<std::size_t>(length));
        }
        return result;
    }

    bool contains(const std::string& haystack, const std::string& needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    void run_child_check(const char* mode)
    {
        begin_case(mode);
        std::string self;
        if (!resolve_self(self))
        {
            std::printf("ASAN_STACK child %s: could not resolve own executable path\n", mode);
            std::exit(2);
        }

        int pipe_fds[2];
        if (::pipe(pipe_fds) != 0)
        {
            std::printf("ASAN_STACK child %s: pipe failed\n", mode);
            std::exit(2);
        }

        auto pid = ::fork();
        if (pid < 0)
        {
            std::printf("ASAN_STACK child %s: fork failed\n", mode);
            std::exit(2);
        }
        if (pid == 0)
        {
            ::close(pipe_fds[0]);
            ::dup2(pipe_fds[1], STDERR_FILENO);
            ::close(pipe_fds[1]);
            // deterministic sanitizer behavior in the child
            ::setenv("ASAN_OPTIONS", "detect_leaks=0:halt_on_error=1", 1);
            std::string argument = std::string("child:") + mode;
            ::execl(self.c_str(), self.c_str(), argument.c_str(), static_cast<char*>(nullptr));
            std::fprintf(stderr, "ASAN_STACK_CHILD exec failed\n");
            ::_exit(127);
        }

        ::close(pipe_fds[1]);
        auto output = read_all(pipe_fds[0]);
        ::close(pipe_fds[0]);

        int status = 0;
        ::waitpid(pid, &status, 0);
        bool died = WIFSIGNALED(status)
                    || (WIFEXITED(status) && WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 127);

        std::string marker = std::string("STACK_ASAN_CHILD mode=") + mode;
        bool        reached = contains(output, marker);
        bool        reported
            = contains(output, "AddressSanitizer") && contains(output, "use-after-poison");
        // the report must come from the designated write, not from an unrelated crash:
        // find the address printed right before the access and require it in the report
        bool address_matched = false;
        {
            auto position = output.find(marker);
            if (position != std::string::npos)
            {
                auto address_position = output.find("0x", position);
                if (address_position != std::string::npos)
                {
                    auto address_end = output.find_first_of(" \n\r", address_position);
                    if (address_end != std::string::npos)
                    {
                        auto address = output.substr(address_position,
                                                     address_end - address_position);
                        address_matched = contains(output, "WRITE of size 1 at " + address)
                                          || contains(output, "on address " + address);
                    }
                }
            }
        }

        std::printf("ASAN_STACK child mode=%s reached=%d report=%d address_match=%d died=%d\n",
                    mode, reached ? 1 : 0, reported ? 1 : 0, address_matched ? 1 : 0,
                    died ? 1 : 0);
        CHECK(reached);
        CHECK(reported);
        CHECK(address_matched);
        CHECK(died);
    }

    void run_asan_suite()
    {
        test_sizes_and_alignments();
        test_neighbor_redzones();
        test_data_before_marker();
        test_cache_reuse();
        test_try_allocate_failure();
        test_array_traits();
        test_raii_unwind();
        test_move();
        test_cleanup();
        test_zero_length();

        for (auto mode : child_modes)
            run_child_check(mode);
    }
#else
    // ---------------------------------------------------------------------
    // Disabled mode: original behavior must be intact
    // ---------------------------------------------------------------------
    void run_disabled_suite()
    {
        begin_case("disabled-original-behavior");
        memory_stack<> stack(memory_stack<>::min_block_size(4096));
        auto           marker = stack.top();
        auto memory = static_cast<unsigned char*>(stack.allocate(31, 16));
        CHECK(reinterpret_cast<std::uintptr_t>(memory) % 16u == 0u);
        fill(memory, 31, 0x35);
        CHECK(filled(memory, 31, 0x35));
        stack.unwind(marker);
        // without ASan support the old memory is plain accessible and reused
        CHECK(accessible(memory, 31));
        auto reused = static_cast<unsigned char*>(stack.allocate(31, 16));
        CHECK(reused == memory);

        auto capacity = stack.capacity_left();
        stack.unwind(marker);
        CHECK(stack.capacity_left() >= capacity);
        stack.shrink_to_fit();
    }
#endif
} // namespace

int main(int argc, char** argv)
{
#if FOONATHAN_MEMORY_ASAN
    if (argc > 1 && std::strncmp(argv[1], "child:", 6) == 0)
    {
        const char* mode = argv[1] + 6;
        if (std::strcmp(mode, "tail") == 0)
            child_tail_write();
        else if (std::strcmp(mode, "unwind-same") == 0)
            child_unwind_same_block_write();
        else if (std::strcmp(mode, "unwind-cached") == 0)
            child_unwind_cached_block_write();
        else
        {
            std::fprintf(stderr, "unknown child mode %s\n", mode);
            return 2;
        }
        // a working ASan must have aborted before this point
        std::fprintf(stderr, "STACK_ASAN_CHILD mode=%s did not crash\n", mode);
        return 3;
    }

    run_asan_suite();
    std::printf("ASAN_STACK normal mode=asan cases=%d assertions=%d\n", g_cases, g_assertions);
#else
    (void)argc;
    (void)argv;
    run_disabled_suite();
    std::printf("ASAN_STACK normal mode=disabled cases=%d assertions=%d\n", g_cases,
                g_assertions);
#endif
    return 0;
}
