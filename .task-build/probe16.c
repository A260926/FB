#include <stdio.h>
#include "buffer_atomic_contract.c"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

int main(void) {
    ensure_allocator();
    size_t prev = 0;
    for (unsigned n = 1; n <= 80; ++n) {
        struct buffer_model_result r = buffer_model_run(83, n);
        size_t live = buffer_test_live_allocations();
        if (live != prev)
            printf("n=%u live=%zu failed=%d field=%s\n", n, live,
                r.failed, r.failed ? r.field : "-");
        prev = live;
        if (r.failed) break;
    }
    puts("DONE83");
    return 0;
}
