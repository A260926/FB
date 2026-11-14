#include <stdio.h>
#include "buffer_atomic_contract.c"

int task_check(int value, const char *expr, const char *file, int line) {
    (void)expr; (void)file; (void)line;
    return value;
}

static void refs(const char *t, struct evbuffer *b) {
    printf("%s %p refcnt=%d len=%zu\n", t, (void*)b, b->refcnt, b->total_len);
}

int main(void) {
    ensure_allocator();
    size_t start = buffer_test_live_allocations();

    /* src holds an external reference block */
    struct ext_block blk;
    block_init(&blk, 1, 6);
    struct evbuffer *src = evbuffer_new();
    evbuffer_add_reference(src, blk.data, blk.len, block_cleanup, &blk);

    struct evbuffer *view = evbuffer_new();
    struct evbuffer *sink = evbuffer_new();

    evbuffer_add_buffer_reference(view, src);   /* multicast chain in view, source=src */
    refs("after share src", src);
    refs("view", view);

    int r = evbuffer_remove_buffer(view, sink, evbuffer_get_length(view));
    printf("remove_buffer r=%d view=%zu sink=%zu\n", r,
        evbuffer_get_length(view), evbuffer_get_length(sink));
    refs("src after transfer", src);
    refs("sink", sink);

    evbuffer_free(view);
    refs("src after free view", src);
    evbuffer_free(sink);
    refs("src after free sink", src);
    evbuffer_free(src);
    printf("cleanup_calls=%d live=%zu (start=%zu)\n", blk.cleanup_calls,
        buffer_test_live_allocations(), start);
    block_discard_unused(&blk);
    return blk.cleanup_calls == 1 && buffer_test_live_allocations() == start ? 0 : 1;
}
