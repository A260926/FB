#include <stdio.h>
#include "buffer_atomic_contract.c"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

int main(void) {
    ensure_allocator();
    for (unsigned s = 1; s <= 128; ++s) {
        struct buffer_model_result r = buffer_model_run(s, 80);
        if (r.failed)
            printf("seed=%u op=%u field=%s\n", s, r.op_index, r.field);
    }
    puts("SWEEP_DONE");
    return 0;
}
