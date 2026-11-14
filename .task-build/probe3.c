#include <event2/buffer.h>
#include <event2/event.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct blk { unsigned char *p; size_t n; int id; int calls; };
static void cu(const void *p, size_t n, void *a) {
    struct blk *b = a;
    if (p != (void*)b->p || n != b->n) { printf("BAD ARGS id=%d\n", b->id); exit(9); }
    b->calls++;
    free(b->p); b->p = NULL;
}
static void mk(struct blk *b, int id, size_t n) {
    b->p = malloc(n); b->n = n; b->id = id; b->calls = 0;
    memset(b->p, 'a' + id, n);
}
static int total_calls(struct blk *bs, int n) { int s=0; for (int i=0;i<n;i++) s+=bs[i].calls; return s; }

/* Sequence: reference block into src; share src into view; transfer part of
 * src into dst (whole-chain move because block fits); free all. */
static void test_whole_transfer_after_share(void) {
    struct blk b; mk(&b, 1, 6);
    struct evbuffer *src = evbuffer_new(), *view = evbuffer_new(), *dst = evbuffer_new();
    evbuffer_add_reference(src, b.p, b.n, cu, &b);
    evbuffer_add_buffer_reference(view, src);
    int r = evbuffer_remove_buffer(src, dst, 6); /* whole chain */
    printf("whole-after-share r=%d src=%zu dst=%zu view=%zu calls=%d\n",
        r, evbuffer_get_length(src), evbuffer_get_length(dst),
        evbuffer_get_length(view), b.calls);
    evbuffer_free(src); evbuffer_free(view); evbuffer_free(dst);
    printf("  after free calls=%d (expect 1)\n", b.calls);
    if (b.calls != 1) { printf("LEAK/DOUBLE whole-after-share\n"); }
    if (b.calls == 0) free(b.p);
}

/* Partial transfer: block in src, request stops mid-chain (needs tail copy). */
static void test_partial_transfer(void) {
    struct blk b; mk(&b, 2, 8);
    struct evbuffer *src = evbuffer_new(), *view = evbuffer_new(), *dst = evbuffer_new();
    evbuffer_add_reference(src, b.p, b.n, cu, &b);
    evbuffer_add_buffer_reference(view, src);
    int r = evbuffer_remove_buffer(src, dst, 3); /* fragment copy */
    printf("partial r=%d src=%zu dst=%zu view=%zu calls=%d\n",
        r, evbuffer_get_length(src), evbuffer_get_length(dst),
        evbuffer_get_length(view), b.calls);
    evbuffer_free(src); evbuffer_free(view); evbuffer_free(dst);
    printf("  after free calls=%d (expect 1)\n", b.calls);
    if (b.calls != 1) printf("LEAK/DOUBLE partial\n");
    if (b.calls == 0) free(b.p);
}

/* Two blocks, share, transfer spanning both then free in mixed order. */
static void test_two_blocks(void) {
    struct blk bs[2]; mk(&bs[0], 3, 4); mk(&bs[1], 4, 5);
    struct evbuffer *src = evbuffer_new(), *view = evbuffer_new(), *dst = evbuffer_new();
    evbuffer_add_reference(src, bs[0].p, bs[0].n, cu, &bs[0]);
    evbuffer_add_reference(src, bs[1].p, bs[1].n, cu, &bs[1]);
    evbuffer_add_buffer_reference(view, src);
    int r = evbuffer_remove_buffer(src, dst, 6); /* whole 4 + 2 fragment */
    printf("two r=%d src=%zu dst=%zu view=%zu calls=%d\n",
        r, evbuffer_get_length(src), evbuffer_get_length(dst),
        evbuffer_get_length(view), total_calls(bs, 2));
    evbuffer_free(dst);
    evbuffer_free(src);
    evbuffer_free(view);
    printf("  after free calls=%d (expect 2) [%d,%d]\n",
        total_calls(bs, 2), bs[0].calls, bs[1].calls);
    for (int i=0;i<2;i++) if (bs[i].calls == 0) free(bs[i].p);
}

int main(void) {
    test_whole_transfer_after_share();
    test_partial_transfer();
    test_two_blocks();
    puts("DONE");
    return 0;
}
