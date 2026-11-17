#pragma once
//
// High-precision timing for the MemPDB benchmark suite.
//
// On x86/x64: RDTSC/RDTSCP calibrated against a high-res OS clock.
// Elsewhere: steady_clock ticks (treated as a 1 Hz-normalized counter via
// TscPerSec() returning 1e9 so Ms()/Nsf() still report wall time).
//
#include <cstddef>
#include <cstdint>
#include <fstream>

#if defined(_WIN32)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#define MEMPDB_HAVE_RDTSC 1
#else
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <x86intrin.h>
#define MEMPDB_HAVE_RDTSC 1
#else
#define MEMPDB_HAVE_RDTSC 0
#endif
#endif

namespace bench
{
#if MEMPDB_HAVE_RDTSC
    inline uint64_t RdtscStart() noexcept
    {
        _mm_lfence();
        const uint64_t t = __rdtsc();
        _mm_lfence();
        return t;
    }

    inline uint64_t RdtscEnd() noexcept
    {
        unsigned aux;
        const uint64_t t = __rdtscp(&aux);
        _mm_lfence();
        return t;
    }
#else
    inline uint64_t RdtscStart() noexcept
    {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull
             + static_cast<uint64_t>(ts.tv_nsec);
    }

    inline uint64_t RdtscEnd() noexcept { return RdtscStart(); }
#endif

#if defined(_WIN32)
    inline double TscPerSec() noexcept
    {
        LARGE_INTEGER qf;
        QueryPerformanceFrequency(&qf);

        const HANDLE thread = GetCurrentThread();
        const DWORD_PTR oldMask = SetThreadAffinityMask(thread, 1);

        LARGE_INTEGER q0;
        QueryPerformanceCounter(&q0);
        const uint64_t t0 = RdtscStart();

        const double targetTicks = static_cast<double>(qf.QuadPart) * 0.20; // ~200 ms
        LARGE_INTEGER q1;
        do
        {
            QueryPerformanceCounter(&q1);
        } while (static_cast<double>(q1.QuadPart - q0.QuadPart) < targetTicks);

        const uint64_t t1 = RdtscEnd();

        if (oldMask) SetThreadAffinityMask(thread, oldMask);

        const double secs =
            static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(qf.QuadPart);
        return static_cast<double>(t1 - t0) / secs;
    }

    inline std::size_t WorkingSetBytes() noexcept
    {
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            return pmc.WorkingSetSize;
        return 0;
    }
#else
    inline double MonoSecs() noexcept
    {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<double>(ts.tv_sec)
             + static_cast<double>(ts.tv_nsec) * 1e-9;
    }

    inline double TscPerSec() noexcept
    {
#if !MEMPDB_HAVE_RDTSC
        return 1e9; // Rdtsc* returns nanoseconds directly
#else
        cpu_set_t oldSet;
        cpu_set_t newSet;
        CPU_ZERO(&oldSet);
        CPU_ZERO(&newSet);
        CPU_SET(0, &newSet);
        const bool pinned =
            pthread_getaffinity_np(pthread_self(), sizeof(oldSet), &oldSet) == 0
            && pthread_setaffinity_np(pthread_self(), sizeof(newSet), &newSet) == 0;

        const double t0Wall = MonoSecs();
        const uint64_t t0 = RdtscStart();

        double t1Wall;
        do
        {
            t1Wall = MonoSecs();
        } while ((t1Wall - t0Wall) < 0.20);

        const uint64_t t1 = RdtscEnd();

        if (pinned)
            pthread_setaffinity_np(pthread_self(), sizeof(oldSet), &oldSet);

        return static_cast<double>(t1 - t0) / (t1Wall - t0Wall);
#endif
    }

    inline std::size_t WorkingSetBytes() noexcept
    {
        std::ifstream f("/proc/self/statm");
        if (!f) return 0;
        std::size_t sizePages = 0;
        std::size_t rssPages  = 0;
        f >> sizePages >> rssPages;
        if (!f) return 0;
        const long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) return 0;
        return rssPages * static_cast<std::size_t>(page);
    }
#endif
}
