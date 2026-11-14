#include <stdio.h>
#include "buffer_atomic_contract.c"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

int main(void) {
    ensure_allocator();
    unsigned fails = 0;
    for (unsigned s = 1; s <= 2000; ++s) {
        struct buffer_model_result r = buffer_model_run(s * 2654435761u, 80);
        if (r.failed) {
            printf("FAIL seed=%u op=%u field=%s\n", s, r.op_index, r.field);
            if (++fails > 10) break;
        }
    }
    printf(fails ? "SWEEP_FOUND_FAILURES\n" : "SWEEP_2000_OK\n");
    return fails ? 1 : 0;
}
