#pragma once
#ifdef __APPLE__
#include <cstdlib>
inline void *moc_posix_memalign(size_t alignment, size_t size) {
    void *p = nullptr;
    return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
}
#define memalign moc_posix_memalign
#endif
