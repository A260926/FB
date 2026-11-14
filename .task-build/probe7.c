#include <stdio.h>
#include "buffer_atomic_contract.c"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

int main(void) {
    ensure_allocator();
    faults_enabled = 0;
    size_t prev = 0;
    for (unsigned n = 1; n <= 80; ++n) {
        struct buffer_model_result r = buffer_model_run(7, n);
        size_t live = buffer_test_live_allocations();
        if (live != prev)
            printf("n=%u live=%zu (delta %zd) failed=%d\n", n, live,
                (long)live - (long)prev, r.failed);
        prev = live;
    }
    puts("BISECT2_DONE");
    return 0;
}
