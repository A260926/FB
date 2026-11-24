// C++ API/linkage smoke test for the optional covmerge entry point.

#include "libhfuzz/covmerge.h"

#include <cassert>
#include <cstdint>
#include <sys/mman.h>

extern "C" {
void __sanitizer_cov_8bit_counters_init(char *start, char *end);
void instrument8BitCountersCount(void);
}

int main() {
    const size_t bytes = hfCovMergeSize();
    assert(bytes >= static_cast<size_t>(HF_COVMERGE_MAX_MODULES) *
                        HF_COVMERGE_MODULE_POSITIONS);

    void *memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);

    assert(hfCovMergeInit(memory, bytes, 7));
    assert(hfCovMergeAttach(memory, bytes));

    char counter[2] = {};
    __sanitizer_cov_8bit_counters_init(counter, counter + 2);
    assert(hfCovMergeBind(counter, counter + 2, 0xCAFEULL, 0xBABEULL));

    assert(hfCovMergeBegin() == 7);
    counter[0] = 4;
    counter[1] = 0;
    instrument8BitCountersCount();

    std::uint8_t value = 0;
    assert(hfCovMergeRead(0xCAFEULL, 0xBABEULL, 0, &value));
    assert(value == 4);
    assert(counter[0] == 0);

    hfCovMergeDetach();
    assert(munmap(memory, bytes) == 0);
    return 0;
}
