#include <stdio.h>
#include "buffer_atomic_contract.c"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

int main(void) {
    ensure_allocator();
    faults_enabled = 0; /* run the identical seed with every op succeeding */
    size_t before = buffer_test_live_allocations();
    struct buffer_model_result r = buffer_model_run(7, 80);
    size_t after = buffer_test_live_allocations();
    printf("NOFAULT failed=%d field=%s live_before=%zu live_after=%zu\n",
        r.failed, r.failed ? r.field : "-", before, after);
    return r.failed ? 1 : 0;
}
