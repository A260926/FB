#include <stdio.h>
#include "buffer_atomic_contract.c"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

int main(void) {
    ensure_allocator();
    struct buffer_model_result r = buffer_model_run(40, 80);
    printf("seed40 failed=%d op=%u field=%s model=%zu real=%zu\n",
        r.failed, r.op_index, r.failed ? r.field : "-",
        r.model_value, r.real_value);
    return r.failed ? 1 : 0;
}
