// Compatibility adapter: glibc dprintf may allocate from the modeled user heap.
// Use the checker-owned allocator without weakening its user-context checks.
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include "mymemory.h"
extern "C" int task_dprintf(int fd, const char *format, ...) {
    char local[1024];
    va_list ap, copy;
    va_start(ap, format);
    va_copy(copy, ap);
    int count = vsnprintf(local, sizeof local, format, ap);
    va_end(ap);
    if (count < 0) { va_end(copy); return count; }
    char *buffer = local;
    if ((size_t)count >= sizeof local) {
        buffer = (char *)model_malloc((size_t)count + 1);
        if (!buffer) { va_end(copy); errno = ENOMEM; return -1; }
        if (vsnprintf(buffer, (size_t)count + 1, format, copy) != count) {
            va_end(copy); model_free(buffer); errno = EIO; return -1;
        }
    }
    va_end(copy);
    size_t offset = 0;
    while (offset < (size_t)count) {
        ssize_t wrote = write(fd, buffer + offset, (size_t)count - offset);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) {
            if (buffer != local) model_free(buffer);
            return -1;
        }
        offset += (size_t)wrote;
    }
    if (buffer != local) model_free(buffer);
    return count;
}
