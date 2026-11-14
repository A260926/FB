#include <stdio.h>
#include "buffer_atomic_contract.c"
#include "evbuffer-internal.h"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

int main(void) {
    ensure_allocator();
    struct buffer_model_result r = buffer_model_run(83, 80);
    printf("seed83 failed=%d field=%s live=%zu\n", r.failed,
        r.failed ? r.field : "-", buffer_test_live_allocations());
    return 0;
}
