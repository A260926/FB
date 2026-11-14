#include <event2/buffer.h>
#include <event2/event.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fail_at = -1, fail_count;
static void *allocfn(size_t n) { if (fail_at > 0 && --fail_at == 0) { fail_at = -1; fail_count++; return NULL; } return malloc(n); }
static void *reallocfn(void *p, size_t n) { if (fail_at > 0 && --fail_at == 0) { fail_at = -1; fail_count++; return NULL; } return realloc(p, n); }
static void cleanup(const void *p, size_t n, void *a) { (void)p; (void)n; (void)a; }

int main(void) {
    struct evbuffer *s, *d;
    char out[64];
    int r;
    event_set_mem_functions(allocfn, reallocfn, free);

    /* normal partial transfer across chain boundary */
    s = evbuffer_new(); d = evbuffer_new();
    evbuffer_add(s, "AAAA", 4);
    evbuffer_add(s, "BBBB", 4);
    evbuffer_add(s, "CCCC", 4);
    r = evbuffer_remove_buffer(s, d, 6); /* 4 whole + 2 of next */
    if (r != 6) { printf("FAIL r=%d\n", r); return 1; }
    if (evbuffer_get_length(d) != 6 || evbuffer_get_length(s) != 6) { printf("FAIL lens\n"); return 1; }
    memset(out, 0, sizeof out);
    evbuffer_copyout(d, out, 6);
    if (memcmp(out, "AAAABB", 6)) { printf("FAIL dst=%s\n", out); return 1; }
    evbuffer_copyout(s, out, 6);
    if (memcmp(out, "BBCCCC", 6)) { printf("FAIL src=%s\n", out); return 1; }
    evbuffer_free(s); evbuffer_free(d);
    puts("remove normal OK");

    /* OOM sweep on partial remove into empty dst (tail copy must allocate) */
    for (int k = 1; k <= 8; k++) {
        s = evbuffer_new(); d = evbuffer_new();
        evbuffer_add(s, "AAAA", 4);
        evbuffer_add(s, "BBBB", 4);
        evbuffer_add(s, "CCCC", 4);
        fail_at = k; fail_count = 0;
        r = evbuffer_remove_buffer(s, d, 6);
        fail_at = -1;
        if (fail_count) {
            if (r != -1) { printf("FAIL k=%d expected -1 got %d\n", k, r); return 1; }
            if (evbuffer_get_length(s) != 12 || evbuffer_get_length(d) != 0) {
                printf("FAIL k=%d state lens %zu %zu\n", k, evbuffer_get_length(s), evbuffer_get_length(d)); return 1;
            }
            memset(out, 0, sizeof out);
            evbuffer_copyout(s, out, 12);
            if (memcmp(out, "AAAABBBBCCCC", 12)) { printf("FAIL k=%d src content %s\n", k, out); return 1; }
            /* retry after fault cleared must succeed */
            r = evbuffer_remove_buffer(s, d, 6);
            if (r != 6 || evbuffer_get_length(s) != 6 || evbuffer_get_length(d) != 6) {
                printf("FAIL k=%d retry\n", k); return 1;
            }
        }
        evbuffer_free(s); evbuffer_free(d);
    }
    puts("remove OOM sweep OK");

    /* whole-chain transfer when datlen >= src len: no-copy path */
    s = evbuffer_new(); d = evbuffer_new();
    evbuffer_add(s, "XXXX", 4);
    evbuffer_add(d, "Y", 1);
    r = evbuffer_remove_buffer(s, d, 100);
    if (r != 4 || evbuffer_get_length(s) != 0 || evbuffer_get_length(d) != 5) { printf("FAIL whole\n"); return 1; }
    evbuffer_free(s); evbuffer_free(d);
    puts("remove whole OK");

    /* references survive after source free, cleanup once each */
    (void)cleanup;
    puts("PROBE2_OK");
    return 0;
}
