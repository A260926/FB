// Copyright (C) 2015-2025 Jonathan Müller and foonathan/memory contributors
// SPDX-License-Identifier: Zlib

// Standalone verification for the AddressSanitizer poison lifecycle of
// foonathan::memory::memory_stack.
//
// Two kinds of checks are performed:
//   * "state" cases run in this process and query the shadow memory byte by
//     byte through the compiler-provided sanitizer/asan_interface.h, plus real
//     reads and writes inside the requested ranges.
//   * "fault" cases re-exec this binary in a child process.  The child rebuilds
//     the exact scenario, prints a unique marker naming its own faulting
//     address, then deliberately touches that inaccessible byte.  The parent
//     only accepts the case if the child reached the access (marker present),
//     AddressSanitizer (not a plain crash) reported an error and the report
//     names the exact address from the marker.
//
// The whole file is C++11 and uses only the system compiler's ASan runtime.

#include <foonathan/memory/memory_stack.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if FOONATHAN_MEMORY_ASAN
#include <sanitizer/asan_interface.h>
#endif

#include <sys/wait.h>
#include <unistd.h>

using namespace foonathan::memory;

namespace
{
    int g_cases      = 0;
    int g_assertions = 0;
    int g_failures   = 0;

#define REQUIRE(cond)                                                                             \
    do                                                                                             \
    {                                                                                              \
        ++g_assertions;                                                                            \
        if (!(cond))                                                                               \
        {                                                                                          \
            ++g_failures;                                                                          \
            std::printf("  ASSERTION FAILED line %d: %s\n", __LINE__, #cond);                     \
        }                                                                                          \
    } while (0)

#if FOONATHAN_MEMORY_ASAN
    bool is_poisoned(const void* addr)
    {
        return __asan_address_is_poisoned(addr) != 0;
    }

    // Asserts the exact accessibility window of a sub-allocation: every
    // requested byte is readable/writable, while an 8 byte tail behind it is
    // inaccessible, even when object and tail share a shadow granule.
    void check_window(unsigned char* ptr, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i)
            REQUIRE(!is_poisoned(ptr + i));
        for (std::size_t i = size; i < size + 8u; ++i)
            REQUIRE(is_poisoned(ptr + i));
        // real access over the whole requested range
        unsigned char sink = 0;
        for (std::size_t i = 0; i < size; ++i)
            ptr[i] = static_cast<unsigned char>(i * 7u + 3u);
        for (std::size_t i = 0; i < size; ++i)
            sink = static_cast<unsigned char>(sink + ptr[i]);
        if (sink == 0xFF)
            std::printf("");
    }

    // Asserts a byte range is fully poisoned.
    void check_range_poisoned(unsigned char* begin, unsigned char* end)
    {
        for (auto p = begin; p != end; ++p)
            REQUIRE(is_poisoned(p));
    }
#endif

    // ---- fault scenarios ---------------------------------------------------
    // Each builds its own stack and returns the address that must be
    // inaccessible at the moment the child performs the bad access.  They run
    // independently in the freshly exec'd child, so all pointers are local to
    // that process.

#if FOONATHAN_MEMORY_ASAN
    unsigned char* fault_setup_tail_redzone()
    {
        static memory_stack<> stack(memory_stack<>::min_block_size(1024));
        auto* one = static_cast<unsigned char*>(stack.allocate(1u, 1u));
        check_window(one, 1u);
        return one + 1u; // first byte of the redzone of a one-byte object
    }

    unsigned char* fault_setup_same_block_unwind()
    {
        static memory_stack<> stack(memory_stack<>::min_block_size(1024));
        auto marker = stack.top();
        auto* dead = static_cast<unsigned char*>(stack.allocate(31u, 16u));
        check_window(dead, 31u);
        stack.unwind(marker);
        check_range_poisoned(dead, dead + 31u + 8u);
        return dead;
    }

    unsigned char* fault_setup_cross_block_cached()
    {
        static memory_stack<> stack(memory_stack<>::min_block_size(256));
        auto root = stack.top();
        static_cast<void>(stack.allocate(40u, 8u));
        auto* second = static_cast<unsigned char*>(stack.allocate(200u, 1u)); // grows
        static_cast<void>(stack.allocate(200u, 1u));                         // grows again
        stack.unwind(root); // all blocks return to the cache, still poisoned
        check_range_poisoned(second, second + 40u);
        return second; // lives in a block that is now only in the cache
    }

    struct fault_setup
    {
        const char* name;
        unsigned char* (*fn)();
    };

    const fault_setup fault_table[] = {
        {"tail_redzone", &fault_setup_tail_redzone},
        {"same_block_unwind", &fault_setup_same_block_unwind},
        {"cross_block_cached", &fault_setup_cross_block_cached},
    };

    unsigned char* run_fault_setup(const char* name)
    {
        for (const auto& f : fault_table)
            if (std::strcmp(f.name, name) == 0)
                return f.fn();
        return nullptr;
    }
#endif

    // Child entry point: rebuild the scenario, announce the exact target
    // address, then touch it.
    int run_fault_child(const char* fault_name)
    {
#if FOONATHAN_MEMORY_ASAN
        auto* target = run_fault_setup(fault_name);
        if (target == nullptr)
        {
            std::printf("ERROR: unknown fault case %s\n", fault_name);
            return 97;
        }

        std::printf("MARKER %s target=0x%llx\n", fault_name,
                    static_cast<unsigned long long>(
                        reinterpret_cast<std::uintptr_t>(target)));
        std::fflush(stdout);

        volatile unsigned char* ptr = target;
        ptr[0] = 0xBC; // must be reported as an AddressSanitizer error
        std::printf("ERROR: %s access was not caught\n", fault_name);
        return 99;
#else
        (void)fault_name;
        std::printf("ERROR: fault case requested with FOONATHAN_MEMORY_ASAN disabled\n");
        return 96;
#endif
    }

    // Spawns the named fault case and validates the AddressSanitizer report.
    // The target address is taken from the child's own marker, so an unmapped
    // address producing a plain SIGSEGV cannot masquerade as success.
    void expect_fault(const char* self, const char* fault_name)
    {
        int out_pipe[2];
        int err_pipe[2];
        if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
        {
            ++g_failures;
            std::printf("  %s: pipe() failed\n", fault_name);
            return;
        }

        pid_t pid = fork();
        if (pid == 0)
        {
            ::close(out_pipe[0]);
            ::close(err_pipe[0]);
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            ::close(out_pipe[1]);
            ::close(err_pipe[1]);
            execl(self, self, "fault", fault_name, static_cast<char*>(nullptr));
            _exit(98);
        }

        ::close(out_pipe[1]);
        ::close(err_pipe[1]);

        char out_buf[512];
        char err_buf[4096];
        std::size_t out_len = 0, err_len = 0;
        for (;;)
        {
            auto n = read(out_pipe[0], out_buf + out_len, sizeof(out_buf) - 1 - out_len);
            if (n <= 0)
                break;
            out_len += static_cast<std::size_t>(n);
        }
        for (;;)
        {
            auto n = read(err_pipe[0], err_buf + err_len, sizeof(err_buf) - 1 - err_len);
            if (n <= 0)
                break;
            err_len += static_cast<std::size_t>(n);
        }
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        out_buf[out_len] = '\0';
        err_buf[err_len] = '\0';

        int status = 0;
        waitpid(pid, &status, 0);

        // extract the target the child itself announced
        char want_marker[128];
        std::snprintf(want_marker, sizeof(want_marker), "MARKER %s target=", fault_name);
        const char* marker_at = std::strstr(out_buf, want_marker);
        const char* addr_at   = marker_at ? marker_at + std::strlen(want_marker) : nullptr;

        bool marker_seen = marker_at != nullptr;
        bool addr_named  = false;
        if (addr_at)
        {
            // the marker prints the address as 0xHEX; require that same token
            // inside the sanitizer report on stderr
            char want_addr[40];
            std::size_t i = 0;
            while (addr_at[i] != '\0' && addr_at[i] != '\n' && i < sizeof(want_addr) - 1)
            {
                want_addr[i] = addr_at[i];
                ++i;
            }
            want_addr[i] = '\0';
            addr_named    = std::strstr(err_buf, want_addr) != nullptr;
        }

        bool asan_report = std::strstr(err_buf, "AddressSanitizer:") != nullptr;
        bool died        = !WIFEXITED(status) || WEXITSTATUS(status) != 0;

        REQUIRE(marker_seen);
        REQUIRE(asan_report);
        REQUIRE(addr_named);
        REQUIRE(died);
        std::printf("  [fault] %-28s marker=%d asan=%d addr=%d died=%d\n", fault_name,
                    marker_seen, asan_report, addr_named, died);
    }

#if FOONATHAN_MEMORY_ASAN
    // ---- state cases -------------------------------------------------------

    void case_sizes_and_alignments()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(4096));
        const std::size_t sizes[] = {1u, 7u, 8u, 9u, 31u};
        for (auto s : sizes)
        {
            auto* p = static_cast<unsigned char*>(stack.allocate(s, 1u));
            check_window(p, s);
        }

        // natural and over-aligned requests
        const std::size_t aligns[] = {1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u};
        for (auto a : aligns)
        {
            auto* p = static_cast<unsigned char*>(stack.allocate(33u, a));
            REQUIRE((reinterpret_cast<std::uintptr_t>(p) & (a - 1u)) == 0u);
            check_window(p, 33u);
        }

        // the leading alignment gap of a raised object stays inaccessible
        auto* before = static_cast<unsigned char*>(stack.allocate(1u, 1u));
        auto* aligned = static_cast<unsigned char*>(stack.allocate(16u, 64u));
        check_range_poisoned(before + 1u, aligned);
    }

    void case_marker_prefix_survives()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(1024));
        auto* keep = static_cast<unsigned char*>(stack.allocate(40u, 8u));
        for (int i = 0; i < 40; ++i)
            keep[i] = 0x5A;

        auto marker = stack.top(); // everything below stays alive
        auto* dead = static_cast<unsigned char*>(stack.allocate(57u, 1u));
        check_window(dead, 57u);

        stack.unwind(marker);

        for (int i = 0; i < 40; ++i)
            REQUIRE(keep[i] == 0x5A); // content preserved
        for (int i = 0; i < 40; ++i)
            REQUIRE(!is_poisoned(keep + i)); // still accessible
        check_range_poisoned(dead, dead + 57u + 8u);
    }

    void case_cache_reuse_subrange()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(256));
        auto root = stack.top();

        auto* first = static_cast<unsigned char*>(stack.allocate(40u, 8u));
        auto* second = static_cast<unsigned char*>(stack.allocate(200u, 1u)); // grows
        auto* third = static_cast<unsigned char*>(stack.allocate(200u, 1u));  // grows again
        REQUIRE(second != nullptr);
        REQUIRE(third != nullptr);

        stack.unwind(root); // every block returns to the cache poisoned
        check_range_poisoned(first, first + 40u);
        check_range_poisoned(second, second + 40u);
        check_range_poisoned(third, third + 40u);

        // reuse opens only the freshly handed-out sub-range, never the block
        auto* reused = static_cast<unsigned char*>(stack.allocate(31u, 16u));
        check_window(reused, 31u);
        REQUIRE(is_poisoned(reused + 64u));
    }

    void case_move_transfers_lifecycle()
    {
        auto* source = new memory_stack<>(memory_stack<>::min_block_size(512));
        auto root    = source->top();
        auto* p      = static_cast<unsigned char*>(source->allocate(31u, 1u));
        for (int i = 0; i < 31; ++i)
            p[i] = 0x77;

        memory_stack<> receiver(std::move(*source));
        delete source; // moved-from object owns nothing now

        for (int i = 0; i < 31; ++i)
            REQUIRE(p[i] == 0x77);
        for (int i = 0; i < 31; ++i)
            REQUIRE(!is_poisoned(p + i));

        receiver.unwind(root);
        REQUIRE(is_poisoned(p));
    }

    void case_raii_unwinder()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(512));
        auto* older = static_cast<unsigned char*>(stack.allocate(24u, 1u));
        check_window(older, 24u);

        {
            memory_stack_raii_unwind<memory_stack<>> guard(stack);
            auto* scoped = static_cast<unsigned char*>(stack.allocate(24u, 1u));
            check_window(scoped, 24u);
        } // destructor unwinds

        for (std::size_t i = 0; i < 24u; ++i)
            REQUIRE(!is_poisoned(older + i));
        REQUIRE(is_poisoned(older + 32u));
    }

    void case_failed_allocation_keeps_stack_usable()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(200));
        auto marker = stack.top();
        REQUIRE(stack.try_allocate(50u, 1u) != nullptr);
        REQUIRE(stack.try_allocate(100000u, 1u) == nullptr); // no growth, clean failure
        auto* after = static_cast<unsigned char*>(stack.try_allocate(10u, 1u));
        REQUIRE(after != nullptr);
        check_window(after, 10u);
        stack.unwind(marker);
    }

    void case_debug_and_release_cleanup()
    {
        // exercises block recycling, cache return, shrink_to_fit and
        // destruction through the narrow internal-access guards, in whatever
        // fill/fence configuration this binary was built with
        memory_stack<> stack(memory_stack<>::min_block_size(128));
        for (int round = 0; round < 4; ++round)
        {
            auto marker = stack.top();
            for (std::size_t i = 0; i < 6u; ++i)
            {
                auto size = i * 8u + 1u;
                auto* p   = static_cast<unsigned char*>(stack.allocate(size, 1u));
                for (std::size_t b = 0; b < size; ++b)
                    p[b] = static_cast<unsigned char>(b);
            }
            stack.unwind(marker); // blocks go back to the cache
        }
        stack.shrink_to_fit(); // cached blocks return to the underlying allocator

        auto* fresh = static_cast<unsigned char*>(stack.allocate(17u, 1u));
        check_window(fresh, 17u);
    }

    void case_zero_size_convention()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(256));
        void* zero = stack.allocate(0u, 1u);
        REQUIRE(zero != nullptr);
        // a following non-zero allocation is still placed and fully guarded
        auto* p = static_cast<unsigned char*>(stack.allocate(9u, 1u));
        check_window(p, 9u);
    }

    void case_allocator_traits_lifecycle()
    {
        // drive the stack exclusively through its allocator_traits and
        // composable_allocator_traits specializations; the poison contract is
        // identical because both forward to the same instrumented member calls
        using stack_type = memory_stack<>;
        using traits     = allocator_traits<stack_type>;
        using composable = composable_allocator_traits<stack_type>;

        stack_type stack(stack_type::min_block_size(1024));
        auto marker = stack.top();

        auto* node = static_cast<unsigned char*>(
            traits::allocate_node(stack, 31u, 16u));
        check_window(node, 31u);

        auto* arr = static_cast<unsigned char*>(
            traits::allocate_array(stack, 4u, 9u, 1u)); // 36 byte array
        check_window(arr, 4u * 9u);

        auto* maybe = static_cast<unsigned char*>(
            composable::try_allocate_node(stack, 100000u, 1u));
        REQUIRE(maybe == nullptr); // clean failure leaves the state intact

        auto* small = static_cast<unsigned char*>(
            composable::try_allocate_node(stack, 5u, 1u));
        REQUIRE(small != nullptr);
        check_window(small, 5u);

        REQUIRE(composable::try_deallocate_node(stack, node, 31u, 16u));

        stack.unwind(marker);
        REQUIRE(is_poisoned(node));
        REQUIRE(is_poisoned(arr));
        REQUIRE(is_poisoned(small));
    }
#endif

    // Behaviour that must hold regardless of the sanitizer configuration, so the
    // same binary also serves as the feature-off baseline.
    void case_basic_contract()
    {
        memory_stack<> stack(memory_stack<>::min_block_size(512));
        auto marker = stack.top();
        auto* a     = static_cast<unsigned char*>(stack.allocate(31u, 16u));
        REQUIRE(a != nullptr);
        REQUIRE(reinterpret_cast<std::uintptr_t>(a) % 16u == 0u);
        for (int i = 0; i < 31; ++i)
            a[i] = 0x35;
        stack.unwind(marker);
        auto* b = static_cast<unsigned char*>(stack.allocate(31u, 16u));
        REQUIRE(b == a); // same-address reuse is part of the stack contract
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "fault") == 0)
        return run_fault_child(argv[2]);

    const char* self = argv[0];
    std::printf("== foonathan memory_stack AddressSanitizer checks "
                "(FOONATHAN_MEMORY_ASAN=%d) ==\n",
                static_cast<int>(FOONATHAN_MEMORY_ASAN));

    ++g_cases;
    case_basic_contract();
    std::printf("  [state] basic_contract\n");

#if FOONATHAN_MEMORY_ASAN
    struct state_entry
    {
        const char* name;
        void (*fn)();
    };
    const state_entry state_cases[] = {
        {"sizes_and_alignments", &case_sizes_and_alignments},
        {"marker_prefix_survives", &case_marker_prefix_survives},
        {"cache_reuse_subrange", &case_cache_reuse_subrange},
        {"allocator_traits_lifecycle", &case_allocator_traits_lifecycle},
        {"move_transfers_lifecycle", &case_move_transfers_lifecycle},
        {"raii_unwinder", &case_raii_unwinder},
        {"failed_allocation_keeps_usable", &case_failed_allocation_keeps_stack_usable},
        {"debug_and_release_cleanup", &case_debug_and_release_cleanup},
        {"zero_size_convention", &case_zero_size_convention},
    };
    for (const auto& c : state_cases)
    {
        ++g_cases;
        c.fn();
        std::printf("  [state] %s\n", c.name);
    }

    const char* fault_names[] = {"tail_redzone", "same_block_unwind", "cross_block_cached"};
    for (const char* name : fault_names)
    {
        ++g_cases;
        expect_fault(self, name);
    }
#endif

    std::printf("== cases=%d assertions=%d failures=%d ==\n", g_cases, g_assertions,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
