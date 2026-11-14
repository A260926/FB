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
    struct ext_block blk;
    block_init(&blk, 1, 6);

    struct evbuffer *src = evbuffer_new();   /* buf1 */
    struct evbuffer *view = evbuffer_new();  /* buf4 */
    evbuffer_add_reference(src, blk.data, blk.len, block_cleanup, &blk);

    /* share src -> view: view holds multicast chain, extra->source = src */
    evbuffer_add_buffer_reference(view, src);
    refs("src after share", src);

    /* transfer the multicast chain back into src itself */
    int r = evbuffer_remove_buffer(view, src, evbuffer_get_length(view));
    printf("transfer back r=%d view=%zu src=%zu\n", r,
        evbuffer_get_length(view), evbuffer_get_length(src));
    refs("src after transfer back", src);

    evbuffer_free(view);
    refs("src after free view", src);
    evbuffer_free(src);
    printf("cleanup=%d live=%zu start=%zu\n", blk.cleanup_calls,
        buffer_test_live_allocations(), start);
    block_discard_unused(&blk);
    return 0;
}
