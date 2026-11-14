#include <stdio.h>
#include "buffer_atomic_contract.c"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

static void dump(const char *tag, struct evbuffer *b) {
    struct evbuffer_chain *c;
    int i = 0;
    printf("%s total=%zu first=%p last=%p lwd=%p\n", tag, b->total_len,
        (void*)b->first, (void*)b->last, (void*)b->last_with_datap);
    for (c = b->first; c; c = c->next) {
        printf("  chain[%d]=%p next=%p off=%zu flags=0x%x refcnt=%d buf=%p\n",
            i++, (void*)c, (void*)c->next, c->off, c->flags, c->refcnt,
            (void*)c->buffer);
        if (i > 40) { printf("  ...runaway\n"); break; }
    }
}

int main(void) {
    ensure_allocator();
    struct buffer_model_result r = buffer_model_run(7, 43);
    (void)r;
    puts("DUMP_DONE");
    return 0;
}
