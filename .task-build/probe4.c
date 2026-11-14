#include <stdio.h>
#include "buffer_atomic_contract.c"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

int main(void) {
    ensure_allocator();
    for (unsigned n = 1; n <= 80; ++n) {
        size_t before = buffer_test_live_allocations();
        struct buffer_model_result r = buffer_model_run(7, n);
        size_t after = buffer_test_live_allocations();
        if (r.failed) {
            printf("n=%u MODEL FAIL op=%u field=%s\n", n, r.op_index, r.field);
            break;
        }
        if (after != before)
            printf("n=%u LEAK delta=%zd (before=%zu after=%zu)\n",
                n, (long)after - (long)before, before, after);
    }
    puts("BISECT_DONE");
    return 0;
}
